#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UART_LINK_RX_CHUNK_BYTES 256
#define UART_LINK_RX_HISTORY_BYTES 256

typedef enum {
    UART_LINK_MODE_STOPPED = 0,
    UART_LINK_MODE_DATA,
    UART_LINK_MODE_ISP,
} uart_link_mode_t;

/* Receives one raw UART2 chunk from the background reader. The callback runs
 * after the UART mutex is released and must copy data before returning. */
typedef void (*uart_link_rx_callback_t)(const uint8_t *data, size_t length);

/* Normal-mode counters and newest received bytes, oldest first. Read chunks
 * are stream fragments, not protocol frames. ISP bytes are excluded. History
 * is RAM-only; the oldest bytes roll out when its fixed capacity is reached.
 */
typedef struct {
    uart_link_mode_t mode;
    uint32_t baud_rate;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    size_t rx_length;
    uint8_t rx_data[UART_LINK_RX_HISTORY_BYTES];
} uart_link_snapshot_t;

esp_err_t uart_link_init(void);
/* Stops RX before deleting the driver; ignored during an active ISP lease. */
void uart_link_deinit(void);

/* ISP coordinator only: acquire once per job, release on every exit path.
 * begin drains DATA TX, changes baud and discards stale input. end drains ISP
 * TX, restores the normal baud and discards ISP input before enabling DATA.
 */
esp_err_t uart_link_begin_isp(uint32_t baud_rate);
esp_err_t uart_link_end_isp(void);
/* SWD coordinator: drain DATA TX, pause RX interrupts, release UART pin routing.
 * The UART driver stays installed. end_isp restores both mapping and normal baud.
 */
esp_err_t uart_link_begin_gpio(void);

/* Register the consumer for live UART2 receive chunks. Passing NULL disables
 * the callback while the local history and counters continue to work. */
void uart_link_set_rx_callback(uart_link_rx_callback_t callback);

/* Return ESP_ERR_INVALID_STATE during ISP. Payload and optional CR LF are
 * emitted atomically with respect to mode changes and other DATA sends.
 */
esp_err_t uart_link_send(const uint8_t *data, size_t length);
esp_err_t uart_link_send_raw(const uint8_t *data, size_t length);
void uart_link_snapshot(uart_link_snapshot_t *snapshot);
uint32_t uart_link_tx_bytes(void);
#ifdef __cplusplus
}
#endif
