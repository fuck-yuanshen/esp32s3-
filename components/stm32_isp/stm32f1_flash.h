#pragma once
#include <stddef.h>
#include "arm_dap.h"
#define STM32F1_FLASH_BASE 0x08000000U
typedef struct {
    arm_dap_t *dap;
    uint16_t device_id;
    uint32_t flash_bytes;
    uint32_t page_bytes;
    uint32_t saved_demcr;
    bool demcr_saved;
    bool lock_on_exit;
} stm32f1_target_t;
/* The job coordinates SWD attach. Prepare arms vector catch and debug halt
 * before the flash controller is accessed. Automatic mode can do this while
 * NRST is unwired because the core is reset after verification. */
esp_err_t stm32f1_prepare(stm32f1_target_t *t);
esp_err_t stm32f1_halt_identify(stm32f1_target_t *t);
esp_err_t stm32f1_unlock(stm32f1_target_t *t);
bool stm32f1_range_valid(const stm32f1_target_t *t, uint32_t address, size_t size);
esp_err_t stm32f1_erase_page(stm32f1_target_t *t, uint32_t address);
esp_err_t stm32f1_program(stm32f1_target_t *t, uint32_t address,
                         const uint8_t *data, size_t size);
esp_err_t stm32f1_verify(stm32f1_target_t *t, uint32_t address,
                        const uint8_t *data, size_t size);
esp_err_t stm32f1_finish(stm32f1_target_t *t);
/* Leaves the target halted but free: it resumes the halted PC, which is still
 * inside the replaced image. Wired-reset jobs follow this with an NRST pulse. */
esp_err_t stm32f1_disable_debug(stm32f1_target_t *t);
/* Restarts the target from its reset vector with debug released, so the image
 * that was just written actually starts. Required when NRST is not wired. */
esp_err_t stm32f1_reset_and_run(stm32f1_target_t *t);
