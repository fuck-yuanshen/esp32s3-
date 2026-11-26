#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "button_input.h"
#include "esp_err.h"
#include "lvgl.h"
#include "st7789_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Block indefinitely in lvgl_port_lock(). */
#define LVGL_PORT_WAIT_FOREVER UINT32_MAX

typedef struct {
    st7789_display_handle_t display;   /**< Panel created by st7789_display_create(). */
    button_input_handle_t buttons;     /**< NULL: do not create a keypad input device. */
    unsigned int buffer_lines;         /**< Override the Kconfig buffer height, 0 = default. */
} lvgl_port_config_t;

/** Initialise LVGL, bind it to the panel and create a 5-way keypad input device.
 *  Task priority, stack size and core come from menuconfig (LVGL port menu).
 *  Build the UI after this call and before lvgl_port_start(); afterwards every
 *  LVGL call must be wrapped in lvgl_port_lock()/lvgl_port_unlock().
 *  This does not touch the SPI bus itself: flushing goes through the display
 *  driver, so one task may own both the panel and LVGL.
 */
esp_err_t lvgl_port_init(const lvgl_port_config_t *config);
void lvgl_port_deinit(void);

/** Start the task that calls lv_timer_handler(). Call it once the UI exists. */
esp_err_t lvgl_port_start(void);

lv_display_t *lvgl_port_display(void);
lv_indev_t *lvgl_port_keypad(void);

/** Recursive lock around LVGL. Returns false on timeout or before init. */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

/** Diagnostics: number of flushed areas and the slowest one, in microseconds. */
uint32_t lvgl_port_flush_count(void);
uint32_t lvgl_port_flush_max_us(void);

#ifdef __cplusplus
}
#endif
