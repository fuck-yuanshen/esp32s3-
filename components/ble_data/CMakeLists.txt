idf_component_register(SRCS "ble_data.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt nvs_flash data_store uart_link)
