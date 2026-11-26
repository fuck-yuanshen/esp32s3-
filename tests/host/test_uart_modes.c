/* Compile the real UART/ISP implementations against deterministic host mocks.
 * Each case checks a state transition, actual RX consumption, or cleanup path.
 * Scripted bootloader replies exercise orchestration, not AN3155 conformance.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_stub.h"

static bool fail_strdup;
static char *host_strdup(const char *text)
{
    if (fail_strdup) return NULL;
    const size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static size_t host_strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size) {
        const size_t copied = length < size - 1 ? length : size - 1;
        memcpy(dst, src, copied);
        dst[copied] = 0;
    }
    return length;
}

#define TAG uart_test_tag
#include "../../components/uart_link/uart_link.c"
#undef TAG
#define TAG image_test_tag
#include "../../components/stm32_isp/stm32_image.c"
#undef TAG
#define TAG isp_test_tag
#define strlcpy host_strlcpy
#define strdup host_strdup
#include "../../components/stm32_isp/stm32_isp.c"
#undef strdup
#undef strlcpy
#undef TAG

struct host_semaphore { bool mutex; int tokens; };
static struct host_semaphore semaphores[8];
static unsigned semaphore_count;
static TaskFunction_t rx_task, flash_worker;
static void *flash_argument;
static bool step_rx, fail_task, restored_in_reset;
static int config_failures, installs, deletes, driver_reads, flushes;
static int actual_baud, pin_levels[49];
static uint8_t normal_rx[1024], normal_tx[1024], isp_replies[1024];
static size_t normal_rx_size, normal_rx_offset, normal_tx_size;
static size_t reply_size, reply_offset;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(semaphore_count < 8);
    SemaphoreHandle_t sem = &semaphores[semaphore_count++];
    *sem = (struct host_semaphore){true, 1};
    return sem;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t sem = xSemaphoreCreateMutex();
    *sem = (struct host_semaphore){false, 0};
    return sem;
}
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    assert(sem);
    if (!sem->tokens && !sem->mutex && timeout == portMAX_DELAY) rx_task(NULL);
    if (!sem->tokens) { assert(!sem->mutex); return 0; }
    --sem->tokens;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem && !sem->tokens);
    ++sem->tokens;
    return 1;
}
int xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)stack; (void)priority; (void)handle;
    if (fail_task) { fail_task = false; return 0; }
    if (strcmp(name, "uart2_rx") == 0) rx_task = fn;
    else { flash_worker = fn; flash_argument = arg; }
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (step_rx) s_stopping = true;
}
void vTaskDelete(void *task) { (void)task; }

static bool host_rx_irq = true;
static unsigned host_gpio_resets, host_pin_maps;
static bool host_fail_pin_map;
esp_err_t gpio_reset_pin(int pin)
{ assert(pin == 19 || pin == 20); ++host_gpio_resets; return ESP_OK; }
esp_err_t uart_disable_rx_intr(uart_port_t port)
{ assert(port == 2); host_rx_irq = false; return ESP_OK; }
esp_err_t uart_enable_rx_intr(uart_port_t port)
{ assert(port == 2); host_rx_irq = true; return ESP_OK; }
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int queue,
                              void *handle, int flags)
{
    (void)tx; (void)queue; (void)handle; (void)flags;
    assert(port == 2 && rx >= 4096);
    ++installs;
    return ESP_OK;
}
esp_err_t uart_driver_delete(uart_port_t port)
{ assert(port == 2); ++deletes; return ESP_OK; }
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    assert(port == 2 && s_lock->tokens == 0);
    if (config_failures) { --config_failures; return ESP_FAIL; }
    actual_baud = config->baud_rate;
    if (s_state.mode == UART_LINK_MODE_ISP && actual_baud == 115200 &&
        pin_levels[CONFIG_STM32_NRST_GPIO] == 0) restored_in_reset = true;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)rts; (void)cts;
    assert(port == 2 && tx == 19 && rx == 20);
    ++host_pin_maps;
    return host_fail_pin_map ? ESP_FAIL : ESP_OK;
}
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t timeout)
{ (void)timeout; assert(port == 2); return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t port)
{
    assert(port == 2);
    ++flushes;
    normal_rx_offset = normal_rx_size;
    return ESP_OK;
}
int uart_write_bytes(uart_port_t port, const void *data, size_t size)
{
    assert(port == 2);
    if (s_state.mode == UART_LINK_MODE_DATA) {
        assert(s_lock->tokens == 0 && normal_tx_size + size <= sizeof(normal_tx));
        memcpy(normal_tx + normal_tx_size, data, size);
        normal_tx_size += size;
    } else assert(s_state.mode == UART_LINK_MODE_ISP);
    return (int)size;
}
int uart_read_bytes(uart_port_t port, void *data, uint32_t size, TickType_t timeout)
{
    assert(port == 2);
    ++driver_reads;
    uint8_t *source;
    size_t *offset, length;
    if (s_state.mode == UART_LINK_MODE_DATA) {
        assert(timeout == 0 && s_lock->tokens == 0);
        source = normal_rx; offset = &normal_rx_offset; length = normal_rx_size;
    } else {
        assert(s_state.mode == UART_LINK_MODE_ISP);
        if (timeout == 0) return 0; /* ISP flush precedes each command. */
        source = isp_replies; offset = &reply_offset; length = reply_size;
    }
    const size_t remaining = length - *offset;
    const size_t count = size < remaining ? size : remaining;
    memcpy(data, source + *offset, count);
    *offset += count;
    return (int)count;
}
esp_err_t gpio_config(const gpio_config_t *config)
{ assert(config->mode == GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t gpio_set_level(int pin, int level)
{ assert(pin == 15 || pin == 16); pin_levels[pin] = level; return ESP_OK; }

/* One deterministic receive iteration, then a stop at the next loop boundary. */
static void poll_rx(void)
{
    step_rx = true;
    rx_task(NULL);
    step_rx = false;
    s_stopping = false;
    assert(xSemaphoreTake(s_rx_stopped, 0));
}
static void expect_data(void)
{
    uart_link_snapshot_t snapshot;
    uart_link_snapshot(&snapshot);
    assert(snapshot.mode == UART_LINK_MODE_DATA && snapshot.baud_rate == 115200);
    assert(actual_baud == 115200 && !stm32_isp_busy());
}
static void reply(uint8_t value)
{ assert(reply_size < sizeof(isp_replies)); isp_replies[reply_size++] = value; }
static void handshake_replies(void)
{
    reply_size = reply_offset = 0;
    const uint8_t bytes[] = {ISP_ACK, ISP_ACK, 2, 0x04, 0x35, ISP_ACK,
                             ISP_ACK, 0x31, ISP_ACK};
    for (size_t i = 0; i < sizeof(bytes); ++i) reply(bytes[i]);
}
static void acknowledgements(unsigned count)
{ while (count--) reply(ISP_ACK); }
static void run_flash(const char *path, bool success, bool entered)
{
    restored_in_reset = false;
    const uint32_t rx_before = s_state.rx_bytes, tx_before = s_state.tx_bytes;
    assert(stm32_isp_start(path, 0, 0, true) == ESP_OK);
    assert(stm32_isp_busy() && s_state.mode == UART_LINK_MODE_ISP);
    assert(stm32_isp_start(path, 0, 0, true) == ESP_ERR_INVALID_STATE);
    flash_worker(flash_argument);
    expect_data();
    assert(s_job.state == (success ? STM32_ISP_DONE : STM32_ISP_FAILED));
    assert(s_state.rx_bytes == rx_before && s_state.tx_bytes == tx_before);
    assert(restored_in_reset == entered);
    assert(pin_levels[15] == 0 && pin_levels[16] == 1);
}

/* Exercise production image detection and HEX validation using temporary files. */
static void check_image(const char *text, esp_err_t expected, uint32_t address)
{
    const char *path = "host_image.hex";
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    fclose(file);
    image_t image;
    assert(image_load(path, &image, 0x08000000u) == expected);
    if (expected == ESP_OK) {
        assert(image.count == 1 && image.segments[0].address == address);
        assert(image.segments[0].length == 4);
        const uint8_t data[] = {1, 2, 3, 4};
        assert(memcmp(image.segments[0].data, data, sizeof(data)) == 0);
    }
    image_free(&image);
    assert(remove(path) == 0);
}

static void test_hex_images(void)
{
    check_image(":020000040800F2\r\n:0400000001020304F2\r\n:00000001FF\r\n",
                ESP_OK, 0x08000000u);
    check_image("\xEF\xBB\xBF:020000021234B6\n:0400000001020304F2\n:00000001FF\n",
                ESP_OK, 0x00012340u);
    check_image(":0400000300000000F9\n:0400000508000000EF\n"
                ":0400000001020304F2\n:00000001FF\n", ESP_OK, 0);
    check_image(":0400000001020304F3\n:00000001FF\n", ESP_ERR_INVALID_CRC, 0);
    check_image(":0400000001020304G2\n:00000001FF\n", ESP_ERR_INVALID_RESPONSE, 0);
    check_image(":0400000001020304F2\n", ESP_ERR_INVALID_RESPONSE, 0);
    check_image(":0400000001020304F200\n:00000001FF\n", ESP_ERR_INVALID_SIZE, 0);
    check_image(":0400000001020304F2\n:00000001FF\n:00000001FF\n",
                ESP_ERR_INVALID_RESPONSE, 0);
    check_image(":00000006FA\n:00000001FF\n", ESP_ERR_NOT_SUPPORTED, 0);
    puts("PASS 9 Intel HEX parsing, address and corruption cases");
}

int main(void)
{
    test_hex_images();
    assert(uart_link_init() == ESP_OK && stm32_isp_init() == ESP_OK);
    assert(installs == 1 && deletes == 0); expect_data();
    puts("PASS default DATA mode and single driver owner");

    memcpy(normal_rx, "response", 8); normal_rx_size = 8; normal_rx_offset = 0;
    const int old_flushes = flushes;
    assert(uart_link_send((const uint8_t *)"Q", 1) == ESP_OK);
    assert(normal_tx_size == (HOST_APPEND_CRLF ? 3u : 1u));
    assert(normal_tx[0] == 'Q' && flushes == old_flushes);
    if (HOST_APPEND_CRLF) assert(memcmp(normal_tx + 1, "\r\n", 2) == 0);
    poll_rx(); assert(s_state.rx_bytes == 8 && s_state.rx_length == 8);
    assert(memcmp(s_state.rx_data, "response", 8) == 0);
    puts("PASS normal full-duplex TX/RX preserves pending input");

    for (unsigned i = 0; i < 512; ++i) normal_rx[i] = (uint8_t)(i / 256 + 1);
    normal_rx_size = 512; normal_rx_offset = 0;
    poll_rx(); poll_rx(); assert(s_state.rx_length == 256 && s_state.rx_bytes == 520);
    for (unsigned i = 0; i < 256; ++i) assert(s_state.rx_data[i] == 2);
    puts("PASS binary RX history rolls over at 256 bytes");

    assert(uart_link_begin_isp(57600) == ESP_OK);
    const int reads_before = driver_reads;
    poll_rx(); assert(driver_reads == reads_before);
    assert(uart_link_send_raw((const uint8_t *)"X", 1) == ESP_ERR_INVALID_STATE);
    assert(uart_link_send((const uint8_t *)"X", 1) == ESP_ERR_INVALID_STATE);
    assert(uart_link_begin_isp(57600) == ESP_ERR_INVALID_STATE);
    uart_link_deinit(); assert(deletes == 0);
    assert(uart_link_end_isp() == ESP_OK); expect_data();
    puts("PASS ISP excludes normal TX/RX and driver deletion");

    unsigned maps_before = host_pin_maps;
    assert(uart_link_begin_gpio() == ESP_OK);
    assert(!host_rx_irq && host_gpio_resets == 2);
    assert(s_state.mode == UART_LINK_MODE_ISP);
    assert(uart_link_send_raw((const uint8_t *)"X", 1) == ESP_ERR_INVALID_STATE);
    int swd_reads = driver_reads; poll_rx(); assert(driver_reads == swd_reads);
    assert(uart_link_begin_gpio() == ESP_ERR_INVALID_STATE);
    host_fail_pin_map = true;
    assert(uart_link_end_isp() != ESP_OK);
    assert(!host_rx_irq && s_state.mode == UART_LINK_MODE_ISP);
    host_fail_pin_map = false;
    assert(uart_link_end_isp() == ESP_OK);
    assert(host_rx_irq && host_pin_maps == maps_before + 2);
    expect_data();
    puts("PASS GPIO handoff, paused RX, failed remap isolation and recovery");

    config_failures = 1;
    assert(uart_link_begin_isp(57600) != ESP_OK); expect_data();
    config_failures = 2;
    assert(uart_link_begin_isp(57600) != ESP_OK);
    assert(s_state.mode == UART_LINK_MODE_ISP);
    assert(uart_link_end_isp() == ESP_OK); expect_data();
    puts("PASS failed reconfiguration rolls back or isolates the port");

    fail_strdup = true;
    assert(stm32_isp_start("firmware.bin", 0, 0, true) == ESP_ERR_NO_MEM);
    fail_strdup = false; expect_data();
    fail_task = true;
    assert(stm32_isp_start("firmware.bin", 0, 0, true) == ESP_ERR_NO_MEM);
    expect_data();
    puts("PASS allocation and task-start failures restore DATA mode");

    run_flash("missing-firmware.bin", false, false);
    puts("PASS missing image restores DATA without resetting target");
    const uint8_t firmware[] = {0x11, 0x22, 0x33, 0x44};
    FILE *file = fopen("fixture.bin", "wb"); assert(file);
    assert(fwrite(firmware, 1, sizeof(firmware), file) == sizeof(firmware)); fclose(file);
    reply_size = reply_offset = 0; run_flash("fixture.bin", false, true);
    puts("PASS handshake timeout restores DATA while target is reset");
    handshake_replies(); run_flash("fixture.bin", false, true);
    puts("PASS erase failure restores DATA mode");
    handshake_replies(); acknowledgements(3); run_flash("fixture.bin", false, true);
    puts("PASS write failure restores DATA mode");
    handshake_replies(); acknowledgements(9);
    for (unsigned i = 0; i < 256; ++i) reply(0);
    run_flash("fixture.bin", false, true);
    puts("PASS verify mismatch restores DATA mode");
    handshake_replies(); acknowledgements(9);
    for (unsigned i = 0; i < 256; ++i) reply(i < sizeof(firmware) ? firmware[i] : 0xFF);
    run_flash("fixture.bin", true, true);
    assert(reply_offset == reply_size && s_job.percent == 100);
    puts("PASS successful flash restores DATA before publishing DONE");
    memcpy(normal_rx, "OK", 2); normal_rx_size = 2; normal_rx_offset = 0;
    poll_rx(); assert(s_state.rx_bytes == 522);
    assert(uart_link_send_raw((const uint8_t *)"Z", 1) == ESP_OK);
    puts("PASS ordinary TX/RX resumes after flash completion");
    uart_link_deinit(); assert(deletes == 1 && s_state.mode == UART_LINK_MODE_STOPPED);
    assert(uart_link_send_raw((const uint8_t *)"Z", 1) == ESP_ERR_INVALID_STATE);
    assert(uart_link_init() == ESP_OK && installs == 2);
    puts("PASS RX shutdown and reinitialization");
    remove("fixture.bin");
    printf("UART/ISP host checks passed (APPEND_CRLF=%d)\n", HOST_APPEND_CRLF);
    return 0;
}
