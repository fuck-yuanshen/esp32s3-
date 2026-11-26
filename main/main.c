/* main.c
 *
 * Purpose in the project:
 *   Boot sequence for the offline tool. Order matters:
 *     1. validate the button pin map before the LCD claims its GPIOs;
 *     2. create the panel and the keypad;
 *     3. mount SPIFFS and load data.txt;
 *     4. bring up BLE and the shared payload/STM32 UART on GPIO19/20;
 *     5. hand control to the LVGL application shell.
 */

#include "app_ui.h"
#include "ble_data.h"
#include "board_buttons.h"
#include "board_display.h"
#include "data_store.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "stm32_isp.h"
#include "st7789_display.h"
#include "uart_link.h"

static const char *TAG = "main";

/* NimBLE keeps pairing data in NVS; a fresh or erased NVS must be created here
 * before the host starts or every BLE write fails with ESP_ERR_NVS_NOT_INITIALIZED.
 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());

    const st7789_display_config_t display_config = board_display_config();
    st7789_display_handle_t display = NULL;
    button_input_handle_t buttons = NULL;

    ESP_ERROR_CHECK(board_buttons_create(&display_config, &buttons));

    esp_err_t ret = st7789_display_create(&display_config, &display);
    if (ret != ESP_OK) {
        button_input_destroy(buttons);
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }

    ret = data_store_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storage init failed: %s", esp_err_to_name(ret));
    }

    ret = uart_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "payload UART init failed: %s", esp_err_to_name(ret));
    }

    ret = stm32_isp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STM32 ISP init failed: %s", esp_err_to_name(ret));
    }

    ret = ble_data_init("ESP32S3-TOOL");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(ret));
    }

    ret = app_ui_run(display, buttons);

    /* app_ui_run only returns on a fatal LVGL failure. */
    ESP_LOGE(TAG, "UI stopped: %s", esp_err_to_name(ret));
    st7789_display_destroy(display);
    button_input_destroy(buttons);
}
