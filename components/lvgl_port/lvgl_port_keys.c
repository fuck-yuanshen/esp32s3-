#include "lvgl_port_keys.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* The debounce needs a steady sample rate; the LVGL handler period can be longer. */
#define POLL_PERIOD_US 10000

static const char *TAG = "lvgl_port_keys";

static const char *const s_button_names[BUTTON_COUNT] = {
    [BUTTON_UP] = "UP",
    [BUTTON_DOWN] = "DOWN",
    [BUTTON_LEFT] = "LEFT",
    [BUTTON_RIGHT] = "RIGHT",
    [BUTTON_OK] = "OK",
};

static const uint32_t s_key_map[BUTTON_COUNT] = {
    [BUTTON_UP] = LV_KEY_UP,
    [BUTTON_DOWN] = LV_KEY_DOWN,
    [BUTTON_LEFT] = LV_KEY_LEFT,
    [BUTTON_RIGHT] = LV_KEY_RIGHT,
    [BUTTON_OK] = LV_KEY_ENTER,
};

static struct {
    button_input_handle_t buttons;
    esp_timer_handle_t timer;
    lv_indev_t *indev;
    lv_group_t *group;
    volatile uint32_t pressed;
    uint32_t last_key;
    int last_button;
} s_keys;

/* LVGL 9 moves group focus only for LV_KEY_NEXT / LV_KEY_PREV. Any other key,
 * including the arrow keys, is forwarded to the focused widget as LV_EVENT_KEY.
 * Plain buttons and labels ignore that event, so a five-way keypad would only
 * appear to work through its OK key. Remapping the arrows to PREV/NEXT keeps
 * every screen navigable with the same hardware.
 */
static lv_key_t arrow_nav_remap(lv_indev_t *indev, lv_key_t key)
{
    (void)indev;
#if CONFIG_LVGL_PORT_ARROW_NAV
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_LEFT:
        return LV_KEY_PREV;
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT:
        return LV_KEY_NEXT;
    default:
        break;
    }
#endif
    return key;
}

/* Sampling runs from a periodic timer so the debounce window is measured with a
 * stable period. Edge events are logged here because this is the only place
 * that observes them: the log separates a wiring fault from a UI fault.
 */
static void poll_timer_cb(void *argument)
{
    (void)argument;
    const uint32_t events = button_input_poll(s_keys.buttons);
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        if (events & BUTTON_PRESS_EVENT(i)) {
            s_keys.last_button = i;
            s_keys.last_key = s_key_map[i];
            ESP_LOGI(TAG, "%s pressed (press #%d)", s_button_names[i], i + 1);
        }
    }
    s_keys.pressed = button_input_pressed_mask(s_keys.buttons);
}

/* Called from the LVGL task. The key reported on release is kept equal to the
 * last pressed key; LVGL substitutes the previous key itself when it processes
 * the release, so reporting a stale key here cannot lose the click.
 */
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const uint32_t mask = s_keys.pressed;
    if (mask) {
        int active = s_keys.last_button;
        if (active < 0 || active >= BUTTON_COUNT ||
            !(mask & BUTTON_PRESS_EVENT(active))) {
            active = -1;
            for (int i = 0; i < BUTTON_COUNT; ++i) {
                if (mask & BUTTON_PRESS_EVENT(i)) {
                    active = i;
                    break;
                }
            }
        }
        if (active >= 0) {
            s_keys.last_key = s_key_map[active];
        }
        data->key = s_keys.last_key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key = s_keys.last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t lvgl_port_keys_init(button_input_handle_t buttons)
{
    if (!buttons) {
        return ESP_ERR_INVALID_ARG;
    }
    s_keys.buttons = buttons;
    s_keys.last_key = LV_KEY_ENTER;
    s_keys.last_button = -1;

    s_keys.group = lv_group_create();
    s_keys.indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_keys.group && s_keys.indev, ESP_ERR_NO_MEM, TAG,
                        "create LVGL group and input device");
    lv_indev_set_type(s_keys.indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_keys.indev, keypad_read_cb);
    lv_indev_set_key_remap_cb(s_keys.indev, arrow_nav_remap);
    lv_indev_set_group(s_keys.indev, s_keys.group);
    lv_group_set_default(s_keys.group);

    /* Report the idle levels once: a pin that already reads low before any key
     * is touched points at a wiring or board conflict rather than at LVGL.
     */
    const uint32_t idle = button_input_pressed_mask(buttons);
    if (idle) {
        for (int i = 0; i < BUTTON_COUNT; ++i) {
            if (idle & BUTTON_PRESS_EVENT(i)) {
                ESP_LOGW(TAG, "%s reads low at startup: check that its GPIO is not "
                              "shorted to GND or driven by a board function",
                         s_button_names[i]);
            }
        }
    }

    const esp_timer_create_args_t args = {
        .callback = poll_timer_cb,
        .name = "lvgl_buttons",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_keys.timer), TAG, "create poll timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_keys.timer, POLL_PERIOD_US), TAG,
                        "start poll timer");
    ESP_LOGI(TAG, "keypad ready: polling every %d ms, arrows=%s", POLL_PERIOD_US / 1000,
             CONFIG_LVGL_PORT_ARROW_NAV ? "focus next/prev" : "sent to widget");
    return ESP_OK;
}

void lvgl_port_keys_deinit(void)
{
    if (s_keys.timer) {
        esp_timer_stop(s_keys.timer);
        esp_timer_delete(s_keys.timer);
        s_keys.timer = NULL;
    }
    if (s_keys.indev) {
        lv_indev_delete(s_keys.indev);
        s_keys.indev = NULL;
    }
    if (s_keys.group) {
        lv_group_delete(s_keys.group);
        s_keys.group = NULL;
    }
    s_keys.buttons = NULL;
    s_keys.pressed = 0;
}

lv_indev_t *lvgl_port_keys_indev(void)
{
    return s_keys.indev;
}
