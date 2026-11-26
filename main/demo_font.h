#pragma once

#include "st7789_display.h"

/** Draw an uppercase ASCII label with a transparent 5x7 bitmap font. */
esp_err_t demo_font_draw(st7789_display_handle_t display, int x, int y,
                         const char *text, unsigned int scale, uint16_t color);
