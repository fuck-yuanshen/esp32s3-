/* Coordinates persistent BIN/HEX images, exclusive pin ownership and the SWD
 * target modules. The public stm32_isp API stays stable for the LVGL screens.
 * Only this worker touches the SWD bus; the UI polls a short mutex snapshot.
 */
#include "stm32_isp.h"
#include "stm32_image.h"
#include "stm32f1_flash.h"
#include "swd_gpio.h"
#include "uart_link.h"
#include "data_store.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

#define SWD_IMAGE_BLOCK 256U
static const char *TAG = "stm32_swd";
static SemaphoreHandle_t s_job_lock;
static bool s_initialized, s_busy;
static uint32_t s_base;
static bool s_verify;
static stm32_isp_status_t s_status;

static void publish(uint8_t percent, const char *step)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.percent = percent;
    strlcpy(s_status.message, step, sizeof(s_status.message));
    xSemaphoreGive(s_job_lock);
}
static void progress(uint32_t done, uint32_t total, bool verify)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.bytes_written = done;
    s_status.bytes_total = total;
    s_status.percent = (uint8_t)((verify ? 80 : 15) +
                         ((uint64_t)done * (verify ? 18 : 65)) / total);
    xSemaphoreGive(s_job_lock);
}

static esp_err_t program_image(const image_t *image, stm32f1_target_t *target,
                               bool verify, const char **stage)
{
    uint32_t first = image->low_address & ~(SWD_IMAGE_BLOCK - 1U);
    uint32_t end = (image->high_address + SWD_IMAGE_BLOCK - 1U) & ~(SWD_IMAGE_BLOCK - 1U);
    /* Validate every address before erasing a single page. Restrict the driver
     * to implemented user flash; peripheral/option/XL-bank addresses fail here.
     */
    *stage = "image outside target flash";
    for (size_t i = 0; i < image->count; ++i) {
        if (!stm32f1_range_valid(target, image->segments[i].address,
                                 image->segments[i].length)) return ESP_ERR_INVALID_SIZE;
    }
    if (end < first || !stm32f1_range_valid(target, first, end - first))
        return ESP_ERR_INVALID_SIZE;
    bool pages[256] = {0};
    const uint32_t page_count = target->flash_bytes / target->page_bytes;
    if (page_count > sizeof(pages) / sizeof(pages[0])) return ESP_ERR_NOT_SUPPORTED;
    for (size_t i = 0; i < image->count; ++i) {
        const image_segment_t *s = &image->segments[i];
        uint32_t lo = (s->address - STM32F1_FLASH_BASE) / target->page_bytes;
        uint32_t hi = (s->address + s->length - 1U - STM32F1_FLASH_BASE) / target->page_bytes;
        for (uint32_t p = lo; p <= hi; ++p) pages[p] = true;
    }
    uint8_t block[SWD_IMAGE_BLOCK];
    uint32_t total = 0;
    *stage = "reading firmware";
    for (uint32_t a = first; a < end; a += SWD_IMAGE_BLOCK) {
        bool present;
        esp_err_t e = image_read_block(image, a, block, sizeof(block), &present);
        if (e != ESP_OK) return e;
        if (present) total += SWD_IMAGE_BLOCK;
        vTaskDelay(1);
    }
    if (!total) return ESP_ERR_INVALID_SIZE;
    *stage = "unlock flash / check protection";
    esp_err_t e = stm32f1_unlock(target);
    if (e != ESP_OK) return e;
    *stage = "erasing pages";
    for (uint32_t p = 0; p < page_count; ++p) {
        if (!pages[p]) continue;
        publish((uint8_t)(5 + p * 10 / page_count), *stage);
        e = stm32f1_erase_page(target, STM32F1_FLASH_BASE + p * target->page_bytes);
        if (e != ESP_OK) return e;
        vTaskDelay(1);
    }
    for (unsigned pass = 0; pass < (verify ? 2U : 1U); ++pass) {
        uint32_t done = 0;
        *stage = pass ? "verify mismatch / read error" : "programming flash";
        publish(pass ? 80 : 15, pass ? "verifying" : "writing");
        for (uint32_t a = first; a < end; a += SWD_IMAGE_BLOCK) {
            bool present;
            e = image_read_block(image, a, block, sizeof(block), &present);
            if (e != ESP_OK) return e;
            if (present) {
                e = pass ? stm32f1_verify(target, a, block, sizeof(block))
                         : stm32f1_program(target, a, block, sizeof(block));
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "target address 0x%08lX: %s", (unsigned long)a, esp_err_to_name(e));
                    return e;
                }
                done += SWD_IMAGE_BLOCK;
                progress(done, total, pass != 0);
            }
            vTaskDelay(1);
        }
    }
    return ESP_OK;
}

