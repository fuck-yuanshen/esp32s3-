#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE GATT bridge.
 *
 * Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX chr 6E400002-... write      : one line of "name|hex:AA55" appended to data.txt
 *   TX chr 6E400003-... notify     : status and log text back to the phone
 *   FW chr 6E400004-... write/notify: STM32 image upload
 *   UART chr 6E400005-... notify     : raw UART2 receive data
 *
 * A phone therefore needs any BLE serial terminal. Firmware upload uses the
 * same characteristic with a small text protocol:
 *     FW:BEGIN:<size>   stage a new image
 *     FW:HEX:<hex>      append decoded bytes
 *     FW:END            verify byte count and publish fw.bin
 * Plain binary writes to the FW characteristic are also appended verbatim.
 */
esp_err_t ble_data_init(const char *device_name);
void ble_data_deinit(void);

/* Queue a line for the notification characteristic. Returns false when no
 * phone is subscribed, which is normal while the link is idle.
 */
bool ble_data_notify(const char *text);
bool ble_data_connected(void);

/* Forward one raw UART2 RX chunk to the phone on the UART data notification
 * characteristic. The function preserves all byte values and may split a
 * chunk into conservative BLE notification packets. */
bool ble_data_notify_uart(const uint8_t *data, size_t length);

/* Enable the live bridge from the NUS RX characteristic to UART2. The bridge
 * forwards each received BLE write as one raw UART write and never appends
 * line endings or stores the bytes in data.txt. */
void ble_data_set_live_uart(bool enabled);
bool ble_data_live_uart_enabled(void);

/* Called from the BLE host task when data.txt changed, so the UI can refresh. */
typedef void (*ble_data_event_cb_t)(void);
void ble_data_set_data_changed_cb(ble_data_event_cb_t callback);

#ifdef __cplusplus
}
#endif
