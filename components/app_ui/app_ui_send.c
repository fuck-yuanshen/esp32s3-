/* app_ui_send.c
 *
 * Purpose in the project:
 *   "Send payload" screen. It lists every record stored in data.txt, lets the
 *   user pick one with the five-way keypad and transmits it on shared GPIO19/20
 *   to the connected device. STM32 ISP uses the same link in a separate mode.
 *
 *   The list is rebuilt from data_store whenever the screen is shown, so a
 *   record added over BLE appears without a reboot.
 */

#include "app_ui_internal.h"

#include <stdio.h>

#include "data_store.h"
#include "esp_log.h"
#include "lvgl.h"
#include "uart_link.h"
#include "ui_theme.h"

static const char *TAG = "app_ui_send";

static lv_obj_t *s_status;

/* Timer and labels belong to one screen instance. Reload can create the next
 * instance before the previous screen is destroyed, so timers use no globals.
 */
typedef struct {
    lv_obj_t *stats;
    lv_obj_t *preview;
    lv_obj_t *list;
    lv_timer_t *timer;
    uart_link_mode_t last_mode;
    uint32_t last_rx;
    uint32_t last_tx;
    bool rendered;
} send_monitor_t;

static void monitor_poll(lv_timer_t *timer)
{
    send_monitor_t *monitor = lv_timer_get_user_data(timer);
    uart_link_snapshot_t state;
    uart_link_snapshot(&state);
    if (monitor->rendered && state.mode == monitor->last_mode &&
        state.rx_bytes == monitor->last_rx && state.tx_bytes == monitor->last_tx) {
        return;
    }
    const char *mode = state.mode == UART_LINK_MODE_DATA ? "DATA" :
                       state.mode == UART_LINK_MODE_ISP ? "FLASH" : "OFF";
    lv_label_set_text_fmt(monitor->stats, "%s %lu  TX:%lu  RX:%lu", mode,
                          (unsigned long)state.baud_rate,
                          (unsigned long)state.tx_bytes, (unsigned long)state.rx_bytes);
    const bool enabled = state.mode == UART_LINK_MODE_DATA;
    const uint32_t count = lv_obj_get_child_count(monitor->list);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *button = lv_obj_get_child(monitor->list, i);
        if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (state.rx_length) {
        const size_t length = state.rx_length < UI_RX_PREVIEW_BYTES
                            ? state.rx_length : UI_RX_PREVIEW_BYTES;
        const uint8_t *tail = state.rx_data + state.rx_length - length;
        char hex[UI_RX_PREVIEW_BYTES * 3 + 1] = {0};
        char ascii[UI_RX_PREVIEW_BYTES + 1] = {0};
        for (size_t i = 0; i < length; ++i) {
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", tail[i]);
            ascii[i] = tail[i] >= 32 && tail[i] <= 126 ? (char)tail[i] : '.';
        }
        lv_label_set_text_fmt(monitor->preview, "RX HEX: %s\nASCII: %s", hex, ascii);
    } else {
        lv_label_set_text(monitor->preview, "RX HEX: --\nASCII: waiting for device");
    }
    monitor->last_mode = state.mode;
    monitor->last_rx = state.rx_bytes;
    monitor->last_tx = state.tx_bytes;
    monitor->rendered = true;
}

static void monitor_deleted(lv_event_t *event)
{
    send_monitor_t *monitor = lv_event_get_user_data(event);
    if (monitor->timer) lv_timer_delete(monitor->timer);
    lv_free(monitor);
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    app_request_screen(app_menu_screen);
}

/* Short hex preview so two records with similar names stay distinguishable on
 * a 320 px wide panel.
 */
static void hex_preview(const data_record_t *record, char *out, size_t capacity)
{
    size_t used = 0;
    out[0] = '\0';
    const size_t limit = record->length < 10 ? record->length : 10;
    for (size_t i = 0; i < limit && used + 4 < capacity; ++i) {
        used += (size_t)snprintf(out + used, capacity - used, "%02X ", record->payload[i]);
    }
    if (record->length > limit && used + 3 < capacity) {
        snprintf(out + used, capacity - used, "..");
    }
}

