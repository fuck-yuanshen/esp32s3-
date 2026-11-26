/* STM32F1 single-bank flash controller over MEM-AP. Page erases and halfword
 * writes follow PM0075/RM0008. Option bytes and read protection are never changed.
 */
#include "stm32f1_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define F1_TRY(call) do { esp_err_t e_ = (call); if (e_ != ESP_OK) return e_; } while (0)
#define FLASH_KEYR 0x40022004U
#define FLASH_SR   0x4002200CU
#define FLASH_CR   0x40022010U
#define FLASH_AR   0x40022014U
#define FLASH_OBR  0x4002201CU
#define DHCSR      0xE000EDF0U
#define DEMCR      0xE000EDFCU
#define AIRCR      0xE000ED0CU
#define AIRCR_SYSRESETREQ 0x05FA0004U /* VECTKEY 0x05FA | SYSRESETREQ */
static esp_err_t rd(stm32f1_target_t *t, uint32_t a, uint32_t *v)
{ return arm_dap_read32(t->dap, a, v); }
static esp_err_t wr(stm32f1_target_t *t, uint32_t a, uint32_t v)
{ return arm_dap_write32(t->dap, a, v); }
static esp_err_t wait_flash(stm32f1_target_t *t, uint32_t timeout_ms, bool check_errors)
{
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        uint32_t sr;
        F1_TRY(rd(t, FLASH_SR, &sr));
        if (!(sr & 1U)) {
            if (check_errors && (sr & 0x14U)) return ESP_FAIL;
            return ESP_OK;
        }
        if (esp_timer_get_time() >= end) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
esp_err_t stm32f1_prepare(stm32f1_target_t *t)
{
    /* Preserve the original vector-catch setting across reconnect attempts. */
    if (!t->demcr_saved) {
        F1_TRY(rd(t, DEMCR, &t->saved_demcr));
        t->demcr_saved = true;
    }
    F1_TRY(wr(t, DEMCR, t->saved_demcr | 1U));
    return wr(t, DHCSR, 0xA05F0003U);
}
esp_err_t stm32f1_halt_identify(stm32f1_target_t *t)
{
    F1_TRY(wr(t, DHCSR, 0xA05F0003U));
    bool halted = false;
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t dhcsr;
        F1_TRY(rd(t, DHCSR, &dhcsr));
        if (dhcsr & (1U << 17)) { halted = true; break; }
        vTaskDelay(1);
    }
    if (!halted) return ESP_ERR_TIMEOUT;
    uint32_t id, size, option, obr;
    F1_TRY(rd(t, 0xE0042000, &id));
    t->device_id = (uint16_t)(id & 0xFFFU);
    switch (t->device_id) {
    case 0x412: case 0x410: t->page_bytes = 1024; break;
    case 0x414: t->page_bytes = 2048; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    /* Check the Cortex-M3 identity as well as DBGMCU's density identifier. */
    uint32_t cpuid;
    F1_TRY(rd(t, 0xE000ED00, &cpuid));
    if ((cpuid & 0xFF00FFF0U) != 0x4100C230U) return ESP_ERR_NOT_SUPPORTED;
    F1_TRY(rd(t, 0x1FFFF7E0, &size));
    size &= 0xFFFFU;
    if ((t->device_id == 0x412 && (size < 16 || size > 32)) ||
        (t->device_id == 0x410 && (size < 64 || size > 128)) ||
        (t->device_id == 0x414 && (size < 256 || size > 512)))
        return ESP_ERR_INVALID_RESPONSE;
    t->flash_bytes = size * 1024U;
    F1_TRY(rd(t, 0x1FFFF800, &option));
    F1_TRY(rd(t, FLASH_OBR, &obr));
    if ((option & 0xFFU) != 0xA5U || (obr & 2U)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
bool stm32f1_range_valid(const stm32f1_target_t *t, uint32_t address, size_t size)
{
    return size && address >= STM32F1_FLASH_BASE &&
           address - STM32F1_FLASH_BASE < t->flash_bytes &&
           size <= t->flash_bytes - (address - STM32F1_FLASH_BASE);
}
esp_err_t stm32f1_unlock(stm32f1_target_t *t)
{
    F1_TRY(wait_flash(t, 2000, false));
    uint32_t cr;
    F1_TRY(rd(t, FLASH_CR, &cr));
    t->lock_on_exit = true;
    if (cr & 0x80U) {
        F1_TRY(wr(t, FLASH_KEYR, 0x45670123));
        F1_TRY(wr(t, FLASH_KEYR, 0xCDEF89AB));
        F1_TRY(rd(t, FLASH_CR, &cr));
        if (cr & 0x80U) return ESP_ERR_INVALID_STATE;
    }
    return wr(t, FLASH_SR, 0x34);
}
esp_err_t stm32f1_erase_page(stm32f1_target_t *t, uint32_t address)
{
    if (!t->page_bytes || address % t->page_bytes ||
        !stm32f1_range_valid(t, address, t->page_bytes)) return ESP_ERR_INVALID_ARG;
    F1_TRY(wr(t, FLASH_SR, 0x34));
    F1_TRY(wr(t, FLASH_CR, 2));
    F1_TRY(wr(t, FLASH_AR, address));
    F1_TRY(wr(t, FLASH_CR, 0x42));
    F1_TRY(wait_flash(t, 2000, true));
    return wr(t, FLASH_CR, 0);
}
esp_err_t stm32f1_program(stm32f1_target_t *t, uint32_t address,
                         const uint8_t *data, size_t size)
{
    if (!data || (address & 1U) || (size & 1U) ||
        !stm32f1_range_valid(t, address, size)) return ESP_ERR_INVALID_ARG;
    F1_TRY(wr(t, FLASH_SR, 0x34));
    F1_TRY(wr(t, FLASH_CR, 1));
    for (size_t i = 0; i < size; i += 2) {
        const uint16_t half = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
        if (half != 0xFFFFU) {
            F1_TRY(arm_dap_write16(t->dap, address + (uint32_t)i, half));
            F1_TRY(wait_flash(t, 100, true));
        }
        /* A slow GPIO transport must yield even when the target never sets BSY. */
        if ((i & 63U) == 62U) vTaskDelay(1);
    }
    return wr(t, FLASH_CR, 0);
}
esp_err_t stm32f1_verify(stm32f1_target_t *t, uint32_t address,
                        const uint8_t *data, size_t size)
{
    if (!data || (address & 3U) || (size & 3U) ||
        !stm32f1_range_valid(t, address, size)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < size; i += 4) {
        uint32_t got;
        F1_TRY(rd(t, address + (uint32_t)i, &got));
        uint32_t expected = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                            ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
        if (got != expected) return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
esp_err_t stm32f1_finish(stm32f1_target_t *t)
{
    /* Best-effort cleanup attempts every operation even after a lost SWD link. */
    esp_err_t result = ESP_OK, e;
    if (t->lock_on_exit) {
        e = wr(t, FLASH_CR, 0x80);
        if (e != ESP_OK) result = e;
    }
    if (t->demcr_saved) {
        e = wr(t, DEMCR, t->saved_demcr);
        if (e != ESP_OK) result = e;
    }
    return result;
}
esp_err_t stm32f1_disable_debug(stm32f1_target_t *t)
{
    return t->demcr_saved ? wr(t, DHCSR, 0xA05F0000U) : ESP_OK;
}
esp_err_t stm32f1_reset_and_run(stm32f1_target_t *t)
{
    /* Clearing C_DEBUGEN alone resumes the halted PC, which still points inside
     * the image that was just replaced, so the new program never reaches main()
     * until the next power cycle. SYSRESETREQ is the only restart available
     * when NRST is not wired. VC_CORERESET must already be clear (stm32f1_finish
     * restores DEMCR) or the reset halts on the vector catch instead of running.
     */
    (void)wr(t, DHCSR, 0xA05F0000U);
    vTaskDelay(pdMS_TO_TICKS(2));
    /* Both writes can report a bus fault while the system reset is in flight,
     * so the debug port is re-read instead of trusting either result. */
    (void)wr(t, AIRCR, AIRCR_SYSRESETREQ);
    vTaskDelay(pdMS_TO_TICKS(20));
    (void)wr(t, DHCSR, 0xA05F0000U);
    uint32_t idcode = 0;
    esp_err_t e = t->dap->transfer(t->dap->context, false, true, 0, &idcode);
    if (e != ESP_OK) return e;
    return (idcode & 1U) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
