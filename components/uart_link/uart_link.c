/* Owns UART2, background reception and exclusive handoff to STM32 ISP.
 * Driver access and mode changes share one mutex. Normal sends and the RX
 * worker are excluded for the entire ISP lease, not just while changing baud.
 */
#include "uart_link.h"
#include <limits.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define UART_LINK_PORT ((uart_port_t)CONFIG_UART_LINK_PORT)
#define UART_LINK_DRIVER_RX_BYTES 4096
#define UART_LINK_RX_POLL_MS 10

static const char *TAG = "uart_link";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_rx_stopped;
static bool s_ready;
static bool s_stopping;
static bool s_gpio_lease;
static uart_link_snapshot_t s_state;
static uart_link_rx_callback_t s_rx_callback;

/* Only ownership transitions reconfigure and discard input. Ordinary sends
 * leave pending RX bytes intact, preserving an immediate slave response.
 */
static esp_err_t configure_locked(uint32_t baud_rate)
{
    const uart_config_t config = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_wait_tx_done(UART_LINK_PORT, pdMS_TO_TICKS(2000));
    if (ret == ESP_OK) ret = uart_param_config(UART_LINK_PORT, &config);
    if (ret == ESP_OK) ret = uart_flush_input(UART_LINK_PORT);
    if (ret == ESP_OK) s_state.baud_rate = baud_rate;
    return ret;
}

static void save_rx_locked(const uint8_t *data, size_t length)
{
    size_t keep = s_state.rx_length;
    if (keep + length > sizeof(s_state.rx_data)) {
        keep = sizeof(s_state.rx_data) - length;
        memmove(s_state.rx_data, s_state.rx_data + s_state.rx_length - keep, keep);
    }
    memcpy(s_state.rx_data + keep, data, length);
    s_state.rx_length = keep + length;
    s_state.rx_bytes += (uint32_t)length;
}

static void receive_task(void *argument)
{
    (void)argument;
    uint8_t data[UART_LINK_RX_CHUNK_BYTES];
    for (;;) {
        int received = 0;
        uart_link_rx_callback_t callback = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_stopping) {
            xSemaphoreGive(s_lock);
            break;
        }
        if (s_ready && s_state.mode == UART_LINK_MODE_DATA) {
            received = uart_read_bytes(UART_LINK_PORT, data, sizeof(data), 0);
            if (received > 0) {
                save_rx_locked(data, (size_t)received);
                callback = s_rx_callback;
            }
        }
        xSemaphoreGive(s_lock);
        if (received > 0 && callback) {
            callback(data, (size_t)received);
        }
        if (received <= 0) vTaskDelay(pdMS_TO_TICKS(UART_LINK_RX_POLL_MS));
        else taskYIELD();
    }
    xSemaphoreGive(s_rx_stopped);
    vTaskDelete(NULL);
}

void uart_link_set_rx_callback(uart_link_rx_callback_t callback)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rx_callback = callback;
    xSemaphoreGive(s_lock);
}

esp_err_t uart_link_init(void)
{
    /* Boot serializes initialization. Locks survive deinit/reinit so readers
     * never race a freed synchronization object.
     */
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_rx_stopped) s_rx_stopped = xSemaphoreCreateBinary();
    if (!s_lock || !s_rx_stopped) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ready) {
        const esp_err_t ret = s_stopping ? ESP_ERR_INVALID_STATE : ESP_OK;
        xSemaphoreGive(s_lock);
        return ret;
    }
    esp_err_t ret = uart_driver_install(UART_LINK_PORT, UART_LINK_DRIVER_RX_BYTES,
                                        0, 0, NULL, 0);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }
    memset(&s_state, 0, sizeof(s_state));
    ret = configure_locked(CONFIG_UART_LINK_BAUD);
    if (ret == ESP_OK) {
        ret = uart_set_pin(UART_LINK_PORT, CONFIG_UART_LINK_TX_GPIO,
                           CONFIG_UART_LINK_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(UART_LINK_PORT);
        xSemaphoreGive(s_lock);
        return ret;
    }
    s_stopping = false;
    s_ready = true;
    s_state.mode = UART_LINK_MODE_DATA;
    xSemaphoreTake(s_rx_stopped, 0);
    if (xTaskCreate(receive_task, "uart2_rx", 3072, NULL, 5, NULL) != pdPASS) {
        s_ready = false;
        s_state.mode = UART_LINK_MODE_STOPPED;
        uart_driver_delete(UART_LINK_PORT);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "UART%d DATA: TX=%d RX=%d %d 8N1, RX task ready",
             CONFIG_UART_LINK_PORT, CONFIG_UART_LINK_TX_GPIO,
             CONFIG_UART_LINK_RX_GPIO, CONFIG_UART_LINK_BAUD);
    return ESP_OK;
}

