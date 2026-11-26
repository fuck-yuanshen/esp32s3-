#include "lvgl_demo.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_port.h"

static const char *TAG = "lvgl_demo";

#define STATUS_PERIOD_MS 30000

typedef struct {
    const char *name;
    uint32_t rgb;
} color_action_t;

static const color_action_t s_actions[] = {
    {"RED", 0xFF0000},
    {"GREEN", 0x00FF00},
    {"BLUE", 0x0000FF},
};

static lv_obj_t *s_preview;
static lv_obj_t *s_bar;
static lv_obj_t *s_bar_label;
static lv_obj_t *s_status;
static unsigned int s_activations;

static void bar_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int32_t value = (lv_bar_get_value(s_bar) + 2) % 101;
    lv_bar_set_value(s_bar, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bar_label, "%d%%", (int)value);
}

/* Focus feedback exists so a keypad wiring problem and a focus problem can be
 * told apart from the panel: the serial log shows the raw button press, and
 * this handler shows whether LVGL moved the group focus afterwards.
 */
static void focus_event_cb(lv_event_t *event)
{
    const color_action_t *action = lv_event_get_user_data(event);
    ESP_LOGI(TAG, "focus -> %s", action->name);
    /* Adding the first object focuses it immediately, before the status label
     * exists; the label is refreshed again after the UI is complete.
     */
    if (s_status) {
        lv_label_set_text_fmt(s_status, "Focus: %s", action->name);
    }
}

static void action_event_cb(lv_event_t *event)
{
    const color_action_t *action = lv_event_get_user_data(event);
    lv_obj_set_style_bg_color(s_preview, lv_color_hex(action->rgb), 0);
    s_activations++;
    lv_label_set_text_fmt(s_status, "OK -> %s   (%u)", action->name, s_activations);
    ESP_LOGI(TAG, "%s activated, count=%u", action->name, s_activations);
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *add_action_button(lv_obj_t *parent, lv_group_t *group, size_t index,
                                   int x, int y, int width, int height)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(s_actions[index].rgb), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, lv_color_white(), LV_STATE_FOCUS_KEY);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, s_actions[index].name);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, (void *)&s_actions[index]);
    lv_obj_add_event_cb(button, focus_event_cb, LV_EVENT_FOCUSED, (void *)&s_actions[index]);
    if (group) {
        lv_group_add_obj(group, button);
    }
    return button;
}

/* Landscape (320x240 after a 90 or 270 degree rotation) places the preview and
 * progress bar in a left column and the action buttons in a right column, so
 * the whole UI fits the shorter height. Portrait keeps the single column that
 * the 240x320 panel needs. Both branches share the same widgets and callbacks,
 * so changing the rotation setting never changes behaviour.
 */
static void build_ui(int width, int height)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    char title[48];
    lv_snprintf(title, sizeof(title), "LVGL %d.%d on ESP32-S3", LVGL_VERSION_MAJOR,
                LVGL_VERSION_MINOR);
    char subtitle[48];
    lv_snprintf(subtitle, sizeof(subtitle), "ST7789 %dx%d %s", width, height,
                width > height ? "landscape" : "portrait");

    lv_group_t *group = lvgl_port_keypad() ? lv_indev_get_group(lvgl_port_keypad()) : NULL;
    const bool landscape = width > height;
    lv_obj_t *first = NULL;

    if (landscape) {
        add_label(screen, title, 0, 4, width);
        add_label(screen, subtitle, 0, 22, width);

        s_preview = lv_obj_create(screen);
        lv_obj_set_pos(s_preview, 8, 48);
        lv_obj_set_size(s_preview, 140, 62);
        lv_obj_set_style_radius(s_preview, 8, 0);
        lv_obj_set_style_border_width(s_preview, 2, 0);
        lv_obj_set_style_border_color(s_preview, lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_preview, lv_color_hex(0x404040), 0);
        lv_obj_remove_flag(s_preview, LV_OBJ_FLAG_SCROLLABLE);

        s_bar = lv_bar_create(screen);
        lv_obj_set_pos(s_bar, 8, 120);
        lv_obj_set_size(s_bar, 140, 16);
        lv_bar_set_range(s_bar, 0, 100);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

        s_bar_label = add_label(screen, "0%", 8, 140, 140);
        add_label(screen, "Arrows: focus\nOK: activate", 8, 162, 140);

        for (size_t i = 0; i < sizeof(s_actions) / sizeof(s_actions[0]); ++i) {
            lv_obj_t *button = add_action_button(screen, group, i, 160, 48 + (int)i * 50, 152, 42);
            if (!first) {
                first = button;
            }
        }
        s_status = add_label(screen, "Focus: RED", 0, height - 24, width);
    } else {
        add_label(screen, title, 0, 6, width);
        add_label(screen, subtitle, 0, 26, width);

        s_preview = lv_obj_create(screen);
        lv_obj_set_pos(s_preview, 20, 50);
        lv_obj_set_size(s_preview, 200, 50);
        lv_obj_set_style_radius(s_preview, 8, 0);
        lv_obj_set_style_border_width(s_preview, 2, 0);
        lv_obj_set_style_border_color(s_preview, lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_preview, lv_color_hex(0x404040), 0);
        lv_obj_remove_flag(s_preview, LV_OBJ_FLAG_SCROLLABLE);

        s_bar = lv_bar_create(screen);
        lv_obj_set_pos(s_bar, 20, 110);
        lv_obj_set_size(s_bar, 200, 16);
        lv_bar_set_range(s_bar, 0, 100);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

        s_bar_label = add_label(screen, "0%", 0, 132, width);
        add_label(screen, "Arrows: focus     OK: activate", 0, 152, width);

        for (size_t i = 0; i < sizeof(s_actions) / sizeof(s_actions[0]); ++i) {
            lv_obj_t *button = add_action_button(screen, group, i, 20, 174 + (int)i * 42, 200, 36);
            if (!first) {
                first = button;
            }
        }
        s_status = add_label(screen, "Focus: RED", 0, height - 22, width);
    }

    if (group && first) {
        lv_group_focus_obj(first);
        lv_label_set_text(s_status, "Focus: RED");
    }
}

esp_err_t lvgl_demo_run(st7789_display_handle_t display, button_input_handle_t buttons)
{
    const lvgl_port_config_t config = {
        .display = display,
        .buttons = buttons,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&config), TAG, "initialise the LVGL port");
    build_ui(st7789_display_width(display), st7789_display_height(display));
    lv_timer_create(bar_timer_cb, 120, NULL);
    ESP_RETURN_ON_ERROR(lvgl_port_start(), TAG, "start the LVGL task");
    ESP_LOGI(TAG, "internal free heap after LVGL: %u bytes (minimum %u)",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
        ESP_LOGI(TAG, "heap=%u B, flushed areas=%u, slowest flush=%.2f ms",
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)lvgl_port_flush_count(),
                 (double)lvgl_port_flush_max_us() / 1000.0);
    }
}
