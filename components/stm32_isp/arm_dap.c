#include "arm_dap.h"

#define DAP_TRY(call) do { esp_err_t e_ = (call); if (e_ != ESP_OK) return e_; } while (0)
static esp_err_t dp_write(arm_dap_t *d, uint8_t a, uint32_t v)
{ return d->transfer(d->context, false, false, a, &v); }
static esp_err_t dp_read(arm_dap_t *d, uint8_t a, uint32_t *v)
{ return d->transfer(d->context, false, true, a, v); }
static esp_err_t ap_write(arm_dap_t *d, uint8_t a, uint32_t v)
{ return d->transfer(d->context, true, false, a, &v); }
static esp_err_t finish(arm_dap_t *d)
{
    uint32_t v;
    DAP_TRY(dp_read(d, 0xC, &v));
    DAP_TRY(dp_read(d, 4, &v));
    if (v & 0xB2U) {
        (void)dp_write(d, 0, 0x1EU);
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t arm_dap_connect(arm_dap_t *d)
{
    d->idcode = 0;
    d->connect_step = "DPIDR read";
    DAP_TRY(dp_read(d, 0, &d->idcode));
    if (d->idcode == 0 || d->idcode == UINT32_MAX || !(d->idcode & 1))
        return ESP_ERR_INVALID_RESPONSE;
    d->connect_step = "ABORT write";
    DAP_TRY(dp_write(d, 0, 0x1E));
    d->connect_step = "SELECT write";
    DAP_TRY(dp_write(d, 8, 0));
    d->connect_step = "CTRL/STAT power request";
    DAP_TRY(dp_write(d, 4, 0x50000000));
    d->connect_step = "CTRL/STAT power acknowledge";
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t ctrl;
        DAP_TRY(dp_read(d, 4, &ctrl));
        if ((ctrl & 0xA0000000U) == 0xA0000000U) return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t setup(arm_dap_t *d, uint32_t address, uint32_t csw)
{
    DAP_TRY(dp_write(d, 8, 0));
    DAP_TRY(ap_write(d, 0, csw));
    return ap_write(d, 4, address);
}
esp_err_t arm_dap_read32(arm_dap_t *d, uint32_t address, uint32_t *value)
{
    if (!value || (address & 3U)) return ESP_ERR_INVALID_ARG;
    DAP_TRY(setup(d, address, 0x23000042));
    uint32_t posted;
    DAP_TRY(d->transfer(d->context, true, true, 0xC, &posted));
    DAP_TRY(dp_read(d, 0xC, value));
    return finish(d);
}
esp_err_t arm_dap_write32(arm_dap_t *d, uint32_t address, uint32_t value)
{
    if (address & 3U) return ESP_ERR_INVALID_ARG;
    DAP_TRY(setup(d, address, 0x23000042));
    DAP_TRY(ap_write(d, 0xC, value));
    return finish(d);
}
esp_err_t arm_dap_write16(arm_dap_t *d, uint32_t address, uint16_t value)
{
    if (address & 1U) return ESP_ERR_INVALID_ARG;
    DAP_TRY(setup(d, address, 0x23000041));
    DAP_TRY(ap_write(d, 0xC, (uint32_t)value << ((address & 2U) * 8)));
    return finish(d);
}
