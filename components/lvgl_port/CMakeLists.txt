idf_component_register(SRCS "lvgl_port.c" "lvgl_port_keys.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl st7789_display button_input
                       PRIV_REQUIRES esp_timer freertos)