static void log_connection(const swd_wire_t *wire, const arm_dap_t *dap,
                           const char *mode, esp_err_t ret)
{
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s, request=0x%02X ACK=0x%X DPIDR=0x%08lX: %s",
                 mode, dap->connect_step, (unsigned)wire->last_request,
                 (unsigned)wire->last_ack, (unsigned long)dap->idcode, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "%s: DP IDCODE 0x%08lX", mode, (unsigned long)dap->idcode);
    }
}

#if CONFIG_STM32_SWD_MANUAL_RESET

#define MANUAL_RESET_TIMEOUT_US INT64_C(30000000)
#define SWD_ATTACH_GRACE_US INT64_C(750000)

static void set_reset_prompt(stm32_isp_reset_prompt_t prompt)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.reset_prompt = prompt;
    xSemaphoreGive(s_job_lock);
}

esp_err_t stm32_isp_confirm_reset(stm32_isp_reset_prompt_t prompt)
{
    if (!s_job_lock || prompt == STM32_ISP_RESET_NONE) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    bool pending = s_status.state == STM32_ISP_RUNNING && s_status.reset_prompt == prompt;
    if (pending) s_status.reset_prompt = STM32_ISP_RESET_NONE;
    xSemaphoreGive(s_job_lock);
    return pending ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool reset_prompt_pending(void)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    bool pending = s_status.reset_prompt != STM32_ISP_RESET_NONE;
    xSemaphoreGive(s_job_lock);
    return pending;
}

static esp_err_t attach_and_prepare(swd_wire_t *wire, stm32f1_target_t *target,
                                     bool trace)
{
    swd_wire_reset(wire);
    if (trace) swd_gpio_trace_begin();
    esp_err_t ret = arm_dap_connect(target->dap);
    if (trace) swd_gpio_trace_end();
    if (ret != ESP_OK) return ret;
    target->dap->connect_step = "core reset catch";
    return stm32f1_prepare(target);
}

/* Catch reset while the button is held when the debug port permits it.
 * Continue probing across release for targets that respond only after reset.
 * Flash access is gated on the operator acknowledging release. */
static esp_err_t connect_target_manual(swd_wire_t *wire, stm32f1_target_t *target,
                                       const char **stage)
{
    *stage = "Timed out waiting for RESET hold";
    int64_t deadline = esp_timer_get_time() + MANUAL_RESET_TIMEOUT_US;
    set_reset_prompt(STM32_ISP_RESET_HOLD);
    ESP_LOGI(TAG, "Manual reset: hold STM32 RESET and press ESP32 OK");
    while (reset_prompt_pending()) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }

    swd_gpio_slow_down(wire);
    esp_err_t ret = attach_and_prepare(wire, target, true);
    log_connection(wire, target->dap, "Manual RESET held", ret);
    set_reset_prompt(STM32_ISP_RESET_RELEASE);
    ESP_LOGI(TAG, "Manual reset: release STM32 RESET and press ESP32 OK");
    deadline = esp_timer_get_time() + MANUAL_RESET_TIMEOUT_US;
    while (reset_prompt_pending()) {
        if (esp_timer_get_time() >= deadline) {
            *stage = "Timed out waiting for RESET release";
            return ESP_ERR_TIMEOUT;
        }
        if (ret != ESP_OK) ret = attach_and_prepare(wire, target, false);
        vTaskDelay(1);
    }

    *stage = "SWD connect / check PA13 PA14";
    if (ret != ESP_OK) ret = attach_and_prepare(wire, target, true);
    deadline = esp_timer_get_time() + SWD_ATTACH_GRACE_US;
    while (ret != ESP_OK && esp_timer_get_time() < deadline) {
        ret = attach_and_prepare(wire, target, false);
        if (ret != ESP_OK) vTaskDelay(1);
    }
    log_connection(wire, target->dap, "Manual RESET released", ret);
    return ret;
}
#elif CONFIG_STM32_SWD_WIRED_RESET
static esp_err_t connect_attempt(swd_wire_t *wire, arm_dap_t *dap, const char *mode)
{
    swd_wire_reset(wire);
    swd_gpio_trace_begin();
    esp_err_t ret = arm_dap_connect(dap);
    swd_gpio_trace_end();
    log_connection(wire, dap, mode, ret);
    return ret;
}

