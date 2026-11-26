/* ble_data.c
 *
 * Purpose in the project:
 *   Provides the phone-facing configuration channel. A phone writes payload
 *   lines that are appended to data.txt on the SPIFFS partition, and can push
 *   a complete STM32 firmware image into fw.bin for the offline programmer.
 *
 *   The GATT layout is the Nordic UART Service shape so any BLE serial
 *   terminal app can be used without a custom client.
 *
 *   Wire protocol (UTF-8 lines on the RX characteristic):
 *     name|hex:AA 55 01        append a payload record
 *     CLEAR                    delete every record
 *     LIST                     dump the record list as notifications
 *     DEL                      delete fw.bin
 *     FW:BEGIN:<bytes>         start a firmware upload
 *     FW:HEX:<hex bytes>       append one chunk (repeat until complete)
 *     FW:END                   finish and verify the byte count
 *     INFO                     report partition usage and fw.bin size
 */

#include "ble_data.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_store.h"
#include "esp_log.h"
#include "uart_link.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_data";

#define BLE_LINE_MAX 512

/* Nordic UART Service UUIDs; kept compatible with generic BLE terminal apps. */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_fw_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x04, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_uart_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x05, 0x00, 0x40, 0x6e);

static uint16_t s_tx_handle;
static uint16_t s_fw_handle;
static uint16_t s_uart_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

static char s_line[BLE_LINE_MAX];
static size_t s_line_length;

static ble_data_event_cb_t s_data_changed_cb;
static volatile bool s_live_uart;

/* ---------------------------------------------------------------- helpers */

static int hex_value(char c)
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

/* Decode a hex chunk straight from the BLE buffer. Chunk sizes are bounded by
 * the negotiated MTU, so a fixed scratch buffer is enough.
 */
static size_t decode_hex(const char *text, uint8_t *out, size_t capacity)
{
    size_t count = 0;
    int high = -1;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == ',' || *p == ':' || *p == '\t') {
            continue;
        }
        int nibble = hex_value(*p);
        if (nibble < 0) {
            return 0;
        }
        if (high < 0) {
            high = nibble;
        } else {
            if (count >= capacity) {
                return 0;
            }
            out[count++] = (uint8_t)((high << 4) | nibble);
            high = -1;
        }
    }
    return high < 0 ? count : 0;
}

static void notify_text(const char *text)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_handle) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, (uint16_t)strlen(text));
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify failed rc=%d", rc);
    }
}

static void notify_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void notify_printf(const char *format, ...)
{
    char buffer[160];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    notify_text(buffer);
}

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_chr_def s_characteristics[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_tx_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    {
        .uuid = &s_fw_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_fw_handle,
    },
    {
        .uuid = &s_uart_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_uart_handle,
    },
    { 0 },
};

#define BLE_UART_NOTIFY_CHUNK 20U

static void uart_rx_notify(const uint8_t *data, size_t length)
{
    (void)ble_data_notify_uart(data, length);
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = s_characteristics,
    },
    { 0 },
};

/* ------------------------------------------------------------- line logic */

static void handle_line(char *line)
{
    ESP_LOGI(TAG, "RX: %s", line);

    if (strcmp(line, "CLEAR") == 0) {
        esp_err_t ret = data_store_clear();
        notify_printf(ret == ESP_OK ? "OK cleared\n" : "ERR clear %s\n", esp_err_to_name(ret));
        if (ret == ESP_OK && s_data_changed_cb) {
            s_data_changed_cb();
        }
        return;
    }
    if (strcmp(line, "LIST") == 0) {
        const size_t count = data_store_count();
        notify_printf("LIST %u\n", (unsigned)count);
        for (size_t i = 0; i < count; ++i) {
            data_record_t record;
            if (data_store_get(i, &record) == ESP_OK) {
                notify_printf("%u %s %u bytes\n", (unsigned)i, record.name,
                              (unsigned)record.length);
            }
        }
        notify_text("END\n");
        return;
    }
    if (strcmp(line, "DEL") == 0) {
        esp_err_t ret = data_store_fw_delete();
        if (ret == ESP_OK) notify_text("OK fw.bin deleted\n");
        else notify_printf("ERR delete %s\n", esp_err_to_name(ret));
        return;
    }
    if (strcmp(line, "INFO") == 0) {
        size_t total = 0;
        size_t used = 0;
        data_store_usage(&total, &used);
        notify_printf("INFO storage %u/%u bytes, fw.bin %u bytes, %u records\n",
                      (unsigned)used, (unsigned)total,
                      (unsigned)data_store_fw_size(), (unsigned)data_store_count());
        return;
    }
    if (strncmp(line, "FW:BEGIN:", 9) == 0) {
        const char *size_arg = line + 9;
        char *end = NULL;
        errno = 0;
        const unsigned long expected = strtoul(size_arg, &end, 10);
        const bool valid = size_arg[0] >= '0' && size_arg[0] <= '9' &&
                           *end == '\0' && errno == 0 && expected <= SIZE_MAX;
        esp_err_t ret = valid ? data_store_fw_begin((size_t)expected) : ESP_ERR_INVALID_ARG;
        if (ret == ESP_OK) {
            notify_printf("OK fw begin %u\n", (unsigned)expected);
        } else {
            notify_printf("ERR fw begin %s\n", esp_err_to_name(ret));
        }
        return;
    }
    if (strncmp(line, "FW:HEX:", 7) == 0) {
        static uint8_t chunk[256];
        const size_t length = decode_hex(line + 7, chunk, sizeof(chunk));
        esp_err_t ret = length ? data_store_fw_write(chunk, length)
                               : ESP_ERR_INVALID_ARG;
        if (ret != ESP_OK) {
            notify_printf("ERR fw write %s\n", esp_err_to_name(ret));
        }
        return;
    }
    if (strcmp(line, "FW:END") == 0) {
        esp_err_t ret = data_store_fw_end();
        if (ret == ESP_OK) {
            notify_printf("OK fw end %u bytes\n", (unsigned)data_store_fw_size());
        } else {
            notify_printf("ERR fw end %s\n", esp_err_to_name(ret));
        }
        return;
    }

    esp_err_t ret = data_store_add_line(line);
    if (ret == ESP_OK) {
        notify_printf("OK record %u saved\n", (unsigned)data_store_count());
        if (s_data_changed_cb) {
            s_data_changed_cb();
        }
    } else {
        notify_printf("ERR record %s\n", esp_err_to_name(ret));
    }
}

