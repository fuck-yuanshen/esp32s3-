#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "st7789_display.h"
#include "button_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Top-level application shell: initialises the LVGL port, builds the menu and
 * keeps the LVGL task running. Blocks forever unless the port fails.
 */
esp_err_t app_ui_run(st7789_display_handle_t display, button_input_handle_t buttons);

#ifdef __cplusplus
}
#endif