/* Try alternate link conditions before any flash access. Some targets answer
 * only after NRST is released. Identification still gates all flash writes. */
static esp_err_t connect_target(swd_wire_t *wire, arm_dap_t *dap, bool *reset_asserted)
{
    swd_gpio_reset(true);
    *reset_asserted = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t ret = connect_attempt(wire, dap, "NRST low");
    if (ret == ESP_OK) return ret;

    swd_gpio_slow_down(wire);
    ret = connect_attempt(wire, dap, "NRST low, slow clock");
    if (ret == ESP_OK) return ret;

    swd_gpio_reset(false);
    *reset_asserted = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return connect_attempt(wire, dap, "NRST released, slow clock");
}
#else
/* Attach directly to a running target when NRST is not wired. The SWD port
 * remains available on STM32F1 after reset, so the core can be halted and
 * prepared without an operator-controlled RESET pulse. */
static esp_err_t connect_target_auto(swd_wire_t *wire, arm_dap_t *dap)
{
    swd_gpio_slow_down(wire);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        swd_wire_reset(wire);
        swd_gpio_trace_begin();
        esp_err_t ret = arm_dap_connect(dap);
        swd_gpio_trace_end();
        log_connection(wire, dap, "automatic SWD attach", ret);
        if (ret == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}
#endif

static void flash_task(void *argument)
{
    char *path = argument;
    image_t image = {0};
    swd_wire_t wire = {0};
    arm_dap_t dap = {.context = &wire, .transfer = swd_wire_transfer};
    stm32f1_target_t target = {.dap = &dap};
    const char *stage = "invalid BIN / HEX image";
    const char *done_message = "done, target running";
    char failure_detail[48];
    bool opened = false;
#if CONFIG_STM32_SWD_WIRED_RESET
    bool touched = false;
#endif
    esp_err_t ret = image_load(path, &image, s_base);
    if (ret != ESP_OK) goto cleanup;
    stage = "SWD GPIO setup";
    /* close must also run after partial GPIO initialization. */
    opened = true;
    ret = swd_gpio_open(&wire);
    if (ret != ESP_OK) goto cleanup;
    stage = "SWD pin levels / check GPIO wiring";
    ret = swd_gpio_check_pads();
    if (ret != ESP_OK) goto cleanup;
    stage = "SWD connect / check PA13 PA14";
    publish(2, "connecting SWD automatically");
#if CONFIG_STM32_SWD_MANUAL_RESET
    ret = connect_target_manual(&wire, &target, &stage);
    if (ret != ESP_OK) goto cleanup;
#elif CONFIG_STM32_SWD_WIRED_RESET
    touched = true;
    bool reset_asserted;
    ret = connect_target(&wire, &dap, &reset_asserted);
    if (ret != ESP_OK) goto cleanup;
    if (reset_asserted) {
        stage = "reset catch / check NRST";
        ret = stm32f1_prepare(&target);
        if (ret != ESP_OK) goto cleanup;
        swd_gpio_reset(false);
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        /* Save the original debug state for the same cleanup path as reset catch. */
        stage = "debug setup after reset";
        ret = stm32f1_prepare(&target);
        if (ret != ESP_OK) goto cleanup;
    }
#else
    ret = connect_target_auto(&wire, &dap);
    if (ret != ESP_OK) {
        stage = "SWD connect / check PA13 PA14";
        goto cleanup;
    }
    stage = "automatic SWD core setup";
    ret = stm32f1_prepare(&target);
    if (ret != ESP_OK) goto cleanup;
#endif
    stage = "F103 identification / protection";
    ret = stm32f1_halt_identify(&target);
    if (ret != ESP_OK) goto cleanup;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.target_id = target.device_id;
    s_status.page_size = target.page_bytes;
    xSemaphoreGive(s_job_lock);
    ESP_LOGI(TAG, "target ID 0x%03X: %lu KB, page %lu B", target.device_id,
             (unsigned long)(target.flash_bytes / 1024), (unsigned long)target.page_bytes);
    ret = program_image(&image, &target, s_verify, &stage);

cleanup:
    /* Keep the specific handshake failure on the existing progress label.
     * Identification failures remain distinguishable from image/write errors. */
    if (ret != ESP_OK && strcmp(stage, "SWD connect / check PA13 PA14") == 0) {
        snprintf(failure_detail, sizeof(failure_detail), "SWD %s ACK=%X req=%02X",
                 dap.connect_step ? dap.connect_step : "connect",
                 (unsigned)wire.last_ack, (unsigned)wire.last_request);
        stage = failure_detail;
    }
    if (ret != ESP_OK) ESP_LOGE(TAG, "%s: %s", stage, esp_err_to_name(ret));
    if (target.demcr_saved) {
        esp_err_t finish = stm32f1_finish(&target);
        if (ret == ESP_OK && finish != ESP_OK) { ret = finish; stage = "target cleanup failed"; }
    }
#if CONFIG_STM32_SWD_MANUAL_RESET
    /* NRST is not wired here, so the target has to restart itself. Releasing
     * the halt alone would resume the halted PC inside the image that was just
     * erased and rewritten, and the new program would not start until the next
     * power cycle. The flash content is already verified at this point, so a
     * failed restart is reported without turning the job into a failure. */
    if (target.demcr_saved) {
        if (stm32f1_reset_and_run(&target) != ESP_OK) {
            ESP_LOGW(TAG, "reset-and-run failed; press RESET on the target");
            (void)stm32f1_disable_debug(&target);
            done_message = "done, press RESET on target";
        }
    }
#elif CONFIG_STM32_SWD_WIRED_RESET
    if (touched) {
        swd_gpio_reset(true);
        vTaskDelay(pdMS_TO_TICKS(30));
        /* Release the debug halt with the application held in physical reset. */
        esp_err_t detach = stm32f1_disable_debug(&target);
        if (ret == ESP_OK && detach != ESP_OK) {
            ret = detach;
            stage = "debug detach failed";
        }
    }
#else
    if (target.demcr_saved) {
        if (ret == ESP_OK) {
            if (stm32f1_reset_and_run(&target) != ESP_OK) {
                ESP_LOGW(TAG, "automatic reset-and-run failed after verification");
                (void)stm32f1_disable_debug(&target);
                done_message = "done, reset retry required";
            }
        } else {
            (void)stm32f1_disable_debug(&target);
        }
    }
#endif
    if (opened) swd_gpio_close();
    esp_err_t restore = uart_link_end_isp();
#if CONFIG_STM32_SWD_WIRED_RESET
    if (touched) swd_gpio_reset(false);
#endif
    if (restore != ESP_OK) { ret = restore; stage = "UART pin restore failed"; }
    image_free(&image);
    data_store_fw_release();
    free(path);
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.reset_prompt = STM32_ISP_RESET_NONE;
    s_status.state = ret == ESP_OK ? STM32_ISP_DONE : STM32_ISP_FAILED;
    if (ret == ESP_OK) s_status.percent = 100;
    strlcpy(s_status.message, ret == ESP_OK ? done_message : stage,
             sizeof(s_status.message));
    s_busy = false;
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

esp_err_t stm32_isp_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!s_job_lock) s_job_lock = xSemaphoreCreateMutex();
    if (!s_job_lock) return ESP_ERR_NO_MEM;
    if (CONFIG_STM32_SWDIO_GPIO == CONFIG_STM32_SWCLK_GPIO ||
        !GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_STM32_SWDIO_GPIO) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_STM32_SWCLK_GPIO)) return ESP_ERR_INVALID_ARG;
