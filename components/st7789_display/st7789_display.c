#include "st7789_display.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PANEL_WIDTH 240
#define PANEL_HEIGHT 320
#define TRANSFER_LINES 16
#define MAX_WIDTH PANEL_HEIGHT
#define PWM_MAX_DUTY 1023u

static const char *TAG = "st7789_display";

struct st7789_display {
    st7789_display_config_t config;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t transfer_done;
    uint16_t *dma_buffer;
    int width;
    int height;
    bool bus_owned;
    bool pwm_ready;
};

static bool color_transfer_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    struct st7789_display *display = context;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(display->transfer_done, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t validate_config(const st7789_display_config_t *config)
{
    if (!config || (config->spi_host != SPI2_HOST && config->spi_host != SPI3_HOST) ||
        config->clock_hz < 1000000 || config->clock_hz > 40000000 ||
        (config->rotation != 0 && config->rotation != 90 &&
         config->rotation != 180 && config->rotation != 270)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int pins[] = {config->sclk_gpio, config->mosi_gpio, config->dc_gpio,
                        config->cs_gpio, config->reset_gpio, config->backlight_gpio};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] == -1 && i >= 3) {
            continue;
        }
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pins[i] == pins[j]) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t init_backlight(struct st7789_display *display)
{
    if (display->config.backlight_gpio < 0) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "configure backlight PWM timer");
    const ledc_channel_config_t channel = {
        .gpio_num = display->config.backlight_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = display->config.backlight_active_low ? PWM_MAX_DUTY : 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "configure backlight PWM output");
    display->pwm_ready = true;
    return ESP_OK;
}

/* RGB565 crosses SPI most-significant byte first, independently of CPU storage. */
static uint16_t wire_rgb565(uint16_t color)
{
    return __builtin_bswap16(color);
}

/* A single staging buffer is reused only after its DMA completion callback. */
static esp_err_t transfer_block(struct st7789_display *display,
                                int x, int y, int width, int height)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(display->panel, x, y,
                         x + width, y + height, display->dma_buffer), TAG, "send pixel block");
    xSemaphoreTake(display->transfer_done, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t st7789_display_create(const st7789_display_config_t *config,
                               st7789_display_handle_t *out_display)
{
    if (!out_display) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_display = NULL;
    ESP_RETURN_ON_ERROR(validate_config(config), TAG, "invalid display configuration");
    esp_err_t ret = ESP_OK;
    struct st7789_display *display = calloc(1, sizeof(*display));
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "allocate display");
    display->config = *config;
    const bool swapped = config->rotation == 90 || config->rotation == 270;
    display->width = swapped ? PANEL_HEIGHT : PANEL_WIDTH;
    display->height = swapped ? PANEL_WIDTH : PANEL_HEIGHT;
    display->mutex = xSemaphoreCreateMutex();
    display->transfer_done = xSemaphoreCreateBinary();
    display->dma_buffer = heap_caps_malloc(MAX_WIDTH * TRANSFER_LINES * sizeof(uint16_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(display->mutex && display->transfer_done && display->dma_buffer,
                      ESP_ERR_NO_MEM, fail, TAG, "allocate DMA buffer and synchronization");
    ESP_GOTO_ON_ERROR(init_backlight(display), fail, TAG, "initialize backlight");

    const spi_bus_config_t bus = {
        .sclk_io_num = config->sclk_gpio,
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_WIDTH * TRANSFER_LINES * sizeof(uint16_t),
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(config->spi_host, &bus, SPI_DMA_CH_AUTO),
                      fail, TAG, "initialize dedicated SPI bus");
    display->bus_owned = true;
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = config->cs_gpio,
        .dc_gpio_num = config->dc_gpio,
        .spi_mode = 0,
        .pclk_hz = config->clock_hz,
        .trans_queue_depth = 1,
        .on_color_trans_done = color_transfer_done,
        .user_ctx = display,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(config->spi_host, &io, &display->io),
                      fail, TAG, "create panel SPI transport");
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = config->reset_gpio,
        .rgb_ele_order = config->bgr_order ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(display->io, &panel, &display->panel),
                      fail, TAG, "create ST7789 controller");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(display->panel), fail, TAG, "reset panel");
    /* Allow power/reset recovery before the first controller command. */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(display->panel), fail, TAG, "initialize panel");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(display->panel, config->invert_colors),
                      fail, TAG, "configure inversion");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(display->panel, swapped), fail, TAG, "configure axes");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(display->panel,
                      config->rotation == 90 || config->rotation == 180,
                      config->rotation == 180 || config->rotation == 270),
                      fail, TAG, "configure orientation");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(display->panel, 0, 0),
                      fail, TAG, "configure address offset");
    ESP_GOTO_ON_ERROR(st7789_display_clear(display, 0), fail, TAG, "clear initial frame");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(display->panel, true), fail, TAG, "enable display");
    if (display->pwm_ready) {
        ESP_GOTO_ON_ERROR(st7789_display_set_brightness(display, 100), fail, TAG, "enable backlight");
    }
    ESP_LOGI(TAG, "%dx%d, rotation=%u, SPI=%u Hz, SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d, "
             "invert=%d bgr=%d",
             display->width, display->height, config->rotation, config->clock_hz,
             config->sclk_gpio, config->mosi_gpio, config->cs_gpio, config->dc_gpio,
             config->reset_gpio, config->backlight_gpio,
             (int)config->invert_colors, (int)config->bgr_order);
    *out_display = display;
    return ESP_OK;
