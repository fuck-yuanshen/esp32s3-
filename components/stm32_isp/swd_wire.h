#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bit-level ADIv5 SWD transport. The GPIO adapter supplies electrical I/O;
 * host tests supply a deterministic target without changing the protocol.
 */
typedef struct {
    void *context;
    void (*clock)(void *, bool);
    void (*data)(void *, bool);
    bool (*read)(void *);
    void (*output)(void *, bool);
    void (*delay)(void *);
} swd_io_t;
typedef struct {
    swd_io_t io;
    unsigned wait_retries;
    uint8_t last_request;
    uint8_t last_ack; /* Raw three-bit response, retained even on protocol errors. */
} swd_wire_t;
void swd_wire_reset(swd_wire_t *wire);
esp_err_t swd_wire_transfer(void *context, bool ap, bool read,
                            uint8_t address, uint32_t *value);
