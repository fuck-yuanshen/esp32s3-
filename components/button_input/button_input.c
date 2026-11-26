#include "button_input.h"

#include <stdbool.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_timer.h"

struct button_state {
    bool candidate_pressed;
    bool stable_pressed;
    int64_t changed_at_us;
};

struct button_input {
    button_input_config_t config;
    struct button_state states[BUTTON_COUNT];
};

esp_err_t button_input_create(const button_input_config_t *config,
                              button_input_handle_t *out_buttons)
{
    if (!out_buttons) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buttons = NULL;
    if (!config || config->debounce_ms < 10 || config->debounce_ms > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t pin_mask = 0;
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        /* Keep GPIO46 free for its boot-strapping role on ESP32-S3. */
        if (!GPIO_IS_VALID_GPIO(config->gpio[i]) || config->gpio[i] == 46) {
            return ESP_ERR_INVALID_ARG;
        }
        const uint64_t bit = 1ULL << config->gpio[i];
        if (pin_mask & bit) {
            return ESP_ERR_INVALID_ARG;
        }
        pin_mask |= bit;
    }
    struct button_input *buttons = calloc(1, sizeof(*buttons));
    if (!buttons) {
        return ESP_ERR_NO_MEM;
    }
    buttons->config = *config;
    const gpio_config_t pins = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pins);
    if (ret != ESP_OK) {
        free(buttons);
        return ret;
    }
    const int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        buttons->states[i].candidate_pressed = gpio_get_level(config->gpio[i]) == 0;
        buttons->states[i].changed_at_us = now_us;
    }
    *out_buttons = buttons;
    return ESP_OK;
}

void button_input_destroy(button_input_handle_t buttons)
{
    if (!buttons) {
        return;
    }
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        gpio_reset_pin(buttons->config.gpio[i]);
    }
    free(buttons);
}

uint32_t button_input_pressed_mask(button_input_handle_t buttons)
{
    if (!buttons) {
        return 0;
    }
    uint32_t mask = 0;
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        if (buttons->states[i].stable_pressed) {
            mask |= BUTTON_PRESS_EVENT(i);
        }
    }
    return mask;
}

uint32_t button_input_poll(button_input_handle_t buttons)
{
    if (!buttons) {
        return 0;
    }
    uint32_t events = 0;
    const int64_t now_us = esp_timer_get_time();
    const int64_t debounce_us = buttons->config.debounce_ms * 1000LL;
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        struct button_state *state = &buttons->states[i];
        const bool pressed = gpio_get_level(buttons->config.gpio[i]) == 0;
        if (pressed != state->candidate_pressed) {
            state->candidate_pressed = pressed;
            state->changed_at_us = now_us;
        }
        if (state->candidate_pressed != state->stable_pressed &&
            now_us - state->changed_at_us >= debounce_us) {
            state->stable_pressed = state->candidate_pressed;
            if (state->stable_pressed) {
                events |= BUTTON_PRESS_EVENT(i);
            }
        }
    }
    return events;
}
