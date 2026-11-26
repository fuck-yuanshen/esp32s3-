#pragma once

#include "button_input.h"
#include "esp_err.h"
#include "st7789_display.h"

/** Build a small LVGL UI and run it forever: the keypad moves the focus and OK
 *  activates the focused item. Requires a display created by st7789_display.
 */
esp_err_t lvgl_demo_run(st7789_display_handle_t display, button_input_handle_t buttons);