/* The RX characteristic is streamed: bytes accumulate until CR or LF, then
 * one command is executed. This keeps long hex payloads off the stack.
 */
static void feed_bytes(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_length) {
                s_line[s_line_length] = '\0';
                handle_line(s_line);
                s_line_length = 0;
            }
            continue;
        }
        if (s_line_length + 1 < sizeof(s_line)) {
            s_line[s_line_length++] = c;
        } else {
            s_line_length = 0;
            notify_text("ERR line too long\n");
        }
    }
}

/* Firmware writes may arrive either as the FW:HEX text protocol or as raw
 * binary on the firmware characteristic. Raw chunks are appended verbatim.
 */
static void feed_firmware(const uint8_t *data, size_t length)
{
    /* Each keyword is compared only when enough bytes are present; a short raw
     * firmware chunk must reach the append path instead of being parsed.
     */
    const bool text_protocol =
        (length >= 3 && memcmp(data, "FW:", 3) == 0) ||
        (length >= 4 && (memcmp(data, "INFO", 4) == 0 || memcmp(data, "LIST", 4) == 0)) ||
        (length >= 5 && memcmp(data, "CLEAR", 5) == 0) ||
        (length >= 3 && memcmp(data, "DEL", 3) == 0);
    if (text_protocol) {
        feed_bytes(data, length);
        if (s_line_length) {
            /* A phone may send a command without a terminator. */
            s_line[s_line_length] = '\0';
            handle_line(s_line);
            s_line_length = 0;
        }
        return;
    }
    esp_err_t ret = data_store_fw_write(data, length);
    if (ret != ESP_OK) {
        notify_printf("ERR fw raw %s\n", esp_err_to_name(ret));
    }
}

/* ---------------------------------------------------------------- GATT IO */

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t buffer[BLE_LINE_MAX];
    if (length > sizeof(buffer)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->chr && ble_uuid_cmp(ctxt->chr->uuid, &s_fw_uuid.u) == 0) {
        feed_firmware(buffer, copied);
    } else if (ctxt->chr && ble_uuid_cmp(ctxt->chr->uuid, &s_rx_uuid.u) == 0 &&
               s_live_uart) {
        /* Live mode deliberately bypasses line parsing so arbitrary binary
         * packets and embedded CR/LF bytes reach the target immediately. */
        const esp_err_t ret = uart_link_send_raw(buffer, copied);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "live UART write dropped: %s (%u bytes)",
                     esp_err_to_name(ret), (unsigned)copied);
        } else {
            ESP_LOGD(TAG, "live UART write: %u bytes", (unsigned)copied);
        }
    } else {
        feed_bytes(buffer, copied);
    }
    return 0;
}

/* ------------------------------------------------------------- GAP events */

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected");
            notify_text("READY\n");
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_line_length = 0;
        data_store_fw_abort();
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%d notify=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    /* Name + 128-bit UUID + flags do not all fit in the 31-byte legacy
     * advertising packet (BLE_HS_EINVAL, rc=4). Keep flags and the name in
     * the main packet and expose the service UUID in the scan response.
     */
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields scan_fields;
    memset(&scan_fields, 0, sizeof(scan_fields));
    scan_fields.uuids128 = (ble_uuid128_t[]){ s_service_uuid };
    scan_fields.num_uuids128 = 1;
    scan_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    uint8_t address[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, address, NULL);
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------- API */

esp_err_t ble_data_init(const char *device_name)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    uart_link_set_rx_callback(uart_rx_notify);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(device_name ? device_name : "ESP32S3-TOOL");
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE stack started");
    return ESP_OK;
}

void ble_data_deinit(void)
{
    uart_link_set_rx_callback(NULL);
    s_live_uart = false;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
}

bool ble_data_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_data_notify_uart(const uint8_t *data, size_t length)
{
    if (!s_live_uart || !data || length == 0 ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        !s_uart_handle) {
        return false;
    }
    while (length) {
        const size_t chunk = length < BLE_UART_NOTIFY_CHUNK
                           ? length : BLE_UART_NOTIFY_CHUNK;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)chunk);
        if (!om) return false;
        const int rc = ble_gatts_notify_custom(s_conn_handle, s_uart_handle, om);
        if (rc != 0) {
            ESP_LOGD(TAG, "UART notify failed rc=%d", rc);
            return false;
        }
        data += chunk;
        length -= chunk;
    }
    return true;
}

void ble_data_set_live_uart(bool enabled)
{
    s_live_uart = enabled;
    ESP_LOGI(TAG, "live UART bridge %s", enabled ? "enabled" : "disabled");
}

bool ble_data_live_uart_enabled(void)
{
    return s_live_uart;
}

bool ble_data_notify(const char *text)
{
    if (!text || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_handle) {
        return false;
    }
    notify_text(text);
    return true;
}

void ble_data_set_data_changed_cb(ble_data_event_cb_t callback)
{
    s_data_changed_cb = callback;
}




