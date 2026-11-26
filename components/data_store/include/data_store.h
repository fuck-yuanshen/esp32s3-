#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent storage on the SPIFFS "storage" partition.
 *
 *   /spiffs/data.txt  payload table, one "name|hex:AA55" line per record
 *   /spiffs/data.tmp  temporary file used for atomic rewrites
 *   /spiffs/fw.bin    STM32 image received over BLE
 *
 * data.txt is loaded once at boot into a small RAM table so the UI never
 * touches flash while the user scrolls. Every change rewrites the file.
 */
#define DATA_STORE_MOUNT_POINT    "/spiffs"
#define DATA_STORE_PATH           DATA_STORE_MOUNT_POINT "/data.txt"
#define DATA_STORE_TMP_PATH       DATA_STORE_MOUNT_POINT "/data.tmp"
#define DATA_STORE_FW_PATH        DATA_STORE_MOUNT_POINT "/fw.bin"

#define DATA_STORE_MAX_RECORDS    32
#define DATA_STORE_NAME_MAX       24
#define DATA_STORE_PAYLOAD_MAX    160

typedef struct {
    char name[DATA_STORE_NAME_MAX + 1];
    uint8_t payload[DATA_STORE_PAYLOAD_MAX];
    uint16_t length;
} data_record_t;

/** Mount SPIFFS and load data.txt. Safe to call once at startup. */
esp_err_t data_store_init(void);

/** Number of records currently in the table. */
size_t data_store_count(void);

/** Copy record `index` into `out`. ESP_ERR_NOT_FOUND when out of range. */
esp_err_t data_store_get(size_t index, data_record_t *out);

/** Append a record and rewrite data.txt. */
esp_err_t data_store_add(const char *name, const uint8_t *payload, size_t length);

/** Remove every record and rewrite data.txt. */
esp_err_t data_store_clear(void);

/** Re-read data.txt from flash (after an external writer changed it). */
esp_err_t data_store_reload(void);

/** Parse one BLE line and append it. Accepted forms:
 *      name|hex:AA 55 01
 *      name|text:hello
 *      name|AA5501            (bare hex when it parses as hex)
 *      name|hello             (otherwise literal text)
 */
esp_err_t data_store_add_line(const char *line);

/** SPIFFS usage for the info screen. */
esp_err_t data_store_usage(size_t *total_bytes, size_t *used_bytes);

/** Firmware image upload session. The BLE layer calls begin/write/end; the
 *  flasher only reads the finished file through DATA_STORE_FW_PATH.
 */
esp_err_t data_store_fw_begin(size_t expected_size);
esp_err_t data_store_fw_write(const uint8_t *data, size_t length);
esp_err_t data_store_fw_end(void);
void data_store_fw_abort(void);
size_t data_store_fw_size(void);
bool data_store_fw_exists(void);
/* A programmer holds the firmware lease across loading/programming/verification.
 * Upload and deletion reject changes while this lease is active.
 */
esp_err_t data_store_fw_acquire(void);
void data_store_fw_release(void);
esp_err_t data_store_fw_delete(void);

#ifdef __cplusplus
}
#endif