void uart_link_deinit(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready || s_stopping || s_state.mode == UART_LINK_MODE_ISP) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_stopping = true;
    s_state.mode = UART_LINK_MODE_STOPPED;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_rx_stopped, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uart_driver_delete(UART_LINK_PORT);
    s_ready = false;
    s_stopping = false;
    s_state.baud_rate = 0;
    xSemaphoreGive(s_lock);
}

esp_err_t uart_link_begin_isp(uint32_t baud_rate)
{
    if (baud_rate == 0 || baud_rate > INT_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_ready && !s_stopping && s_state.mode == UART_LINK_MODE_DATA) {
        ret = configure_locked(baud_rate);
        if (ret == ESP_OK) {
            s_state.mode = UART_LINK_MODE_ISP;
        } else if (configure_locked(CONFIG_UART_LINK_BAUD) != ESP_OK) {
            /* Failed rollback keeps normal traffic excluded from a port whose
             * hardware configuration is no longer known.
             */
            s_state.mode = UART_LINK_MODE_ISP;
        }
    }
    xSemaphoreGive(s_lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ISP owns UART%d: normal TX/RX paused, baud=%u",
                 CONFIG_UART_LINK_PORT, (unsigned)baud_rate);
    }
    return ret;
}

esp_err_t uart_link_end_isp(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_ready && s_state.mode == UART_LINK_MODE_ISP) {
        ret = configure_locked(CONFIG_UART_LINK_BAUD);
        if (ret == ESP_OK) {
            ret = uart_set_pin(UART_LINK_PORT, CONFIG_UART_LINK_TX_GPIO,
                               CONFIG_UART_LINK_RX_GPIO, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE);
        }
        if (ret == ESP_OK && s_gpio_lease) ret = uart_enable_rx_intr(UART_LINK_PORT);
        if (ret == ESP_OK) s_gpio_lease = false;
        if (ret == ESP_OK) s_state.mode = UART_LINK_MODE_DATA;
    }
    xSemaphoreGive(s_lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DATA restored: TX/RX active, baud=%d", CONFIG_UART_LINK_BAUD);
    }
    return ret;
}

esp_err_t uart_link_begin_gpio(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    bool acquired = false;
    if (s_ready && !s_stopping && s_state.mode == UART_LINK_MODE_DATA) {
        ret = uart_wait_tx_done(UART_LINK_PORT, pdMS_TO_TICKS(2000));
        if (ret == ESP_OK) ret = uart_disable_rx_intr(UART_LINK_PORT);
        if (ret == ESP_OK) {
            s_gpio_lease = true;
            acquired = true;
            s_state.mode = UART_LINK_MODE_ISP;
            ret = gpio_reset_pin(CONFIG_UART_LINK_TX_GPIO);
            if (ret == ESP_OK) ret = gpio_reset_pin(CONFIG_UART_LINK_RX_GPIO);
        }
    }
    bool rollback = ret != ESP_OK && acquired;
    xSemaphoreGive(s_lock);
    if (rollback) (void)uart_link_end_isp();
    return ret;
}

static esp_err_t write_locked(const uint8_t *data, size_t length)
{
    const int written = uart_write_bytes(UART_LINK_PORT, data, length);
    if (written > 0) s_state.tx_bytes += (uint32_t)written;
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_payload(const uint8_t *data, size_t length, bool append_crlf)
{
    if (!data || length == 0 || length > INT_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_ready && !s_stopping && s_state.mode == UART_LINK_MODE_DATA) {
        ret = write_locked(data, length);
        if (ret == ESP_OK && append_crlf) {
            static const uint8_t crlf[] = {'\r', '\n'};
            ret = write_locked(crlf, sizeof(crlf));
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t uart_link_send_raw(const uint8_t *data, size_t length)
{
    return send_payload(data, length, false);
}

esp_err_t uart_link_send(const uint8_t *data, size_t length)
{
#if CONFIG_UART_LINK_APPEND_CRLF
    return send_payload(data, length, true);
#else
    return send_payload(data, length, false);
#endif
}

void uart_link_snapshot(uart_link_snapshot_t *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *snapshot = s_state;
    xSemaphoreGive(s_lock);
}

uint32_t uart_link_tx_bytes(void)
{
    uart_link_snapshot_t snapshot;
    uart_link_snapshot(&snapshot);
    return snapshot.tx_bytes;
}
