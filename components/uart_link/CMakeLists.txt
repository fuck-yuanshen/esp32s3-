idf_component_register(SRCS "uart_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_uart esp_driver_gpio)
