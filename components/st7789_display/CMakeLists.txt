idf_component_register(SRCS "st7789_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc
                       PRIV_REQUIRES heap)
