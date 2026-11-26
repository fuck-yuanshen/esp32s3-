/* app_ui_flash.c
 *
 * Purpose in the project:
 *   "Flash STM32" screen. It reports whether a BIN/Intel HEX image exists, how
 *   large it is, then starts the offline programmer. The LVGL task polls
 *   progress. Only the legacy USART backend needs a manual erase-page size.
 */

#include "app_ui_internal.h"

#include <stdio.h>

#include "data_store.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "stm32_isp.h"

static const char *TAG = "app_ui_flash";

#if !CONFIG_STM32_PROGRAMMER_SWD
static const uint32_t s_page_sizes[] = {128, 256, 1024, 2048};
static const char *const s_page_names[] = {"128 B", "256 B", "1 KB", "2 KB"};
#define PAGE_COUNT (sizeof(s_page_sizes) / sizeof(s_page_sizes[0]))

static size_t s_page_index = PAGE_COUNT - 1;   /* default 2 KB */
static lv_obj_t *s_page_button;
#endif

static lv_obj_t *s_status;
static lv_obj_t *s_bar;
static lv_obj_t *s_bar_label;
static lv_obj_t *s_start_button;
static lv_obj_t *s_back_button;
static lv_timer_t *s_poll_timer;
#if CONFIG_STM32_SWD_MANUAL_RESET
static stm32_isp_reset_prompt_t s_displayed_prompt;
#endif

static void back_clicked(lv_event_t *event)
{
    (void)event;
    app_request_screen(app_menu_screen);
}

#if !CONFIG_STM32_PROGRAMMER_SWD
static void page_clicked(lv_event_t *event)
{
    (void)event;
    s_page_index = (s_page_index + 1) % PAGE_COUNT;
    lv_label_set_text_fmt(lv_obj_get_child(s_page_button, 0), "PAGE %s",
                          s_page_names[s_page_index]);
    ESP_LOGI(TAG, "erase page size %u", (unsigned)s_page_sizes[s_page_index]);
}
#endif

static void start_clicked(lv_event_t *event)
{
    (void)event;
#if CONFIG_STM32_SWD_MANUAL_RESET
    if (s_displayed_prompt != STM32_ISP_RESET_NONE) {
        if (stm32_isp_confirm_reset(s_displayed_prompt) == ESP_OK) {
            lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
        }
        return;
    }
#endif
    if (stm32_isp_busy()) return;
    if (!data_store_fw_exists()) {
        app_set_status(s_status, "No firmware: upload BIN or HEX over BLE first");
        return;
    }
#if CONFIG_STM32_PROGRAMMER_SWD
    const uint32_t page_size = 0;
#else
    const uint32_t page_size = s_page_sizes[s_page_index];
#endif
    const esp_err_t ret = stm32_isp_start(DATA_STORE_FW_PATH, 0, page_size, true);
    if (ret == ESP_OK) {
        app_set_status(s_status, "Flashing, keep the STM32 powered");
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_back_button, LV_STATE_DISABLED);
    } else {
        app_set_status(s_status, esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "flash start: %s", esp_err_to_name(ret));
}

#if CONFIG_STM32_SWD_MANUAL_RESET
/* Reuse the focused action button to acknowledge each worker prompt. */
static void render_reset_prompt(stm32_isp_reset_prompt_t prompt)
{
    if (prompt != s_displayed_prompt) {
        s_displayed_prompt = prompt;
        const char *action = "START FLASHING";
        if (prompt == STM32_ISP_RESET_HOLD) action = "RESET HELD";
        if (prompt == STM32_ISP_RESET_RELEASE) action = "RESET RELEASED";
        lv_label_set_text(lv_obj_get_child(s_start_button, 0), action);
    }
    if (prompt == STM32_ISP_RESET_NONE) return;
    const char *instruction = prompt == STM32_ISP_RESET_HOLD
                            ? "Hold STM32 RESET, then press OK"
                            : "Release STM32 RESET, then press OK";
    app_set_status(s_status, instruction);
    lv_label_set_text(s_bar_label, "30 s to confirm");
    lv_obj_remove_state(s_start_button, LV_STATE_DISABLED);
    lv_group_focus_obj(s_start_button);
}
#endif

/* Runs in the LVGL task. Reading the job snapshot only takes the job mutex, so
 * it never blocks the renderer for long.
 */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    stm32_isp_status_t status;
    stm32_isp_status(&status);

    lv_bar_set_value(s_bar, status.percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bar_label, "%u%%  %s", (unsigned)status.percent,
                          status.message);

