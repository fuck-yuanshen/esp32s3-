#include "board_display.h"
#include "sdkconfig.h"

st7789_display_config_t board_display_config(void)
{
    unsigned int rotation = 0;
#if CONFIG_LCD_ROTATION_90
    rotation = 90;
#elif CONFIG_LCD_ROTATION_180
    rotation = 180;
#elif CONFIG_LCD_ROTATION_270
    rotation = 270;
#endif

    const st7789_display_config_t config = {
        .spi_host = SPI2_HOST,
        .sclk_gpio = CONFIG_LCD_PIN_SCLK,
        .mosi_gpio = CONFIG_LCD_PIN_MOSI,
        .cs_gpio = CONFIG_LCD_PIN_CS,
        .dc_gpio = CONFIG_LCD_PIN_DC,
        .reset_gpio = CONFIG_LCD_PIN_RESET,
        .backlight_gpio = CONFIG_LCD_PIN_BL,
#if CONFIG_LCD_BL_ACTIVE_LOW
        .backlight_active_low = true,
#endif
        .clock_hz = CONFIG_LCD_SPI_CLOCK_MHZ * 1000000u,
        .rotation = rotation,
#if CONFIG_LCD_BGR_ORDER
        .bgr_order = true,
#endif
#if CONFIG_LCD_INVERT_COLORS
        .invert_colors = true,
#endif
    };
    return config;
}
