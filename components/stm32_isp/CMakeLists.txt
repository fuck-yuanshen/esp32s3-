if(CONFIG_STM32_PROGRAMMER_SWD)
    set(programmer_sources "stm32_programmer_swd.c" "swd_wire.c" "swd_gpio.c"
                           "arm_dap.c" "stm32f1_flash.c")
else()
    set(programmer_sources "stm32_isp.c")
endif()
idf_component_register(SRCS ${programmer_sources} "stm32_image.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_uart esp_driver_gpio esp_timer esp_rom data_store uart_link)
