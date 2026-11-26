#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct st7789_display *st7789_display_handle_t;

typedef struct {
    spi_host_device_t spi_host;
    int sclk_gpio;
    int mosi_gpio;
    int cs_gpio;              /**< -1: CS is permanently tied low. */
    int dc_gpio;              /**< The module may label this pin A0 or AO. */
    int reset_gpio;           /**< -1: use software reset. */
    int backlight_gpio;       /**< -1: backlight is powered externally. */
    bool backlight_active_low;
    unsigned int clock_hz;
    unsigned int rotation;    /**< 0, 90, 180 or 270 degrees. */
    bool bgr_order;
    bool invert_colors;
} st7789_display_config_t;

/** Owns the selected SPI bus, LEDC low-speed timer 0/channel 0 (if BL enabled).
 *  A successful call initializes a black, visible 240x320/320x240 screen.
 *  Drawing and brightness calls are serialized and block until DMA completes.
 *  Call from tasks, not interrupts. Do not race create/destroy with other calls.
 */
esp_err_t st7789_display_create(const st7789_display_config_t *config,
                               st7789_display_handle_t *out_display);
void st7789_display_destroy(st7789_display_handle_t display);

int st7789_display_width(st7789_display_handle_t display);
int st7789_display_height(st7789_display_handle_t display);

/** Fill a rectangle, clipping at the screen edges; off-screen areas are ignored. */
esp_err_t st7789_display_fill_rect(st7789_display_handle_t display,
                                  int x, int y, int width, int height, uint16_t color);
esp_err_t st7789_display_clear(st7789_display_handle_t display, uint16_t color);

/** Draw an in-bounds rectangle from native uint16_t RGB565 values (e.g. 0xF800).
 *  stride_pixels is the source row length, at least width. The source remains
 *  valid until this blocking call returns; wire byte order is handled internally.
 */
esp_err_t st7789_display_draw_rgb565(st7789_display_handle_t display,
                                    int x, int y, int width, int height,
                                    const uint16_t *pixels, size_t stride_pixels);

/** Set external BL PWM to 0..100%; returns NOT_SUPPORTED for externally powered BL. */
esp_err_t st7789_display_set_brightness(st7789_display_handle_t display, unsigned int percent);

static inline uint16_t st7789_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

#ifdef __cplusplus
}
#endif
