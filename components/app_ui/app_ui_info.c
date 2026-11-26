/* app_ui_info.c
 *
 * Purpose in the project:
 *   Read-only diagnostics screen: panel resolution, flash and SPIFFS usage, the
 *   record count, fw.bin size and the pin assignment. It answers "what is this
 *   board wired to" without a serial console.
 */

#include "app_ui_internal.h"

#include <stdio.h>

#include "data_store.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "sdkconfig.h"

static void back_clicked(lv_event_t *event)
{
    (void)event;
    app_request_screen(app_menu_screen);
}

static void info_build(app_screen_t *screen)
{
    lv_obj_t *column = lv_obj_create(screen->root);
    lv_obj_set_size(column, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 8, 0);
    lv_obj_set_style_pad_row(column, 3, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    app_add_title(column, "SYSTEM INFO");

    size_t total = 0;
    size_t used = 0;
    data_store_usage(&total, &used);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    lv_display_t *display = lvgl_port_display();
    const int width = display ? (int)lv_display_get_horizontal_resolution(display) : 0;
    const int height = display ? (int)lv_display_get_vertical_resolution(display) : 0;

    lv_obj_t *body = lv_label_create(column);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(body, 1, 0);
    lv_label_set_text_fmt(body,
                          "Panel  %d x %d landscape\n"
                          "Flash  %u KB\n"
                          "SPIFFS %u / %u KB\n"
                          "Record %u  fw %u B\n"
                          "UART2 TX%d RX%d\n"
#if CONFIG_STM32_PROGRAMMER_SWD
                          "UART   %d baud\n"
                          "SWD    DIO%d CLK%d RST%d\n"
#else
                          "Payload %d / ISP %d baud\n"
                          "BOOT0  GPIO%d  NRST GPIO%d\n"
#endif
                          "Heap   %u KB free",
                          width, height, (unsigned)(flash_size / 1024),
                          (unsigned)(used / 1024), (unsigned)(total / 1024),
                          (unsigned)data_store_count(), (unsigned)data_store_fw_size(),
                          CONFIG_UART_LINK_TX_GPIO, CONFIG_UART_LINK_RX_GPIO,
#if CONFIG_STM32_PROGRAMMER_SWD
                          CONFIG_UART_LINK_BAUD, CONFIG_STM32_SWDIO_GPIO,
                          CONFIG_STM32_SWCLK_GPIO, CONFIG_STM32_NRST_GPIO,
#else
                          CONFIG_UART_LINK_BAUD, CONFIG_STM32_UART_BAUD,
                          CONFIG_STM32_BOOT0_GPIO, CONFIG_STM32_NRST_GPIO,
#endif
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    app_add_button(screen, column, "- BACK -", back_clicked, NULL);
}

app_screen_t *app_info_screen(void)
{
    return app_screen_create(info_build);
}
