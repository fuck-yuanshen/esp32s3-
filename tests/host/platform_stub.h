#pragma once
/* Deterministic host substitutes for the ESP-IDF interfaces used by UART/ISP.
 * They validate ownership and failure cleanup, not electrical UART timing or
 * preemptive FreeRTOS scheduling. Production sources are compiled unchanged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NOT_FINISHED 5
#define ESP_ERR_INVALID_RESPONSE 6
#define ESP_ERR_NOT_SUPPORTED 7
#define ESP_ERR_INVALID_SIZE 8
#define ESP_ERR_NOT_FOUND 9
#define ESP_ERR_INVALID_CRC 10
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

#define CONFIG_UART_LINK_PORT 2
#define CONFIG_UART_LINK_TX_GPIO 19
#define CONFIG_UART_LINK_RX_GPIO 20
#define CONFIG_UART_LINK_BAUD 115200
#if HOST_APPEND_CRLF
#define CONFIG_UART_LINK_APPEND_CRLF 1
#endif
#define CONFIG_STM32_UART_BAUD 57600
#define CONFIG_STM32_BOOT0_GPIO 15
#define CONFIG_STM32_NRST_GPIO 16
#define CONFIG_STM32_BYTE_PACING_MS 1
#define CONFIG_STM32_DEFAULT_BASE 0x08000000u
#define CONFIG_STM32_DEFAULT_PAGE_SIZE 2048
#define CONFIG_STM32_MASS_ERASE_FALLBACK 1

typedef unsigned TickType_t;
typedef struct host_semaphore *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
#define portMAX_DELAY ((TickType_t)-1)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define taskYIELD() vTaskDelay(0)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
int xSemaphoreGive(SemaphoreHandle_t sem);
int xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
int64_t esp_timer_get_time(void);

typedef int uart_port_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int queue,
                              void *queue_handle, int flags);
esp_err_t uart_driver_delete(uart_port_t port);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t timeout);
esp_err_t uart_flush_input(uart_port_t port);
int uart_write_bytes(uart_port_t port, const void *data, size_t size);
int uart_read_bytes(uart_port_t port, void *data, uint32_t size, TickType_t timeout);

typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, int level);

esp_err_t gpio_reset_pin(int pin);
esp_err_t uart_disable_rx_intr(uart_port_t port);
esp_err_t uart_enable_rx_intr(uart_port_t port);