static void send_clicked(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    data_record_t record;
    if (data_store_get(index, &record) != ESP_OK) {
        app_set_status(s_status, "Record vanished, press Reload");
        return;
    }
    const esp_err_t ret = uart_link_send(record.payload, record.length);
    if (ret == ESP_OK) {
        lv_label_set_text_fmt(s_status, "Sent %u B from %s", (unsigned)record.length,
                              record.name);
        ESP_LOGI(TAG, "sent %s (%u bytes)", record.name, (unsigned)record.length);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        app_set_status(s_status, "Flasher owns pins: UART paused");
    } else {
        lv_label_set_text_fmt(s_status, "UART error: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "send failed: %s", esp_err_to_name(ret));
    }
}

static void reload_clicked(lv_event_t *event)
{
    (void)event;
    data_store_reload();
    app_request_screen(app_send_screen);
}

static void send_build(app_screen_t *screen)
{
    lv_obj_t *column = lv_obj_create(screen->root);
    lv_obj_set_size(column, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, UI_PAD, 0);
    lv_obj_set_style_pad_row(column, UI_GAP, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    const size_t count = data_store_count();
    lv_obj_t *title = app_add_title(column, "SEND PAYLOAD");
    lv_label_set_text_fmt(title, "SEND PAYLOAD  %u/%u", (unsigned)count,
                          (unsigned)DATA_STORE_MAX_RECORDS);

    send_monitor_t *monitor = lv_malloc(sizeof(*monitor));
    if (monitor) *monitor = (send_monitor_t){0};
    s_status = app_add_status(column);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    app_set_status(s_status, count ? "OK sends the focused record"
                                   : "data.txt empty: add lines over BLE");

    if (monitor) {
        monitor->stats = app_add_status(column);
        monitor->preview = app_add_status(column);
        lv_obj_set_height(monitor->preview, UI_RX_HEIGHT);
        lv_obj_set_style_text_color(monitor->preview, UI_TEXT, 0);
    }

    /* The record list is the only scrollable widget on this screen, so LVGL
     * keeps the focused entry visible while the user walks through it.
     */
    lv_obj_t *list = lv_obj_create(column);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (size_t i = 0; i < count; ++i) {
        data_record_t record;
        if (data_store_get(i, &record) != ESP_OK) {
            continue;
        }
        char preview[40];
        hex_preview(&record, preview, sizeof(preview));
        char label[96];
        snprintf(label, sizeof(label), "%u %s  %uB\n%s", (unsigned)(i + 1), record.name,
                 (unsigned)record.length, preview);
        lv_obj_t *button = app_add_button(screen, list, label, send_clicked,
                                          (void *)(uintptr_t)i);
        lv_obj_set_height(button, 46);
    }

    lv_obj_t *footer = lv_obj_create(column);
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, UI_BUTTON_HEIGHT);
    lv_obj_set_style_pad_column(footer, UI_GAP, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *reload = app_add_button(screen, footer, "RELOAD", reload_clicked, NULL);
    lv_obj_t *back = app_add_button(screen, footer, "- BACK -", back_clicked, NULL);
    lv_obj_set_width(reload, 0);
    lv_obj_set_width(back, 0);
    lv_obj_set_flex_grow(reload, 1);
    lv_obj_set_flex_grow(back, 1);
    if (monitor) {
        monitor->list = list;
        monitor->timer = lv_timer_create(monitor_poll, UI_RX_POLL_MS, monitor);
        lv_obj_add_event_cb(screen->root, monitor_deleted, LV_EVENT_DELETE, monitor);
        if (monitor->timer) monitor_poll(monitor->timer);
        else lv_label_set_text(monitor->preview, "RX monitor timer allocation failed");
    }
}

app_screen_t *app_send_screen(void)
{
    return app_screen_create(send_build);
}
