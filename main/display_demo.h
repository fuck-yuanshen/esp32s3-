#pragma once

#include "st7789_display.h"
#include "button_input.h"

/** Display diagnostics, then move a square with direction keys; OK changes color. */
esp_err_t display_demo_run(st7789_display_handle_t display, button_input_handle_t buttons);
