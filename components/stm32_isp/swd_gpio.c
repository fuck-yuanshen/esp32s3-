#include "swd_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

static const char *TAG = "swd_gpio";

/* Retain both half cycles of the first 64 clocks. Each sample reports actual
 * CLK/DIO pad levels and GPIO output enable, rather than the output latch. */
#define TRACE_SAMPLES 128U
static struct {
    bool active, high;
    unsigned count, clock_mismatches;
    uint8_t samples[TRACE_SAMPLES];
} s_trace;

static bool data_output_enabled(void)
{
    const unsigned pin = CONFIG_STM32_SWDIO_GPIO;
    const uint32_t enabled = pin < 32 ? GPIO.enable : GPIO.enable1.val;
    return ((enabled >> (pin & 31U)) & 1U) != 0;
}

static void trace_sample(void)
{
    if (!s_trace.active || s_trace.count >= TRACE_SAMPLES) return;
    bool clock = gpio_ll_get_level(&GPIO, CONFIG_STM32_SWCLK_GPIO) != 0;
    bool data = gpio_ll_get_level(&GPIO, CONFIG_STM32_SWDIO_GPIO) != 0;
    if (clock != s_trace.high) s_trace.clock_mismatches++;
    s_trace.samples[s_trace.count++] = (clock ? 2U : 0U) |
                                      (data ? 1U : 0U) |
                                      (data_output_enabled() ? 4U : 0U);
}

void swd_gpio_trace_begin(void)
{
    s_trace.count = s_trace.clock_mismatches = 0;
    s_trace.active = true;
}

void swd_gpio_trace_end(void)
{
    s_trace.active = false;
    if (!s_trace.count) return;
    char low[TRACE_SAMPLES / 2 + 1], high[sizeof(low)], owner[sizeof(low)];
    unsigned n = 0;
    for (unsigned i = 0; i + 1 < s_trace.count; i += 2) {
        low[n] = (s_trace.samples[i] & 1U) ? '1' : '0';
        high[n] = (s_trace.samples[i + 1] & 1U) ? '1' : '0';
        owner[n] = (s_trace.samples[i] & 4U) ? 'H' : 'T';
        n++;
    }
    low[n] = high[n] = owner[n] = 0;
    ESP_LOGI(TAG, "PAD trace: %u clocks, CLK mismatches=%u; bits are chronological",
             n, s_trace.clock_mismatches);
    ESP_LOGI(TAG, "DIO low : %s", low);
    ESP_LOGI(TAG, "DIO high: %s", high);
    ESP_LOGI(TAG, "OE H/T  : %s (H=host output, T=host released)", owner);
}

static void clock_out(void *ctx, bool high)
{
    (void)ctx;
    s_trace.high = high;
    gpio_ll_set_level(&GPIO, CONFIG_STM32_SWCLK_GPIO, high);
}
static void data_out(void *ctx, bool high)
{ (void)ctx; gpio_ll_set_level(&GPIO, CONFIG_STM32_SWDIO_GPIO, high); }
static bool data_in(void *ctx)
{ (void)ctx; return gpio_ll_get_level(&GPIO, CONFIG_STM32_SWDIO_GPIO) != 0; }
static void direction(void *ctx, bool output)
{
    (void)ctx;
    /* IOMUX and the input path stay configured throughout the transaction.
     * Only OE changes at turnaround, avoiding driver/IOMUX reconfiguration
     * between the park bit and the target acknowledgement. */
    if (output) gpio_ll_output_enable(&GPIO, CONFIG_STM32_SWDIO_GPIO);
    else gpio_ll_output_disable(&GPIO, CONFIG_STM32_SWDIO_GPIO);
}
static void delay_half(void *ctx)
{
    (void)ctx;
    esp_rom_delay_us(CONFIG_STM32_SWD_DELAY_US);
    trace_sample();
}
static void delay_half_slow(void *ctx)
{
    (void)ctx;
    const unsigned delay_us = CONFIG_STM32_SWD_DELAY_US;
    esp_rom_delay_us(delay_us < 8U ? 8U : delay_us);
    trace_sample();
}
void swd_gpio_slow_down(swd_wire_t *wire)
{
    wire->io.delay = delay_half_slow;
}

