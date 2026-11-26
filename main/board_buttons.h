#pragma once

#include "button_input.h"
#include "st7789_display.h"

/** Create board-configured buttons after checking for conflicts with LCD pins. */
esp_err_t board_buttons_create(const st7789_display_config_t *display_config,
                               button_input_handle_t *out_buttons);