#if CONFIG_STM32_SWD_WIRED_RESET
    if (CONFIG_STM32_NRST_GPIO == CONFIG_STM32_SWDIO_GPIO ||
        CONFIG_STM32_NRST_GPIO == CONFIG_STM32_SWCLK_GPIO ||
        !GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_STM32_NRST_GPIO)) return ESP_ERR_INVALID_ARG;
#endif
    esp_err_t e = uart_link_init();
    if (e != ESP_OK) return e;
#if CONFIG_STM32_SWD_WIRED_RESET
    /* NRST is active-low open drain; releasing it never drives against a target
     * reset controller. BOOT0 is left untouched by the SWD backend.
     */
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 1);
    const gpio_config_t reset = {
        .pin_bit_mask = 1ULL << CONFIG_STM32_NRST_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    e = gpio_config(&reset);
    if (e != ESP_OK) return e;
#endif
    s_initialized = true;
    publish(0, "idle, SWD ready");
#if CONFIG_STM32_SWD_MANUAL_RESET
    ESP_LOGI(TAG, "SWD prepared: DIO=%d CLK=%d; manual RESET; normal UART active",
             CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO);
#elif CONFIG_STM32_SWD_WIRED_RESET
    ESP_LOGI(TAG, "SWD prepared: DIO=%d CLK=%d NRST=%d; normal UART active",
             CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO, CONFIG_STM32_NRST_GPIO);
#else
    ESP_LOGI(TAG, "SWD prepared: DIO=%d CLK=%d; automatic attach/reset; normal UART active",
             CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO);
#endif
    return ESP_OK;
}
esp_err_t stm32_isp_start(const char *path, uint32_t base, uint32_t page_size, bool verify)
{
    (void)page_size; /* SWD density identification determines the actual erase size. */
    if (!path || !s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    if (s_busy) { xSemaphoreGive(s_job_lock); return ESP_ERR_INVALID_STATE; }
    s_busy = true;
    xSemaphoreGive(s_job_lock);
    esp_err_t ret = data_store_fw_acquire();
    if (ret != ESP_OK) goto fail;
    ret = uart_link_begin_gpio();
    if (ret != ESP_OK) { data_store_fw_release(); goto fail; }
    char *copy = strdup(path);
    ret = ESP_ERR_NO_MEM;
    if (copy) {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        memset(&s_status, 0, sizeof(s_status));
        s_status.state = STM32_ISP_RUNNING;
        s_base = base ? base : CONFIG_STM32_DEFAULT_BASE;
        s_verify = verify;
        xSemaphoreGive(s_job_lock);
        publish(0, "loading firmware");
        if (xTaskCreate(flash_task, "stm32_swd", 6144, copy, 5, NULL) == pdPASS) return ESP_OK;
        free(copy);
    }
    if (uart_link_end_isp() != ESP_OK) ret = ESP_FAIL;
    data_store_fw_release();
fail:
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_status.state = STM32_ISP_FAILED;
    strlcpy(s_status.message, "start failed / resource busy", sizeof(s_status.message));
    s_busy = false;
    xSemaphoreGive(s_job_lock);
    return ret;
}
void stm32_isp_status(stm32_isp_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_job_lock) return;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_job_lock);
}
bool stm32_isp_busy(void)
{
    if (!s_job_lock) return false;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    bool busy = s_busy;
    xSemaphoreGive(s_job_lock);
    return busy;
}
