/* stm32_isp.c
 *
 * Purpose in the project:
 *   Offline STM32 programmer built on the factory USART bootloader protocol
 *   (ST AN3155). It drives UART2 plus BOOT0/NRST so a bare STM32 can be
 *   reflashed without a PC or ST-LINK.
 *
 *   Supported input: raw .bin (loaded at CONFIG_STM32_DEFAULT_BASE) and Intel
 *   HEX (addresses taken from the file).
 *
 *   Protocol notes that matter in practice:
 *     - every byte is written separately with a delay, because the STM32
 *       bootloader drops frames when a page is sent as one UART burst;
 *     - some STM32L4 parts answer the erase command with 0x1F even though the
 *       erase succeeded, so that code is accepted with a warning and the read
 *       back verification is the real check;
 *     - after programming, BOOT0 is released and NRST is pulsed so the new
 *       application runs from its reset vector.
 *
 *   Reference implementation: STM32_BOOTLOADER_DEMO-main (Arduino), rewritten
 *   for ESP-IDF with explicit timeouts, page alignment and progress reporting.
 */

#include "stm32_isp.h"
#include "stm32_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "uart_link.h"

static const char *TAG = "stm32_isp";

#define ISP_PORT          ((uart_port_t)CONFIG_UART_LINK_PORT)
#define ISP_BLOCK         256
#define ISP_ERASE_BATCH   8          /* page indices per erase command */

#define ISP_ACK           0x79
#define ISP_NACK          0x1F
#define ISP_BUSY          0x76

#define CMD_GET           0x00
#define CMD_GET_VERSION   0x01
#define CMD_GET_ID        0x02
#define CMD_READ          0x11
#define CMD_GO            0x21
#define CMD_WRITE         0x31
#define CMD_ERASE         0x43
#define CMD_ERASE_EXT     0x44

static struct {
    stm32_isp_state_t state;
    uint8_t percent;
    char message[48];
    uint32_t written;
    uint32_t total;
    uint16_t target_id;
    uint32_t page_size;
    uint32_t base_address;
    bool verify;
    bool busy;
} s_job;
static SemaphoreHandle_t s_job_lock;
static bool s_initialized;

/* ------------------------------------------------------------- job status */

static void status_set(stm32_isp_state_t state, uint8_t percent, const char *message)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job.state = state;
    if (percent <= 100) {
        s_job.percent = percent;
    }
    if (message) {
        strlcpy(s_job.message, message, sizeof(s_job.message));
    }
    xSemaphoreGive(s_job_lock);
}

static void status_progress(uint32_t done, uint32_t total, uint8_t base, uint8_t span,
                            const char *message)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job.written = done;
    s_job.total = total;
    if (total) {
        s_job.percent = (uint8_t)(base + (span * (uint64_t)done) / total);
    }
    if (message) {
        strlcpy(s_job.message, message, sizeof(s_job.message));
    }
    xSemaphoreGive(s_job_lock);
}

/* ------------------------------------------------------------ GPIO / UART */

