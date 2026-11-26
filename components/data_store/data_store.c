/* data_store.c
 *
 * Purpose in the project:
 *   Single owner of the SPIFFS "storage" partition.
 *     data.txt : payload table edited over BLE and picked on the LCD
 *     fw.bin   : STM32 firmware image received over BLE
 *   The table is cached in RAM so the UI never reads flash on a key press;
 *   flash is rewritten only when the table changes.
 */

#include "data_store.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "data_store";
#define FW_UPLOAD_PATH DATA_STORE_MOUNT_POINT "/fw.tmp"
#define FW_BACKUP_PATH DATA_STORE_MOUNT_POINT "/fw.bak"

static data_record_t s_records[DATA_STORE_MAX_RECORDS];
static size_t s_count;
static SemaphoreHandle_t s_mutex;

static FILE *s_fw_file;
static size_t s_fw_expected;
static size_t s_fw_written;
static bool s_fw_reading;

static void store_lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void store_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static bool remove_if_present(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT;
}

/* SPIFFS rename requires an unused destination. If power is lost after
 * moving the previous image aside, restore it before accepting another upload.
 */
static esp_err_t recover_fw_backup(void)
{
    struct stat info;
    if (stat(FW_BACKUP_PATH, &info) != 0) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (stat(DATA_STORE_FW_PATH, &info) == 0) {
        return remove_if_present(FW_BACKUP_PATH) ? ESP_OK : ESP_FAIL;
    }
    if (errno != ENOENT) {
        return ESP_FAIL;
    }
    return rename(FW_BACKUP_PATH, DATA_STORE_FW_PATH) == 0 ? ESP_OK : ESP_FAIL;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Accept "AA 55 01", "AA5501" and "0xAA,0x55". Returns the byte count, or -1
 * when the text is not hexadecimal after the separators are removed.
 */
static int parse_hex(const char *text, uint8_t *out, size_t capacity)
{
    size_t count = 0;
    int high = -1;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == ':') {
            continue;
        }
        int nibble;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            ++p;
            continue;
        }
        nibble = hex_nibble(*p);
        if (nibble < 0) {
            return -1;
        }
        if (high < 0) {
            high = nibble;
        } else {
            if (count >= capacity) {
                return -1;
            }
            out[count++] = (uint8_t)((high << 4) | nibble);
            high = -1;
        }
    }
    return high < 0 ? (int)count : -1;
}

/* One record per line: "name|payload". "hex:" and "text:" force the payload
 * interpretation; a bare payload is treated as hex when it parses cleanly and
 * as literal text otherwise, which keeps hand-edited files usable.
 */
