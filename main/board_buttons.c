#include "board_buttons.h"

#include "esp_log.h"
#include "sdkconfig.h"

esp_err_t board_buttons_create(const st7789_display_config_t *display_config,
                               button_input_handle_t *out_buttons)
{
    if (!out_buttons) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buttons = NULL;
    if (!display_config) {
        return ESP_ERR_INVALID_ARG;
    }
    const button_input_config_t config = {
        .gpio = {
            [BUTTON_UP] = CONFIG_BUTTON_PIN_UP,
            [BUTTON_DOWN] = CONFIG_BUTTON_PIN_DOWN,
            [BUTTON_LEFT] = CONFIG_BUTTON_PIN_LEFT,
            [BUTTON_RIGHT] = CONFIG_BUTTON_PIN_RIGHT,
            [BUTTON_OK] = CONFIG_BUTTON_PIN_OK,
        },
        .debounce_ms = CONFIG_BUTTON_DEBOUNCE_MS,
    };
    const int lcd_pins[] = {
        display_config->sclk_gpio, display_config->mosi_gpio,
        display_config->cs_gpio, display_config->dc_gpio,
        display_config->reset_gpio, display_config->backlight_gpio,
    };
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        for (size_t j = 0; j < sizeof(lcd_pins) / sizeof(lcd_pins[0]); ++j) {
            if (config.gpio[i] == lcd_pins[j]) {
                ESP_LOGE("board_buttons", "Button %d conflicts with LCD GPIO%d", i, config.gpio[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    esp_err_t ret = button_input_create(&config, out_buttons);
    if (ret == ESP_OK) {
        ESP_LOGI("board_buttons", "UP=%d DOWN=%d LEFT=%d RIGHT=%d OK=%d, active low, debounce=%u ms",
                 config.gpio[BUTTON_UP], config.gpio[BUTTON_DOWN], config.gpio[BUTTON_LEFT],
                 config.gpio[BUTTON_RIGHT], config.gpio[BUTTON_OK], config.debounce_ms);
    }
    return ret;
}
