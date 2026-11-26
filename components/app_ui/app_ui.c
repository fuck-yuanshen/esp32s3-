/* app_ui.c
 *
 * Purpose in the project:
 *   Application shell. Owns the LVGL port, the active screen and the navigation
 *   between screens. Each screen lives in its own file so this one stays a thin
 *   router:
 *       app_ui_send.c   UART payload picker
 *       app_ui_flash.c  STM32 offline ISP
 *       app_ui_info.c   wiring / storage diagnostics
 *
 *   The five-way keypad drives one LVGL group at a time. Every screen creates
 *   its own group and app_screen_show() rebinds the keypad, so focus never leaks
 *   into a screen that is no longer displayed.
 */

#include "app_ui.h"

#include <string.h>

#include "app_ui_internal.h"
#include "ui_theme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"

static const char *TAG = "app_ui";

static app_screen_t *s_active;

/* ------------------------------------------------------------ screen core */

void app_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, UI_BG, 0);
    lv_obj_set_style_text_color(screen, UI_TEXT, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

app_screen_t *app_screen_create(screen_build_fn rebuild)
{
    app_screen_t *screen = lv_malloc(sizeof(*screen));
    if (!screen) {
        return NULL;
    }
    memset(screen, 0, sizeof(*screen));
    screen->rebuild = rebuild;
    screen->root = lv_obj_create(NULL);
    screen->group = lv_group_create();
    if (!screen->root || !screen->group) {
        app_screen_destroy(screen);
        return NULL;
    }
    app_style_screen(screen->root);
    return screen;
}

void app_screen_destroy(app_screen_t *screen)
{
    if (!screen) {
        return;
    }
    /* Screen-owned LVGL timers must stop before their widgets disappear. */
    if (screen->cleanup) {
        screen->cleanup();
        screen->cleanup = NULL;
    }
    if (screen->group) {
        lv_group_delete(screen->group);
        screen->group = NULL;
    }
    if (screen->root) {
        lv_obj_delete(screen->root);
        screen->root = NULL;
    }
    lv_free(screen);
}

void app_screen_show(app_screen_t *screen)
{
    if (!screen || !screen->root) {
        return;
    }
    if (screen->rebuild) {
        screen->rebuild(screen);
    }
    lv_screen_load(screen->root);
    lv_indev_t *indev = lvgl_port_keypad();
    if (indev) {
        lv_indev_set_group(indev, screen->group);
    }
    if (screen->first_focus) {
        lv_group_focus_obj(screen->first_focus);
    }
}

/* --------------------------------------------------------- shared widgets */

lv_obj_t *app_add_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, UI_ACCENT, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

lv_obj_t *app_add_status(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, UI_TEXT_DIM, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

void app_set_status(lv_obj_t *label, const char *text)
{
    if (label) {
        lv_label_set_text(label, text ? text : "");
    }
}

/* Full-width row with a fixed height, so every screen fits the 240 px height of
 * the landscape panel without scrolling the outer column.
 */
lv_obj_t *app_add_button(app_screen_t *screen, lv_obj_t *parent, const char *text,
                         lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, UI_BUTTON_HEIGHT);
    lv_obj_set_style_radius(button, UI_BUTTON_RADIUS, 0);
    lv_obj_set_style_bg_color(button, UI_SURFACE, 0);
    lv_obj_set_style_text_color(button, UI_TEXT, 0);
    lv_obj_set_style_bg_color(button, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, UI_SURFACE_DISABLED, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, UI_TEXT_DIM, LV_STATE_DISABLED);
    lv_obj_set_style_outline_color(button, UI_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(button, UI_BUTTON_OUTLINE, LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(button, UI_BUTTON_OUTLINE, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, UI_ACCENT, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, UI_BUTTON_OUTLINE_PAD, LV_STATE_FOCUS_KEY);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }
    if (screen && screen->group) {
        lv_group_add_obj(screen->group, button);
        if (!screen->first_focus) {
            screen->first_focus = button;
        }
    }
    return button;
}

/* ------------------------------------------------------------ menu screen */

static void menu_clicked(lv_event_t *event)
{
    app_screen_t *(*factory)(void) = lv_event_get_user_data(event);
    app_request_screen(factory);
}

static void menu_build(app_screen_t *screen)
{
    lv_obj_t *column = lv_obj_create(screen->root);
    lv_obj_set_size(column, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 8, 0);
    lv_obj_set_style_pad_row(column, 6, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    app_add_title(column, "ESP32-S3 OFFLINE TOOL");
    lv_obj_t *hint = app_add_status(column);
    lv_label_set_text(hint, "Arrows move   OK select");

    static const struct {
        const char *label;
        app_screen_t *(*factory)(void);
    } entries[] = {
        { "SEND PAYLOAD / UART2", app_send_screen },
        { "BLE LIVE UART2", app_live_uart_screen },
        { "FLASH STM32", app_flash_screen },
        { "SYSTEM INFO", app_info_screen },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        app_add_button(screen, column, entries[i].label, menu_clicked,
                       (void *)entries[i].factory);
    }
}

app_screen_t *app_menu_screen(void)
{
    return app_screen_create(menu_build);
}

/* ------------------------------------------------------------ navigation */

/* Deleting the widget that is currently handling an event would crash LVGL, so
 * the switch is deferred to the next handler iteration. lv_async_call() keeps
 * the request alive until the LVGL task picks it up.
 */
static void switch_screen_async(void *argument)
{
    app_screen_t *(*factory)(void) = (app_screen_t *(*)(void))argument;
    app_screen_t *next = factory();
    if (!next) {
        ESP_LOGE(TAG, "screen allocation failed");
        return;
    }
    app_screen_t *previous = s_active;
    s_active = next;
    app_screen_show(next);
    /* Delete the old screen only after the new one is loaded, so the panel never
     * shows an empty frame between two screens.
     */
    app_screen_destroy(previous);
}

void app_request_screen(app_screen_t *(*factory)(void))
{
    if (!factory) {
        return;
    }
    if (lv_async_call(switch_screen_async, (void *)factory) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "screen request dropped: async queue full");
    }
}

/* ---------------------------------------------------------------- entry */

esp_err_t app_ui_run(st7789_display_handle_t display, button_input_handle_t buttons)
{
    const lvgl_port_config_t config = {
        .display = display,
        .buttons = buttons,
    };
    esp_err_t ret = lvgl_port_init(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_active = app_menu_screen();
    if (!s_active) {
        lvgl_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    app_screen_show(s_active);

    ret = lvgl_port_start();
    if (ret != ESP_OK) {
        lvgl_port_deinit();
        return ret;
    }

    ESP_LOGI(TAG, "UI running at %dx%d", st7789_display_width(display),
             st7789_display_height(display));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "internal heap free: %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
