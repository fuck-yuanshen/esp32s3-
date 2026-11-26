/* app_ui_live_uart.c
 *
 * Purpose in the project:
 *   Owns the live BLE-to-UART screen. While this screen is visible, writes to
 *   the Nordic UART Service RX characteristic are forwarded byte-for-byte to
 *   UART2 on GPIO19/20. Leaving the screen disables the bridge and returns the
 *   RX characteristic to the data.txt command protocol.
 */

#include "app_ui_internal.h"

#include "ble_data.h"
#include "esp_log.h"
#include "lvgl.h"
#include "uart_link.h"
#include "ui_theme.h"

static const char *TAG = "app_ui_live_uart";

typedef struct {
    lv_obj_t *status;
    lv_obj_t *stats;
    lv_timer_t *timer;
    uart_link_mode_t last_mode;
    uint32_t last_tx;
    bool last_connected;
    bool rendered;
} live_uart_view_t;

static void live_poll(lv_timer_t *timer)
{
    live_uart_view_t *view = lv_timer_get_user_data(timer);
    uart_link_snapshot_t state;
    uart_link_snapshot(&state);
    const bool connected = ble_data_connected();

    if (view->rendered && state.mode == view->last_mode &&
        state.tx_bytes == view->last_tx && connected == view->last_connected) {
        return;
    }

    const char *mode = state.mode == UART_LINK_MODE_DATA ? "READY" :
                       state.mode == UART_LINK_MODE_ISP ? "FLASH BUSY" : "UART OFF";
    lv_label_set_text_fmt(view->stats, "%s  %lu baud\nTX %lu bytes",
                          mode, (unsigned long)state.baud_rate,
                          (unsigned long)state.tx_bytes);
    view->last_mode = state.mode;
    view->last_tx = state.tx_bytes;
    view->last_connected = connected;
    view->rendered = true;

    if (state.mode != UART_LINK_MODE_DATA) {
        app_set_status(view->status, "UART is busy with STM32 flashing");
    } else if (connected) {
        app_set_status(view->status, "BLE connected, RX writes go to UART2");
    } else {
        app_set_status(view->status, "Waiting for phone: ESP32S3-TOOL");
    }
}

static void live_deleted(lv_event_t *event)
{
    live_uart_view_t *view = lv_event_get_user_data(event);
    ble_data_set_live_uart(false);
    if (view->timer) {
        lv_timer_delete(view->timer);
    }
    lv_free(view);
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    app_request_screen(app_menu_screen);
}

static void live_build(app_screen_t *screen)
{
    ble_data_set_live_uart(true);

    lv_obj_t *column = lv_obj_create(screen->root);
    lv_obj_set_size(column, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, UI_PAD, 0);
    lv_obj_set_style_pad_row(column, UI_GAP, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    app_add_title(column, "BLE LIVE UART2");
    lv_obj_t *status = app_add_status(column);
    lv_label_set_text(status, "Waiting for phone: ESP32S3-TOOL");

    live_uart_view_t *view = lv_malloc(sizeof(*view));
    if (!view) {
        app_set_status(status, "Live view allocation failed");
        ble_data_set_live_uart(false);
        return;
    }
    *view = (live_uart_view_t){.status = status};
    view->stats = app_add_status(column);
    lv_obj_set_style_text_color(view->stats, UI_TEXT, 0);
    lv_label_set_text(view->stats, "READY  115200 baud\nTX 0 bytes");

    lv_obj_t *help = app_add_status(column);
    lv_label_set_text(help, "Phone writes NUS RX directly\nBinary and CR/LF are preserved");

    app_add_button(screen, column, "- STOP / BACK -", back_clicked, NULL);
    view->timer = lv_timer_create(live_poll, UI_RX_POLL_MS, view);
    if (!view->timer) {
        app_set_status(status, "Live status timer allocation failed");
    }
    lv_obj_add_event_cb(screen->root, live_deleted, LV_EVENT_DELETE, view);
    if (view->timer) {
        live_poll(view->timer);
    }
    ESP_LOGI(TAG, "BLE live UART screen active");
}

app_screen_t *app_live_uart_screen(void)
{
    return app_screen_create(live_build);
}