static esp_err_t parse_line(const char *line, data_record_t *record)
{
    const char *separator = strchr(line, '|');
    if (!separator) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_length = (size_t)(separator - line);
    while (name_length > 0 && isspace((unsigned char)line[name_length - 1])) {
        --name_length;
    }
    if (name_length == 0 || name_length > DATA_STORE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->name, line, name_length);
    record->name[name_length] = '\0';

    const char *payload = separator + 1;
    while (*payload == ' ') {
        ++payload;
    }

    if (strncmp(payload, "hex:", 4) == 0) {
        int length = parse_hex(payload + 4, record->payload, sizeof(record->payload));
        if (length < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        record->length = (uint16_t)length;
        return ESP_OK;
    }
    if (strncmp(payload, "text:", 5) == 0) {
        size_t length = strlen(payload + 5);
        if (length > DATA_STORE_PAYLOAD_MAX) {
            length = DATA_STORE_PAYLOAD_MAX;
        }
        memcpy(record->payload, payload + 5, length);
        record->length = (uint16_t)length;
        return ESP_OK;
    }

    int hex_length = parse_hex(payload, record->payload, sizeof(record->payload));
    if (hex_length >= 0) {
        record->length = (uint16_t)hex_length;
        return ESP_OK;
    }
    size_t length = strlen(payload);
    if (length > DATA_STORE_PAYLOAD_MAX) {
        length = DATA_STORE_PAYLOAD_MAX;
    }
    memcpy(record->payload, payload, length);
    record->length = (uint16_t)length;
    return ESP_OK;
}

/* Rewrite the table through a temporary file: a power loss mid-write leaves
 * the previous data.txt intact instead of a truncated record list.
 */
static esp_err_t save_locked(void)
{
    FILE *file = fopen(DATA_STORE_TMP_PATH, "w");
    if (!file) {
        ESP_LOGE(TAG, "cannot open %s for writing", DATA_STORE_TMP_PATH);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < s_count; ++i) {
        const data_record_t *record = &s_records[i];
        fprintf(file, "%s|hex:", record->name);
        for (uint16_t b = 0; b < record->length; ++b) {
            fprintf(file, "%02X", record->payload[b]);
        }
        fputc('\n', file);
    }
    if (fflush(file) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);

    unlink(DATA_STORE_PATH);
    if (rename(DATA_STORE_TMP_PATH, DATA_STORE_PATH) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", DATA_STORE_TMP_PATH, DATA_STORE_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t load_locked(void)
{
    s_count = 0;
    FILE *file = fopen(DATA_STORE_PATH, "r");
    if (!file) {
        ESP_LOGI(TAG, "no data.txt yet, table starts empty");
        return ESP_OK;
    }
    char line[DATA_STORE_NAME_MAX + DATA_STORE_PAYLOAD_MAX * 2 + 16];
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0 || line[0] == '#') {
            continue;
        }
        if (s_count >= DATA_STORE_MAX_RECORDS) {
            ESP_LOGW(TAG, "more than %d lines, the rest is ignored", DATA_STORE_MAX_RECORDS);
            break;
        }
        if (parse_line(line, &s_records[s_count]) == ESP_OK) {
            ++s_count;
        } else {
            ESP_LOGW(TAG, "malformed line skipped: %s", line);
        }
    }
    fclose(file);
    ESP_LOGI(TAG, "data.txt loaded: %u record(s)", (unsigned)s_count);
    return ESP_OK;
}

esp_err_t data_store_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_spiffs_conf_t config = {
        .base_path = DATA_STORE_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS ready: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }

    store_lock();
    if (recover_fw_backup() != ESP_OK || !remove_if_present(FW_UPLOAD_PATH)) {
        ESP_LOGW(TAG, "firmware upload recovery failed");
    }
    ret = load_locked();
    store_unlock();
    return ret;
}

size_t data_store_count(void)
{
    store_lock();
    size_t count = s_count;
    store_unlock();
    return count;
}

esp_err_t data_store_get(size_t index, data_record_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    store_lock();
    esp_err_t ret = ESP_OK;
    if (index >= s_count) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        *out = s_records[index];
    }
    store_unlock();
    return ret;
}

