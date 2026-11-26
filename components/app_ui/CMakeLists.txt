idf_component_register(SRCS "app_ui.c" "app_ui_send.c" "app_ui_live_uart.c" "app_ui_flash.c" "app_ui_info.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl_port st7789_display button_input
                       PRIV_REQUIRES data_store ble_data uart_link stm32_isp spi_flash heap)
