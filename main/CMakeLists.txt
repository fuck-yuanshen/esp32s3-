idf_component_register(SRCS "main.c" "board_display.c" "board_buttons.c"
                            "display_demo.c" "demo_font.c" "lvgl_demo.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES st7789_display button_input lvgl_port
                                     data_store ble_data uart_link stm32_isp app_ui
                                     nvs_flash)