esp_err_t data_store_add(const char *name, const uint8_t *payload, size_t length)
{
    if (!name || !payload || length > DATA_STORE_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    store_lock();
    esp_err_t ret = ESP_OK;
    if (s_count >= DATA_STORE_MAX_RECORDS) {
        ret = ESP_ERR_NO_MEM;
    } else {
        data_record_t *record = &s_records[s_count];
        strlcpy(record->name, name, sizeof(record->name));
        memcpy(record->payload, payload, length);
        record->length = (uint16_t)length;
        ++s_count;
        ret = save_locked();
        if (ret != ESP_OK) {
            --s_count;
        }
    }
    store_unlock();
    return ret;
}

esp_err_t data_store_clear(void)
{
    store_lock();
    s_count = 0;
    esp_err_t ret = save_locked();
    store_unlock();
    return ret;
}

esp_err_t data_store_reload(void)
{
    store_lock();
    esp_err_t ret = load_locked();
    store_unlock();
    return ret;
}

esp_err_t data_store_add_line(const char *line)
{
    if (!line) {
        return ESP_ERR_INVALID_ARG;
    }
    data_record_t record;
    esp_err_t ret = parse_line(line, &record);
    if (ret != ESP_OK) {
        return ret;
    }
    return data_store_add(record.name, record.payload, record.length);
}

size_t data_store_fw_size(void)
{
    struct stat info;
    if (stat(DATA_STORE_FW_PATH, &info) != 0) {
        return 0;
    }
    return (size_t)info.st_size;
}

bool data_store_fw_exists(void)
{
    return data_store_fw_size() > 0;
}

esp_err_t data_store_fw_begin(size_t expected_size)
{
    if (expected_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    store_lock();
    esp_err_t ret = ESP_OK;
    if (s_fw_file || s_fw_reading) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (recover_fw_backup() != ESP_OK) {
        ret = ESP_FAIL;
    } else {
        s_fw_file = fopen(FW_UPLOAD_PATH, "wb");
        if (!s_fw_file) {
            ret = ESP_FAIL;
        } else {
            s_fw_expected = expected_size;
            s_fw_written = 0;
            ESP_LOGI(TAG, "firmware upload started, expecting %u bytes",
                     (unsigned)expected_size);
        }
    }
    store_unlock();
    return ret;
}

static void fw_abort_locked(void)
{
    if (s_fw_file) {
        fclose(s_fw_file);
        s_fw_file = NULL;
        if (!remove_if_present(FW_UPLOAD_PATH)) {
            ESP_LOGW(TAG, "could not remove abandoned firmware upload");
        }
    }
}

void data_store_fw_abort(void)
{
    store_lock();
    fw_abort_locked();
    store_unlock();
}

esp_err_t data_store_fw_write(const uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    store_lock();
    esp_err_t ret = ESP_OK;
    if (!s_fw_file) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (length > s_fw_expected - s_fw_written) {
        fw_abort_locked();
        ret = ESP_ERR_INVALID_SIZE;
    } else if (fwrite(data, 1, length, s_fw_file) != length) {
        fw_abort_locked();
        ret = ESP_FAIL;
    } else {
        s_fw_written += length;
    }
    store_unlock();
    return ret;
}

static esp_err_t fw_publish_locked(void)
{
    bool backed_up = rename(DATA_STORE_FW_PATH, FW_BACKUP_PATH) == 0;
    if (!backed_up && errno != ENOENT) {
        return ESP_FAIL;
    }
    if (rename(FW_UPLOAD_PATH, DATA_STORE_FW_PATH) != 0) {
        if (backed_up && recover_fw_backup() != ESP_OK) {
            ESP_LOGE(TAG, "firmware publish failed; previous image retained as backup");
        }
        return ESP_FAIL;
    }
    if (backed_up && !remove_if_present(FW_BACKUP_PATH)) {
        ESP_LOGW(TAG, "could not remove previous firmware image");
    }
    return ESP_OK;
}

esp_err_t data_store_fw_end(void)
{
    store_lock();
    esp_err_t ret = ESP_OK;
    if (!s_fw_file) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_fw_written != s_fw_expected) {
        fw_abort_locked();
        ret = ESP_ERR_INVALID_SIZE;
    } else {
        int flushed = fflush(s_fw_file);
        int closed = fclose(s_fw_file);
        s_fw_file = NULL;
        ret = flushed == 0 && closed == 0 ? fw_publish_locked() : ESP_FAIL;
        if (ret != ESP_OK) {
            if (!remove_if_present(FW_UPLOAD_PATH)) {
                ESP_LOGW(TAG, "could not remove failed firmware upload");
            }
        } else {
            ESP_LOGI(TAG, "firmware upload committed: %u bytes", (unsigned)s_fw_written);
        }
    }
    store_unlock();
    return ret;
}

esp_err_t data_store_fw_acquire(void)
{
    store_lock();
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_fw_file && !s_fw_reading) {
        s_fw_reading = true;
        ret = ESP_OK;
    }
    store_unlock();
    return ret;
}

void data_store_fw_release(void)
{
    store_lock();
    s_fw_reading = false;
    store_unlock();
}

esp_err_t data_store_fw_delete(void)
{
    store_lock();
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_fw_file && !s_fw_reading) {
        ret = recover_fw_backup();
        if (ret == ESP_OK && unlink(DATA_STORE_FW_PATH) != 0) {
            ret = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }
    store_unlock();
    return ret;
}

esp_err_t data_store_usage(size_t *total, size_t *used)
{
    if (!total || !used) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_spiffs_info("storage", total, used);
}