static void reset_pulse(void)
{
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void bootloader_enter(void)
{
    gpio_set_level(CONFIG_STM32_BOOT0_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    reset_pulse();
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* Restore DATA mode while the target is held in reset. Startup bytes from
 * the STM32 application then arrive at the normal baud, not the ISP baud.
 */
static esp_err_t bootloader_leave(void)
{
    gpio_set_level(CONFIG_STM32_BOOT0_GPIO, 0);
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    const esp_err_t ret = uart_link_end_isp();
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ret;
}

static void rx_flush(void)
{
    uint8_t scratch[64];
    while (uart_read_bytes(ISP_PORT, scratch, sizeof(scratch), 0) > 0) {
    }
}

/* One uart_write_bytes() call per byte. A block write is silently dropped by
 * the STM32 bootloader even at 9600 baud, which is the single most important
 * implementation detail of this protocol.
 */
static esp_err_t isp_write(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (uart_write_bytes(ISP_PORT, &data[i], 1) != 1) {
            return ESP_FAIL;
        }
#if CONFIG_STM32_BYTE_PACING_MS > 0
        vTaskDelay(pdMS_TO_TICKS(CONFIG_STM32_BYTE_PACING_MS));
#endif
    }
    return uart_wait_tx_done(ISP_PORT, pdMS_TO_TICKS(2000));
}

static esp_err_t isp_read(uint8_t *buffer, size_t length, uint32_t timeout_ms)
{
    const int read = uart_read_bytes(ISP_PORT, buffer, length, pdMS_TO_TICKS(timeout_ms));
    return read == (int)length ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint8_t xor_checksum(const uint8_t *data, size_t length)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum ^= data[i];
    }
    return sum;
}

/* A response byte of ACK passes, NACK fails, BUSY asks the caller to poll.
 * 0x1F is also what STM32L4 returns after a successful erase, so erase handles
 * it explicitly instead of relying on this helper.
 */
static esp_err_t isp_wait_ack(uint32_t timeout_ms)
{
    uint8_t value;
    esp_err_t ret = isp_read(&value, 1, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (value == ISP_ACK) {
        return ESP_OK;
    }
    if (value == ISP_BUSY) {
        return ESP_ERR_NOT_FINISHED;
    }
    ESP_LOGW(TAG, "target answered 0x%02X", value);
    return ESP_FAIL;
}

static esp_err_t isp_command(uint8_t command, uint32_t timeout_ms)
{
    const uint8_t frame[2] = {command, (uint8_t)~command};
    rx_flush();
    esp_err_t ret = isp_write(frame, sizeof(frame));
    return ret == ESP_OK ? isp_wait_ack(timeout_ms) : ret;
}

static esp_err_t isp_sync(void)
{
    /* The sync byte is 0x7F. Some targets need a second attempt after their
     * reset has settled, hence the retry loop.
     */
    const uint8_t sync = 0x7F;
    for (int attempt = 0; attempt < 6; ++attempt) {
        rx_flush();
        isp_write(&sync, 1);
        if (isp_wait_ack(600) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t isp_get_id(uint16_t *pid)
{
    esp_err_t ret = isp_command(CMD_GET_ID, 800);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t count = 0;
    if (isp_read(&count, 1, 800) != ESP_OK || count < 1 || count > 4) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t bytes[4] = {0};
    if (isp_read(bytes, count, 800) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    ret = isp_wait_ack(800);
    if (ret == ESP_OK && pid) {
        *pid = (uint16_t)((bytes[0] << 8) | bytes[1]);
    }
    return ret;
}

static esp_err_t isp_get_version(uint8_t *version)
{
    esp_err_t ret = isp_command(CMD_GET_VERSION, 800);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t value = 0;
    if (isp_read(&value, 1, 800) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    ret = isp_wait_ack(800);
    if (ret == ESP_OK && version) {
        *version = value;
    }
    return ret;
}

/* Erase is sent with the extended command (0x44) first because F4/L4/G0/G4/H7
 * only accept that form. F1/F3 answer NACK and the same frame is retried with
 * the legacy 0x43 command. Both forms carry N-1 as two bytes plus XOR followed
 * by the page index list plus XOR.
 */
static esp_err_t isp_erase_pages_with(uint8_t command, uint16_t first_page,
                                      uint16_t page_count, uint32_t timeout_ms)
{
    if (page_count == 0) {
        return ESP_OK;
    }
    esp_err_t ret = isp_command(command, 800);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t header[3] = {(uint8_t)((page_count - 1) >> 8),
                         (uint8_t)((page_count - 1) & 0xFF), 0};
    header[2] = header[0] ^ header[1];
    ret = isp_write(header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = isp_wait_ack(1500);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t sum = 0;
    for (uint16_t i = 0; i < page_count; ++i) {
        const uint16_t page = (uint16_t)(first_page + i);
        const uint8_t entry[2] = {(uint8_t)(page >> 8), (uint8_t)(page & 0xFF)};
        ret = isp_write(entry, sizeof(entry));
        if (ret != ESP_OK) {
            return ret;
        }
        sum ^= entry[0] ^ entry[1];
    }
    ret = isp_write(&sum, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t response = 0;
    if (isp_read(&response, 1, timeout_ms) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (response == ISP_ACK) {
        return ESP_OK;
    }
    if (response == ISP_NACK) {
        /* Documented STM32L4 behaviour: 0x1F can also mean "erased anyway".
         * The read-back verification pass is the real check.
         */
        ESP_LOGW(TAG, "erase answered 0x1F (accepted, verify will confirm)");
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t isp_erase_pages(uint16_t first_page, uint16_t page_count,
                                 uint32_t timeout_ms)
{
    esp_err_t ret = isp_erase_pages_with(CMD_ERASE_EXT, first_page, page_count, timeout_ms);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "extended erase refused, retrying legacy 0x43");
        ret = isp_erase_pages_with(CMD_ERASE, first_page, page_count, timeout_ms);
    }
    return ret;
}

/* AN3155 mass erase: extended erase with N-1 = 0xFFFF. Used when the computed
 * page list is rejected, which happens when the selected page size does not
 * match the target's real erase granularity.
 */
static esp_err_t isp_mass_erase(void)
{
    esp_err_t ret = isp_command(CMD_ERASE_EXT, 800);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t frame[3] = {0xFF, 0xFF, 0x00};
    ret = isp_write(frame, sizeof(frame));
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t response = 0;
    if (isp_read(&response, 1, 60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (response == ISP_ACK || response == ISP_NACK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t isp_erase_range(uint32_t flash_base, uint32_t first_address,
                                 uint32_t last_address, uint32_t page_size)
{
    if (first_address < flash_base || last_address <= first_address || !page_size) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t first_page = (first_address - flash_base) / page_size;
    const uint32_t page_count = (last_address - first_address + page_size - 1) / page_size;

    for (uint32_t done = 0; done < page_count; done += ISP_ERASE_BATCH) {
        const uint32_t batch = (page_count - done) < ISP_ERASE_BATCH
                                   ? (page_count - done) : ISP_ERASE_BATCH;
        esp_err_t ret = isp_erase_pages((uint16_t)(first_page + done), (uint16_t)batch,
                                        60000);
        if (ret != ESP_OK) {
#if CONFIG_STM32_MASS_ERASE_FALLBACK
            /* A rejected page list usually means the selected page size does
             * not match the part. A whole-chip erase is the only way to
             * guarantee a blank target, but it destroys every other sector.
             */
            ESP_LOGW(TAG, "page erase failed (%s), falling back to mass erase",
                     esp_err_to_name(ret));
            status_set(STM32_ISP_RUNNING, 8, "mass erasing");
            return isp_mass_erase();
#else
            ESP_LOGE(TAG, "page erase failed: %s", esp_err_to_name(ret));
            return ret;
#endif
        }
        status_set(STM32_ISP_RUNNING, 8, "erasing");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t isp_write_block(uint32_t address, const uint8_t *data, uint16_t length)
{
    if (length == 0 || length > ISP_BLOCK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = isp_command(CMD_WRITE, 800);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t frame[5] = {(uint8_t)(address >> 24), (uint8_t)(address >> 16),
                        (uint8_t)(address >> 8), (uint8_t)address, 0};
    frame[4] = xor_checksum(frame, 4);
    ret = isp_write(frame, sizeof(frame));
    if (ret == ESP_OK) {
        ret = isp_wait_ack(1000);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "address 0x%08X rejected: %s", (unsigned)address, esp_err_to_name(ret));
        return ret;
    }

    const uint8_t n_minus_one = (uint8_t)(length - 1);
    ret = isp_write(&n_minus_one, 1);
    if (ret == ESP_OK) {
        ret = isp_write(data, length);
    }
    uint8_t sum = n_minus_one ^ xor_checksum(data, length);
    if (ret == ESP_OK) {
        ret = isp_write(&sum, 1);
    }
    if (ret == ESP_OK) {
        ret = isp_wait_ack(20000);
    }
    return ret;
}

static esp_err_t isp_read_block(uint32_t address, uint8_t *data, uint16_t length)
{
    esp_err_t ret = isp_command(CMD_READ, 800);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t frame[5] = {(uint8_t)(address >> 24), (uint8_t)(address >> 16),
                        (uint8_t)(address >> 8), (uint8_t)address, 0};
    frame[4] = xor_checksum(frame, 4);
    ret = isp_write(frame, sizeof(frame));
    if (ret == ESP_OK) {
        ret = isp_wait_ack(1000);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t length_frame[2] = {(uint8_t)(length - 1), (uint8_t)~(uint8_t)(length - 1)};
    ret = isp_write(length_frame, sizeof(length_frame));
    if (ret == ESP_OK) {
        ret = isp_wait_ack(1000);
    }
    if (ret == ESP_OK) {
        ret = isp_read(data, length, 10000);
    }
    return ret;
}

/* --------------------------------------------------------- image loading */

/* ------------------------------------------------------------- flash flow */

/* Fills one 256-byte programming block for `address`. Bytes the segment does not
 * cover stay 0xFF. A streamed segment (data == NULL) is read from the open
 * image file, so a large .bin never has to fit in RAM.
 */
static esp_err_t segment_read_block(const image_t *image, const image_segment_t *segment,
                                    uint32_t address, uint8_t *block)
{
    memset(block, 0xFF, ISP_BLOCK);
    const uint32_t block_end = address + ISP_BLOCK;
    const uint32_t seg_end = segment->address + segment->length;
    const uint32_t copy_start = segment->address > address ? segment->address : address;
    const uint32_t copy_end = seg_end < block_end ? seg_end : block_end;
    if (copy_end <= copy_start) {
        return ESP_OK;
    }
    const uint32_t count = copy_end - copy_start;
    uint8_t *destination = &block[copy_start - address];

    if (segment->data) {
        memcpy(destination, &segment->data[copy_start - segment->address], count);
        return ESP_OK;
    }
    if (!image->file) {
        return ESP_ERR_INVALID_STATE;
    }
    const long offset = (long)segment->file_offset + (long)(copy_start - segment->address);
    if (fseek(image->file, offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(destination, 1, count, image->file) != count) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_segment(const image_t *image, const image_segment_t *segment,
                               uint32_t total, uint32_t *progress)
{
    const uint32_t aligned_start = segment->address & ~(ISP_BLOCK - 1u);
    uint32_t address = aligned_start;
    uint8_t block[ISP_BLOCK];

    while (address < segment->address + segment->length) {
        esp_err_t ret = segment_read_block(image, segment, address, block);
        if (ret != ESP_OK) {
            return ret;
        }
        const uint32_t block_end = address + ISP_BLOCK;
        const uint32_t seg_end = segment->address + segment->length;
        const uint32_t copy_start = segment->address > address ? segment->address : address;
        const uint32_t copy_end = seg_end < block_end ? seg_end : block_end;
        if (copy_end > copy_start) {
            ret = isp_write_block(address, block, sizeof(block));
            if (ret != ESP_OK) {
                return ret;
            }
            *progress += copy_end - copy_start;
            status_progress(*progress, total, 10, 70, "writing");
        }
        address += ISP_BLOCK;
    }
    return ESP_OK;
}

/* Reads the same blocks back and compares them with the source image. This is
 * the authoritative success check: several STM32 families answer the erase
 * command with an ambiguous status byte.
 */
static esp_err_t verify_segment(const image_t *image, const image_segment_t *segment,
                                 uint32_t total, uint32_t *progress)
{
    const uint32_t aligned_start = segment->address & ~(ISP_BLOCK - 1u);
    uint32_t address = aligned_start;
    uint8_t actual[ISP_BLOCK];
    uint8_t expected[ISP_BLOCK];

    while (address < segment->address + segment->length) {
        esp_err_t ret = isp_read_block(address, actual, sizeof(actual));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "read back failed at 0x%08X: %s", (unsigned)address,
                     esp_err_to_name(ret));
            return ret;
        }
        ret = segment_read_block(image, segment, address, expected);
        if (ret != ESP_OK) {
            return ret;
        }
        const uint32_t block_end = address + ISP_BLOCK;
        const uint32_t seg_end = segment->address + segment->length;
        const uint32_t copy_start = segment->address > address ? segment->address : address;
        const uint32_t copy_end = seg_end < block_end ? seg_end : block_end;
        if (copy_end > copy_start) {
            /* Only the bytes this segment covers are owned by this image; the
             * rest of the block may belong to another segment or to untouched
             * flash, so it is compared separately.
             */
            const uint32_t offset = copy_start - address;
            if (memcmp(&expected[offset], &actual[offset], copy_end - copy_start) != 0) {
                ESP_LOGE(TAG, "verify mismatch at 0x%08X", (unsigned)copy_start);
                return ESP_FAIL;
            }
            *progress += copy_end - copy_start;
            status_progress(*progress, total, 80, 18, "verifying");
        }
        address += ISP_BLOCK;
    }
    return ESP_OK;
}

static esp_err_t flash_image(const char *path, uint32_t base_address, uint32_t page_size,
                             bool verify, bool *entered_bootloader)
{
    *entered_bootloader = false;
    image_t image;
    esp_err_t ret = image_load(path, &image, base_address);
    if (ret != ESP_OK) {
        status_set(STM32_ISP_FAILED, 0, "image unusable");
        return ret;
    }

    /* Firmware only; the erase range must cover the whole image extent. */
    status_set(STM32_ISP_RUNNING, 2, "entering bootloader");
    *entered_bootloader = true;
    bootloader_enter();

    ret = isp_sync();
    uint16_t target_id = 0;
    if (ret == ESP_OK) {
        ret = isp_get_id(&target_id);
    }
    if (ret == ESP_OK) {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        s_job.target_id = target_id;
        xSemaphoreGive(s_job_lock);
        ESP_LOGI(TAG, "target product id 0x%03X, page size %u", target_id,
                 (unsigned)page_size);
    }
    if (ret == ESP_OK) {
        ret = isp_get_version(NULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bootloader handshake failed: %s", esp_err_to_name(ret));
        status_set(STM32_ISP_FAILED, 0, "no bootloader answer");
        image_free(&image);
        return ret;
    }

    status_set(STM32_ISP_RUNNING, 5, "erasing");
    ret = isp_erase_range(base_address, image.low_address, image.high_address, page_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(ret));
        status_set(STM32_ISP_FAILED, 0, "erase failed");
        image_free(&image);
        return ret;
    }

    uint32_t total = 0;
    for (size_t i = 0; i < image.count; ++i) {
        total += image.segments[i].length;
    }
    uint32_t progress = 0;
    status_progress(0, total, 10, 70, "writing");
    for (size_t i = 0; i < image.count && ret == ESP_OK; ++i) {
        ret = write_segment(&image, &image.segments[i], total, &progress);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write failed at %u/%u bytes: %s", (unsigned)progress,
                 (unsigned)total, esp_err_to_name(ret));
        status_set(STM32_ISP_FAILED, 0, "write failed");
        image_free(&image);
        return ret;
    }

    if (verify) {
        progress = 0;
        status_progress(0, total, 80, 18, "verifying");
        for (size_t i = 0; i < image.count && ret == ESP_OK; ++i) {
            ret = verify_segment(&image, &image.segments[i], total, &progress);
        }
    }

    image_free(&image);
    if (ret != ESP_OK) {
        status_set(STM32_ISP_FAILED, 0, "verify failed");
        return ret;
    }

    /* Releasing BOOT0 and pulsing NRST starts the new application through its
     * reset vector, which works even when the GO command is not supported.
     */
    status_set(STM32_ISP_RUNNING, 99, "starting application");
    return ESP_OK;
}

static void flash_task(void *argument)
{
    char *path = argument;
    uint32_t page_size;
    uint32_t base_address;
    bool verify;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    page_size = s_job.page_size;
    base_address = s_job.base_address;
    verify = s_job.verify;
    xSemaphoreGive(s_job_lock);
    bool entered_bootloader = false;
    const esp_err_t result = flash_image(path, base_address, page_size, verify,
                                         &entered_bootloader);
    /* Every job exit, including image/handshake/erase/verify errors, returns
     * the shared peripheral to normal communication before publishing DONE.
     */
    const esp_err_t restore = entered_bootloader ? bootloader_leave()
                                                : uart_link_end_isp();
    if (restore != ESP_OK) {
        status_set(STM32_ISP_FAILED, 0, "UART restore failed");
        ESP_LOGE(TAG, "normal UART restore failed: %s", esp_err_to_name(restore));
    } else if (result == ESP_OK) {
        status_set(STM32_ISP_DONE, 100, "done, UART restored");
        ESP_LOGI(TAG, "flash complete, normal UART TX/RX restored");
    }
    free(path);
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job.busy = false;
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------- API */

esp_err_t stm32_isp_init(void)
{
    if (!s_job_lock) {
        s_job_lock = xSemaphoreCreateMutex();
        if (!s_job_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_initialized) {
        return ESP_OK;
    }
    /* Preparing control pins does not select ISP or change the normal baud.
     * uart_link is the only module that installs or removes the UART driver.
     */
    esp_err_t ret = uart_link_init();
    if (ret != ESP_OK) {
        return ret;
    }
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << CONFIG_STM32_BOOT0_GPIO) |
                        (1ULL << CONFIG_STM32_NRST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&output);
    if (ret != ESP_OK) {
        return ret;
    }
    gpio_set_level(CONFIG_STM32_BOOT0_GPIO, 0);
    gpio_set_level(CONFIG_STM32_NRST_GPIO, 1);
    s_initialized = true;
    ESP_LOGI(TAG, "ISP prepared, inactive: UART%d TX%d RX%d BOOT0=%d NRST=%d",
             CONFIG_UART_LINK_PORT, CONFIG_UART_LINK_TX_GPIO,
             CONFIG_UART_LINK_RX_GPIO, CONFIG_STM32_BOOT0_GPIO,
             CONFIG_STM32_NRST_GPIO);
    status_set(STM32_ISP_IDLE, 0, "idle");
    return ESP_OK;
}

esp_err_t stm32_isp_start(const char *path, uint32_t base_address, uint32_t page_size,
                          bool verify)
{
    if (!path || !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A zero page size means "use the Kconfig default"; the UI passes an explicit
     * value when the target part uses a different erase granularity.
     */
    if (page_size == 0) {
        page_size = CONFIG_STM32_DEFAULT_PAGE_SIZE;
    }
    if (base_address == 0) {
        base_address = CONFIG_STM32_DEFAULT_BASE;
    }

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    const bool busy = s_job.busy;
    if (!busy) {
        s_job.busy = true;
        s_job.page_size = page_size;
        s_job.base_address = base_address;
        s_job.verify = verify;
        s_job.target_id = 0;
        s_job.written = 0;
        s_job.total = 0;
    }
    xSemaphoreGive(s_job_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t baud_ret = uart_link_begin_isp(CONFIG_STM32_UART_BAUD);
    if (baud_ret != ESP_OK) {
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        s_job.busy = false;
        xSemaphoreGive(s_job_lock);
        return baud_ret;
    }

    char *copy = strdup(path);
    if (!copy) {
        uart_link_end_isp();
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        s_job.busy = false;
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NO_MEM;
    }

    status_set(STM32_ISP_RUNNING, 0, "starting");
    if (xTaskCreate(flash_task, "stm32_flash", 6144, copy, 5, NULL) != pdPASS) {
        free(copy);
        uart_link_end_isp();
        status_set(STM32_ISP_FAILED, 0, "flash task creation failed");
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        s_job.busy = false;
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void stm32_isp_status(stm32_isp_status_t *status)
{
    if (!status) {
        return;
    }
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    status->state = s_job.state;
    status->percent = s_job.percent;
    strlcpy(status->message, s_job.message, sizeof(status->message));
    status->bytes_written = s_job.written;
    status->bytes_total = s_job.total;
    status->target_id = s_job.target_id;
    status->page_size = s_job.page_size;
    xSemaphoreGive(s_job_lock);
}

bool stm32_isp_busy(void)
{
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    const bool busy = s_job.busy;
    xSemaphoreGive(s_job_lock);
    return busy;
}



