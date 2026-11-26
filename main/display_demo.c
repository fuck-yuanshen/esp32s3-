#include "display_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include "demo_font.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_demo";

static esp_err_t show_color_test(st7789_display_handle_t display)
{
    const struct { const char *name; uint16_t color; } tests[] = {
        {"RED", 0xF800}, {"GREEN", 0x07E0}, {"BLUE", 0x001F},
        {"WHITE", 0xFFFF}, {"BLACK", 0x0000},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        ESP_LOGI(TAG, "Full-screen %s (RGB565 0x%04X)", tests[i].name, tests[i].color);
        ESP_RETURN_ON_ERROR(st7789_display_clear(display, tests[i].color), TAG, "clear screen");
        ESP_RETURN_ON_ERROR(demo_font_draw(display, 16, 16, tests[i].name, 3,
                            i == 4 ? 0xFFFF : 0x0000), TAG, "draw color label");
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    return ESP_OK;
}

static esp_err_t draw_gradient(st7789_display_handle_t display, int y, int height)
{
    const int width = st7789_display_width(display) - 16;
    uint16_t *row = malloc(width * sizeof(*row));
    ESP_RETURN_ON_FALSE(row, ESP_ERR_NO_MEM, TAG, "allocate gradient row");
    esp_err_t ret = ESP_OK;
    for (int line = 0; line < height && ret == ESP_OK; ++line) {
        for (int x = 0; x < width; ++x) {
            row[x] = st7789_rgb565(x * 255 / (width - 1), line * 255 / (height - 1),
                                   255 - x * 255 / (width - 1));
        }
        ret = st7789_display_draw_rgb565(display, 8, y + line, width, 1, row, width);
    }
    free(row);
    return ret;
}

static esp_err_t show_geometry_test(st7789_display_handle_t display)
{
    const int width = st7789_display_width(display);
    const int height = st7789_display_height(display);
    ESP_RETURN_ON_ERROR(st7789_display_clear(display, 0x0000), TAG, "clear pattern");
    ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, 0, 0, width, 1, 0xFFFF), TAG, "top border");
    ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, 0, height - 1, width, 1, 0xFFFF), TAG, "bottom border");
    ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, 0, 0, 1, height, 0xFFFF), TAG, "left border");
    ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, width - 1, 0, 1, height, 0xFFFF), TAG, "right border");
    ESP_RETURN_ON_ERROR(demo_font_draw(display, 12, 12, "ST7789V", 3, 0xFFFF), TAG, "title");
    char label[32];
    snprintf(label, sizeof(label), "ESP32 S3 %dX%d", width, height);
    ESP_RETURN_ON_ERROR(demo_font_draw(display, 12, 40, label, 2, 0x07FF), TAG, "dimensions");
    const uint16_t colors[] = {0xF800, 0x07E0, 0x001F};
    const char *labels[] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i) {
        int left = 8 + i * (width - 16) / 3;
        int right = 8 + (i + 1) * (width - 16) / 3;
        ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, left, 68, right - left, 40, colors[i]),
                            TAG, "color bars");
        ESP_RETURN_ON_ERROR(demo_font_draw(display, left + 8, 78, labels[i], 3, 0xFFFF), TAG, "bar label");
    }
    ESP_RETURN_ON_ERROR(draw_gradient(display, 120, 40), TAG, "gradient image");
    ESP_RETURN_ON_ERROR(demo_font_draw(display, 12, 174, "RGB565 SPI DMA", 2, 0xFFFF), TAG, "mode label");
    ESP_LOGI(TAG, "Expect a complete white border, RGB bars and a smooth gradient");
    return ESP_OK;
}

esp_err_t display_demo_run(st7789_display_handle_t display, button_input_handle_t buttons)
{
    if (!display || !buttons) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(show_color_test(display), TAG, "color diagnostics");
    ESP_RETURN_ON_ERROR(show_geometry_test(display), TAG, "geometry diagnostics");
    const int width = st7789_display_width(display);
    const int height = st7789_display_height(display);
    const int max_x = width - 8 - 18;
    /* The diagnostics above occupy the top 200 rows in portrait and about 180
     * rows in landscape; the play area starts below whichever is larger so the
     * square never covers them, and stays inside the visible panel.
     */
    int min_y = width > height ? 190 : 200;
    if (min_y > height - 8 - 18) {
        min_y = height / 2;
    }
    const int max_y = height - 8 - 18;
    const uint16_t square_colors[] = {0xFFE0, 0x07FF, 0xF81F, 0xFFFF};
    const char *button_names[] = {"UP", "DOWN", "LEFT", "RIGHT", "OK"};
    unsigned int color_index = 0;
    int x = 8;
    int y = min_y;
    ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, x, y, 18, 18, square_colors[color_index]),
                        TAG, "draw controlled square");
    ESP_LOGI(TAG, "Direction buttons move the square; OK changes its color");
    for (;;) {
        const uint32_t events = button_input_poll(buttons);
        if (events) {
            ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, x, y, 18, 18, 0), TAG, "erase square");
            for (int i = 0; i < BUTTON_COUNT; ++i) {
                if (events & BUTTON_PRESS_EVENT(i)) {
                    ESP_LOGI(TAG, "Button %s pressed", button_names[i]);
                }
            }
            if (events & BUTTON_PRESS_EVENT(BUTTON_UP)) { y -= 10; }
            if (events & BUTTON_PRESS_EVENT(BUTTON_DOWN)) { y += 10; }
            if (events & BUTTON_PRESS_EVENT(BUTTON_LEFT)) { x -= 10; }
            if (events & BUTTON_PRESS_EVENT(BUTTON_RIGHT)) { x += 10; }
            if (events & BUTTON_PRESS_EVENT(BUTTON_OK)) {
                color_index = (color_index + 1) % (sizeof(square_colors) / sizeof(square_colors[0]));
            }
            x = x < 8 ? 8 : (x > max_x ? max_x : x);
            y = y < min_y ? min_y : (y > max_y ? max_y : y);
            ESP_RETURN_ON_ERROR(st7789_display_fill_rect(display, x, y, 18, 18, square_colors[color_index]),
                                TAG, "draw controlled square");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
