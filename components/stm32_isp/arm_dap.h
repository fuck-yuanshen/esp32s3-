#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ADIv5 DP + AP0 MEM-AP operations, including posted AP reads and write
 * completion. Addresses are target memory addresses, never ESP32 pointers.
 */
typedef esp_err_t (*dap_transfer_fn)(void *, bool, bool, uint8_t, uint32_t *);
typedef struct {
    void *context;
    dap_transfer_fn transfer;
    uint32_t idcode;
    const char *connect_step; /* DP initialization step for transport diagnostics. */
} arm_dap_t;
esp_err_t arm_dap_connect(arm_dap_t *dap);
esp_err_t arm_dap_read32(arm_dap_t *dap, uint32_t address, uint32_t *value);
esp_err_t arm_dap_write32(arm_dap_t *dap, uint32_t address, uint32_t value);
esp_err_t arm_dap_write16(arm_dap_t *dap, uint32_t address, uint16_t value);
