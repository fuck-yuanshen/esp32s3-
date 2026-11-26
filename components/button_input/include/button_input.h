#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_OK,
    BUTTON_COUNT,
} button_id_t;

#define BUTTON_PRESS_EVENT(id) (1u << (unsigned int)(id))

typedef struct {
    int gpio[BUTTON_COUNT];
    unsigned int debounce_ms;
} button_input_config_t;

typedef struct button_input *button_input_handle_t;

/** Configure five independent active-low switches with internal pull-ups.
 *  Each switch connects its GPIO to GND when pressed. GPIOs are owned until
 *  destroy. Poll from one task at intervals of about 5..10 ms; display rendering
 *  can extend that interval, so very short presses may be missed.
 */
esp_err_t button_input_create(const button_input_config_t *config,
                              button_input_handle_t *out_buttons);
void button_input_destroy(button_input_handle_t buttons);

/** Return debounced press edges as BUTTON_PRESS_EVENT bits. Holding a button
 *  does not repeat; a debounced release is required before the next press.
 *  Multiple simultaneous presses can produce multiple bits.
 */
uint32_t button_input_poll(button_input_handle_t buttons);

/** Debounced pressed state as BUTTON_PRESS_EVENT bits without advancing the
 *  debounce timers. Call button_input_poll() first to refresh the state, e.g.
 *  from a periodic timer, then read the mask from another task.
 */
uint32_t button_input_pressed_mask(button_input_handle_t buttons);

#ifdef __cplusplus
}
#endif
