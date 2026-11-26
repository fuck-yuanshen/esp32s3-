#include "lvgl_port.h"

#include <string.h>
#include "esp_check.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_port_keys.h"

/* Do not sleep longer than this, so key polling and animations stay responsive. */
#define MAX_IDLE_MS 30
#define MIN_WAIT_MS 5

static const char *TAG = "lvgl_port";

typedef struct {
    st7789_display_handle_t display;
    lv_display_t *lv_display;
    lv_indev_t *keypad;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    uint16_t *buffer;
    uint32_t buffer_bytes;
    bool lvgl_ready;
    uint32_t flush_count;
    uint32_t flush_max_us;
} lvgl_port_t;

static lvgl_port_t s_port;

static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* LVGL calls this from the handler task; the driver blocks until DMA is done. */
static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    const int width = (int)lv_area_get_width(area);
    const int height = (int)lv_area_get_height(area);
    /* Partial-mode rows are contiguous, but respect a padded draw-buffer stride. */
    const lv_draw_buf_t *active = lv_display_get_buf_active(display);
    size_t stride_pixels = (size_t)width;
    if (active && active->header.stride >= (uint32_t)width * sizeof(uint16_t)) {
        stride_pixels = active->header.stride / sizeof(uint16_t);
    }
    const int64_t start_us = esp_timer_get_time();
    const esp_err_t ret = st7789_display_draw_rgb565(s_port.display, (int)area->x1, (int)area->y1,
                                                     width, height, (const uint16_t *)px_map,
                                                     stride_pixels);
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
    if (elapsed_us > s_port.flush_max_us) {
        s_port.flush_max_us = elapsed_us;
    }
    s_port.flush_count++;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flush %dx%d at (%d,%d) failed: %s", width, height, (int)area->x1,
                 (int)area->y1, esp_err_to_name(ret));
    }
    lv_display_flush_ready(display);
}

static void lvgl_task(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t wait_ms = MAX_IDLE_MS;
        if (lvgl_port_lock(LVGL_PORT_WAIT_FOREVER)) {
            wait_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > MAX_IDLE_MS) {
            wait_ms = MAX_IDLE_MS;
        }
        if (wait_ms < MIN_WAIT_MS) {
            wait_ms = MIN_WAIT_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t lvgl_port_init(const lvgl_port_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config && config->display, ESP_ERR_INVALID_ARG, TAG,
                        "display handle is required");
    ESP_RETURN_ON_FALSE(!s_port.lv_display, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    const int width = st7789_display_width(config->display);
    const int height = st7789_display_height(config->display);
    ESP_RETURN_ON_FALSE(width > 0 && height > 0, ESP_ERR_INVALID_STATE, TAG,
                        "display size is unknown");

    s_port.display = config->display;
    unsigned int lines = config->buffer_lines ? config->buffer_lines : CONFIG_LVGL_PORT_BUFFER_LINES;
    if (lines > (unsigned int)height) {
        lines = (unsigned int)height;
    }
    s_port.buffer_bytes = (uint32_t)lines * (uint32_t)width * sizeof(uint16_t);

    s_port.mutex = xSemaphoreCreateRecursiveMutex();
    ESP_GOTO_ON_FALSE(s_port.mutex, ESP_ERR_NO_MEM, fail, TAG, "allocate LVGL mutex");
    s_port.buffer = heap_caps_malloc(s_port.buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(s_port.buffer, ESP_ERR_NO_MEM, fail, TAG,
                      "allocate %u byte draw buffer", (unsigned int)s_port.buffer_bytes);

    lv_init();
    s_port.lvgl_ready = true;
    lv_tick_set_cb(tick_get_cb);

    s_port.lv_display = lv_display_create((int32_t)width, (int32_t)height);
    ESP_GOTO_ON_FALSE(s_port.lv_display, ESP_ERR_NO_MEM, fail, TAG, "create LVGL display");
    lv_display_set_color_format(s_port.lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_port.lv_display, flush_cb);
    /* Flushing blocks until the SPI transfer completes, so one buffer is enough. */
    lv_display_set_buffers(s_port.lv_display, s_port.buffer, NULL, s_port.buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (config->buttons) {
        ESP_GOTO_ON_ERROR(lvgl_port_keys_init(config->buttons), fail, TAG,
                          "create keypad input device");
        s_port.keypad = lvgl_port_keys_indev();
    }

    ESP_LOGI(TAG, "LVGL %d.%d on %dx%d, %u-line buffer (%u bytes), input=%s",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, width, height, lines,
             (unsigned int)s_port.buffer_bytes, s_port.keypad ? "keypad" : "none");
    return ESP_OK;

fail:
    lvgl_port_deinit();
    return ret;
}

esp_err_t lvgl_port_start(void)
{
    ESP_RETURN_ON_FALSE(s_port.lv_display, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(s_port.task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    BaseType_t created;
    if (CONFIG_LVGL_PORT_TASK_CORE >= 0) {
        created = xTaskCreatePinnedToCore(lvgl_task, "lvgl", CONFIG_LVGL_PORT_TASK_STACK, NULL,
                                          CONFIG_LVGL_PORT_TASK_PRIORITY, &s_port.task,
                                          CONFIG_LVGL_PORT_TASK_CORE);
    } else {
        created = xTaskCreate(lvgl_task, "lvgl", CONFIG_LVGL_PORT_TASK_STACK, NULL,
                              CONFIG_LVGL_PORT_TASK_PRIORITY, &s_port.task);
    }
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create LVGL task");
    ESP_LOGI(TAG, "task started: priority=%u stack=%u core=%d",
             (unsigned int)CONFIG_LVGL_PORT_TASK_PRIORITY,
             (unsigned int)CONFIG_LVGL_PORT_TASK_STACK, CONFIG_LVGL_PORT_TASK_CORE);
    return ESP_OK;
}

void lvgl_port_deinit(void)
{
    if (s_port.task) {
        vTaskDelete(s_port.task);
        s_port.task = NULL;
    }
    lvgl_port_keys_deinit();
    s_port.keypad = NULL;
    if (s_port.lv_display) {
        lv_display_delete(s_port.lv_display);
        s_port.lv_display = NULL;
    }
    if (s_port.lvgl_ready) {
        lv_deinit();
        s_port.lvgl_ready = false;
    }
    free(s_port.buffer);
    s_port.buffer = NULL;
    if (s_port.mutex) {
        vSemaphoreDelete(s_port.mutex);
        s_port.mutex = NULL;
    }
    memset(&s_port, 0, sizeof(s_port));
}

lv_display_t *lvgl_port_display(void)
{
    return s_port.lv_display;
}

lv_indev_t *lvgl_port_keypad(void)
{
    return s_port.keypad;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    if (!s_port.mutex) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == LVGL_PORT_WAIT_FOREVER) ? portMAX_DELAY
                                                                   : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_port.mutex, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (s_port.mutex) {
        xSemaphoreGiveRecursive(s_port.mutex);
    }
}

uint32_t lvgl_port_flush_count(void)
{
    return s_port.flush_count;
}

uint32_t lvgl_port_flush_max_us(void)
{
    return s_port.flush_max_us;
}