fail:
    st7789_display_destroy(display);
    return ret;
}

void st7789_display_destroy(st7789_display_handle_t display)
{
    if (!display) {
        return;
    }
    if (display->pwm_ready) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  display->config.backlight_active_low ? 1 : 0);
    }
    if (display->panel) {
        esp_lcd_panel_disp_on_off(display->panel, false);
        esp_lcd_panel_del(display->panel);
    }
    if (display->io) {
        esp_lcd_panel_io_del(display->io);
    }
    if (display->bus_owned) {
        spi_bus_free(display->config.spi_host);
    }
    if (display->config.backlight_gpio >= 0 && display->pwm_ready) {
        gpio_reset_pin(display->config.backlight_gpio);
    }
    if (display->mutex) {
        vSemaphoreDelete(display->mutex);
    }
    if (display->transfer_done) {
        vSemaphoreDelete(display->transfer_done);
    }
    free(display->dma_buffer);
    free(display);
}

int st7789_display_width(st7789_display_handle_t display)
{
    return display ? display->width : 0;
}

int st7789_display_height(st7789_display_handle_t display)
{
    return display ? display->height : 0;
}

esp_err_t st7789_display_fill_rect(st7789_display_handle_t display,
                                  int x, int y, int width, int height, uint16_t color)
{
    if (!display || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    right = right > display->width ? display->width : right;
    bottom = bottom > display->height ? display->height : bottom;
    if (x >= right || y >= bottom) {
        return ESP_OK;
    }
    width = (int)(right - x);
    height = (int)(bottom - y);
    xSemaphoreTake(display->mutex, portMAX_DELAY);
    const uint16_t wire_color = wire_rgb565(color);
    for (int i = 0; i < width * TRANSFER_LINES; ++i) {
        display->dma_buffer[i] = wire_color;
    }
    esp_err_t ret = ESP_OK;
    for (int row = 0; row < height && ret == ESP_OK; row += TRANSFER_LINES) {
        int lines = height - row < TRANSFER_LINES ? height - row : TRANSFER_LINES;
        ret = transfer_block(display, x, y + row, width, lines);
    }
    xSemaphoreGive(display->mutex);
    return ret;
}

esp_err_t st7789_display_clear(st7789_display_handle_t display, uint16_t color)
{
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }
    return st7789_display_fill_rect(display, 0, 0, display->width, display->height, color);
}

esp_err_t st7789_display_draw_rgb565(st7789_display_handle_t display,
                                    int x, int y, int width, int height,
                                    const uint16_t *pixels, size_t stride_pixels)
{
    if (!display || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        (int64_t)x + width > display->width || (int64_t)y + height > display->height ||
        stride_pixels < (size_t)width ||
        stride_pixels > (SIZE_MAX / sizeof(uint16_t) - (size_t)width) / (size_t)height) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(display->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    for (int row = 0; row < height && ret == ESP_OK; row += TRANSFER_LINES) {
        const int lines = height - row < TRANSFER_LINES ? height - row : TRANSFER_LINES;
        for (int line = 0; line < lines; ++line) {
            const uint16_t *source = pixels + (size_t)(row + line) * stride_pixels;
            for (int col = 0; col < width; ++col) {
                display->dma_buffer[line * width + col] = wire_rgb565(source[col]);
            }
        }
        ret = transfer_block(display, x, y + row, width, lines);
    }
    xSemaphoreGive(display->mutex);
    return ret;
}

esp_err_t st7789_display_set_brightness(st7789_display_handle_t display, unsigned int percent)
{
    if (!display || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->pwm_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    unsigned int duty = PWM_MAX_DUTY * percent / 100;
    if (display->config.backlight_active_low) {
        duty = PWM_MAX_DUTY - duty;
    }
    xSemaphoreTake(display->mutex, portMAX_DELAY);
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (ret == ESP_OK) {
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    xSemaphoreGive(display->mutex);
    return ret;
}