#if CONFIG_STM32_SWD_MANUAL_RESET
    render_reset_prompt(status.reset_prompt);
    if (status.reset_prompt != STM32_ISP_RESET_NONE) return;
#endif
    switch (status.state) {
    case STM32_ISP_RUNNING:
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_back_button, LV_STATE_DISABLED);
        if (status.target_id) {
            lv_label_set_text_fmt(s_status, "ID 0x%03X  page %u B",
                                  (unsigned)status.target_id,
                                  (unsigned)status.page_size);
        } else {
            app_set_status(s_status, "Running, do not unplug");
        }
        break;
    case STM32_ISP_DONE:
        lv_label_set_text_fmt(s_status, "Verified, ID 0x%03X",
                              (unsigned)status.target_id);
#if CONFIG_STM32_SWD_MANUAL_RESET
        lv_label_set_text(s_bar_label, "100%  Tap STM32 RESET to run");
#endif
        lv_obj_remove_state(s_start_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_back_button, LV_STATE_DISABLED);
        break;
    case STM32_ISP_FAILED:
        app_set_status(s_status, "Flashing failed");
        lv_obj_remove_state(s_start_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_back_button, LV_STATE_DISABLED);
        break;
    default:
        break;
    }
}

static void flash_cleanup(void)
{
    if (s_poll_timer) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
}

static void flash_build(app_screen_t *screen)
{
    lv_obj_t *column = lv_obj_create(screen->root);
    lv_obj_set_size(column, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 6, 0);
    lv_obj_set_style_pad_row(column, 4, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

#if CONFIG_STM32_PROGRAMMER_SWD
    app_add_title(column, "STM32 OFFLINE SWD");
#else
    app_add_title(column, "STM32 OFFLINE ISP");
#endif

    lv_obj_t *file = app_add_status(column);
    const size_t size = data_store_fw_size();
    if (size) {
        lv_label_set_text_fmt(file, "Image %u B (BIN/HEX)",
                              (unsigned)size);
    } else {
        lv_label_set_text(file, "No BIN/HEX image: upload over BLE");
    }

    s_status = app_add_status(column);
#if CONFIG_STM32_SWD_MANUAL_RESET
    s_displayed_prompt = STM32_ISP_RESET_NONE;
    lv_label_set_text_fmt(s_status, "DIO=%d CLK=%d | Manual RESET",
                         CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO);
#elif CONFIG_STM32_PROGRAMMER_SWD && CONFIG_STM32_SWD_WIRED_RESET
    lv_label_set_text_fmt(s_status, "DIO=%d CLK=%d NRST=%d",
                         CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO,
                         CONFIG_STM32_NRST_GPIO);
#elif CONFIG_STM32_PROGRAMMER_SWD
    lv_label_set_text_fmt(s_status, "DIO=%d CLK=%d | AUTO RESET",
                         CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO);
#else
    lv_label_set_text_fmt(s_status, "BOOT0=%d NRST=%d",
                         CONFIG_STM32_BOOT0_GPIO, CONFIG_STM32_NRST_GPIO);
#endif

    s_bar = lv_bar_create(column);
    lv_obj_set_width(s_bar, lv_pct(100));
    lv_obj_set_height(s_bar, 14);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_bar_label = lv_label_create(column);
    lv_obj_set_width(s_bar_label, lv_pct(100));
    lv_label_set_long_mode(s_bar_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_bar_label, "idle");
    lv_obj_set_style_text_font(s_bar_label, &lv_font_montserrat_14, 0);

#if !CONFIG_STM32_PROGRAMMER_SWD
    s_page_button = app_add_button(screen, column, "", page_clicked, NULL);
    lv_label_set_text_fmt(lv_obj_get_child(s_page_button, 0), "PAGE %s",
                          s_page_names[s_page_index]);
#endif

    s_start_button = app_add_button(screen, column, "START FLASHING", start_clicked, NULL);
    s_back_button = app_add_button(screen, column, "- BACK -", back_clicked, NULL);

    s_poll_timer = lv_timer_create(poll_timer_cb, 250, NULL);
    if (stm32_isp_busy()) {
        lv_obj_add_state(s_start_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_back_button, LV_STATE_DISABLED);
    }
    screen->cleanup = flash_cleanup;
}

app_screen_t *app_flash_screen(void)
{
    return app_screen_create(flash_build);
}
