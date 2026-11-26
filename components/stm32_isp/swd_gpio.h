#pragma once
#include "swd_wire.h"
/* GPIO ownership must be acquired from uart_link before opening this adapter. */
esp_err_t swd_gpio_open(swd_wire_t *wire);
void swd_gpio_slow_down(swd_wire_t *wire);
/* Samples the shared pads before the first SWD line reset. */
esp_err_t swd_gpio_check_pads(void);
/* Capture one initial transaction at the local pads without UART output
 * inside clock cycles. Dumping happens after the transaction completes. */
void swd_gpio_trace_begin(void);
void swd_gpio_trace_end(void);
void swd_gpio_close(void);
void swd_gpio_reset(bool asserted);
