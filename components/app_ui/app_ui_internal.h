#pragma once

#include "lvgl.h"

/* Internal plumbing shared by the app_ui translation units.
 *
 * Every screen owns one LVGL group. The keypad input device is bound to exactly
 * one group at a time, so focus cannot leak between screens and a screen can be
 * destroyed without disturbing the others.
 */

typedef struct app_screen app_screen_t;
typedef void (*screen_build_fn)(app_screen_t *screen);

struct app_screen {
    lv_obj_t *root;
    lv_group_t *group;
    lv_obj_t *first_focus;
    screen_build_fn rebuild;
    /* Optional teardown for screen-owned LVGL timers. It runs before the widget
     * tree is deleted so a timer can never touch a freed object.
     */
    void (*cleanup)(void);
};

app_screen_t *app_screen_create(screen_build_fn rebuild);
void app_screen_destroy(app_screen_t *screen);
void app_screen_show(app_screen_t *screen);

/* Screen factories. */
app_screen_t *app_menu_screen(void);
app_screen_t *app_send_screen(void);
app_screen_t *app_live_uart_screen(void);
app_screen_t *app_flash_screen(void);
app_screen_t *app_info_screen(void);

/* Shared widget helpers. app_add_button also registers the button in the screen
 * group; the first button added becomes the initial focus target.
 */
void app_style_screen(lv_obj_t *screen);
lv_obj_t *app_add_title(lv_obj_t *parent, const char *text);
lv_obj_t *app_add_status(lv_obj_t *parent);
lv_obj_t *app_add_button(app_screen_t *screen, lv_obj_t *parent, const char *text,
                         lv_event_cb_t cb, void *user_data);
void app_set_status(lv_obj_t *label, const char *text);

/* Replace the active screen. Safe inside an LVGL event callback: the request is
 * queued and executed by an LVGL async call.
 */
void app_request_screen(app_screen_t *(*factory)(void));
