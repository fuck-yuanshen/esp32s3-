#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM32_ISP_IDLE = 0,
    STM32_ISP_RUNNING,
    STM32_ISP_DONE,
    STM32_ISP_FAILED,
} stm32_isp_state_t;

typedef enum {
    STM32_ISP_RESET_NONE = 0,
    STM32_ISP_RESET_HOLD,
    STM32_ISP_RESET_RELEASE,
} stm32_isp_reset_prompt_t;

typedef struct {
    stm32_isp_state_t state;
    stm32_isp_reset_prompt_t reset_prompt;
    uint8_t percent;          /* 0..100 over erase, write and verify */
    char message[48];         /* current step, shown next to the bar */
    uint32_t bytes_written;
    uint32_t bytes_total;
    uint16_t target_id;       /* DBGMCU device ID (SWD) or USART product ID */
    uint32_t page_size;       /* erase page size used by the running job */
} stm32_isp_status_t;

/* Offline programmer; menuconfig selects SWD (default) or legacy USART.
 * Default SWD wiring: GPIO19 -> PA13/SWDIO, GPIO20 -> PA14/SWCLK,
 * common GND, target powered at 3.3 V. Automatic SWD mode attaches to the
 * running target and issues SYSRESETREQ after verification. With wired reset
 * enabled, the configured GPIO drives NRST (open drain).
 * SWO/PB3 is not the SWD data pin. SWD leaves BOOT0 untouched.
 * BIN uses base_address; Intel HEX supplies addresses. The SWD backend
 * identifies single-bank STM32F1 densities and restricts writes to user flash.
 */
esp_err_t stm32_isp_init(void);

/* Run one flash cycle in a background task. An uninitialized module, null path,
 * running job or unavailable DATA-mode lease returns ESP_ERR_INVALID_STATE.
 * File loading errors are reported asynchronously through stm32_isp_status().
 * Initialization leaves normal UART RX/TX active; start alone acquires ISP.
 * Every job exit restores normal UART mode before releasing the job. A UART
 * hardware restore error is reported as FAILED and leaves DATA traffic paused.
 *
 * base_address : load address for a raw .bin; 0 selects CONFIG_STM32_DEFAULT_BASE.
 * page_size    : ignored by SWD (identified automatically); legacy USART uses
 *                CONFIG_STM32_DEFAULT_PAGE_SIZE when this argument is zero.
 *                F1 uses 1024 or 2048, F3/L4/G0/G4 use 2048, L0 uses 128, L1 256.
 * verify       : read the image back after writing.
 */
esp_err_t stm32_isp_start(const char *path, uint32_t base_address, uint32_t page_size,
                          bool verify);

/* Copy the current job state for the UI task. */
void stm32_isp_status(stm32_isp_status_t *status);

#if CONFIG_STM32_SWD_MANUAL_RESET
/* Acknowledge the displayed stage. An obsolete or repeated event is rejected. */
esp_err_t stm32_isp_confirm_reset(stm32_isp_reset_prompt_t prompt);
#endif

/* True while a flash job has reserved the programmer and its shared pins. */
bool stm32_isp_busy(void);

#ifdef __cplusplus
}
#endif

