#pragma once

#include "button_input.h"
#include "esp_err.h"
#include "lvgl.h"

/* Private to the lvgl_port component. */
esp_err_t lvgl_port_keys_init(button_input_handle_t buttons);
void lvgl_port_keys_deinit(void);
lv_indev_t *lvgl_port_keys_indev(void);