static unsigned read_pads(void)
{
    return (gpio_get_level(CONFIG_STM32_SWDIO_GPIO) ? 1U : 0U) |
           (gpio_get_level(CONFIG_STM32_SWCLK_GPIO) ? 2U : 0U);
}

esp_err_t swd_gpio_check_pads(void)
{
    /* GPIO inputs read the pad levels during output. Walking the two outputs
     * independently catches a stuck pad or coupled signals. The subsequent
     * SWD line reset starts the target in a known protocol state. */
    static const unsigned patterns[] = {0, 1, 3, 2, 1};
    unsigned observed[sizeof(patterns) / sizeof(patterns[0])];
    unsigned mismatches = 0;
    for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        data_out(NULL, (patterns[i] & 1U) != 0);
        clock_out(NULL, (patterns[i] & 2U) != 0);
        esp_rom_delay_us(12);
        observed[i] = read_pads();
        if (observed[i] != patterns[i]) mismatches++;
    }

    /* Compare the released line under both weak biases. A line that follows
     * the biases during idle and reads 111 during ACK may be disconnected, or
     * the target may simply have missed the request. */
    direction(NULL, false);
    gpio_set_pull_mode(CONFIG_STM32_SWDIO_GPIO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(200);
    const unsigned weak_down = data_in(NULL);
    gpio_set_pull_mode(CONFIG_STM32_SWDIO_GPIO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(200);
    const unsigned weak_up = data_in(NULL);
    direction(NULL, true);

    ESP_LOGI(TAG, "GPIO%d DIO / GPIO%d CLK: driven pad samples 0,1,3,2,1 -> %u,%u,%u,%u,%u; released DIO weak-down/up=%u/%u",
             CONFIG_STM32_SWDIO_GPIO, CONFIG_STM32_SWCLK_GPIO,
             observed[0], observed[1], observed[2], observed[3], observed[4],
             weak_down, weak_up);
    if (mismatches) {
        const uint64_t mask = (1ULL << CONFIG_STM32_SWDIO_GPIO) |
                              (1ULL << CONFIG_STM32_SWCLK_GPIO);
        gpio_dump_io_configuration(stdout, mask);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

void swd_gpio_reset(bool asserted)
{ gpio_set_level(CONFIG_STM32_NRST_GPIO, asserted ? 0 : 1); }
esp_err_t swd_gpio_open(swd_wire_t *wire)
{
    if (!wire) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = gpio_reset_pin(CONFIG_STM32_SWDIO_GPIO);
    if (ret != ESP_OK) return ret;
    ret = gpio_reset_pin(CONFIG_STM32_SWCLK_GPIO);
    if (ret != ESP_OK) return ret;
    gpio_set_level(CONFIG_STM32_SWDIO_GPIO, 1);
    gpio_set_level(CONFIG_STM32_SWCLK_GPIO, 0);
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_STM32_SWDIO_GPIO) |
                        (1ULL << CONFIG_STM32_SWCLK_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;
    gpio_ll_set_output_enable_ctrl(&GPIO, CONFIG_STM32_SWDIO_GPIO, false, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, CONFIG_STM32_SWCLK_GPIO, false, false);
    *wire = (swd_wire_t){.io = {
        .clock = clock_out, .data = data_out, .read = data_in,
        .output = direction, .delay = delay_half,
    }, .wait_retries = 100};
    return ESP_OK;
}
void swd_gpio_close(void)
{
    gpio_reset_pin(CONFIG_STM32_SWDIO_GPIO);
    gpio_reset_pin(CONFIG_STM32_SWCLK_GPIO);
}
