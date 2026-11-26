#include "swd_wire.h"

static void pulse(swd_wire_t *w)
{
    w->io.clock(w->io.context, false);
    w->io.delay(w->io.context);
    w->io.clock(w->io.context, true);
    w->io.delay(w->io.context);
}
static void put(swd_wire_t *w, unsigned bit)
{
    /* Match CMSIS-DAP SW_WRITE_BIT: establish data before the falling edge
     * and retain it throughout the following rising edge and hold interval. */
    w->io.data(w->io.context, (bit & 1U) != 0);
    w->io.clock(w->io.context, false);
    w->io.delay(w->io.context);
    w->io.clock(w->io.context, true);
    w->io.delay(w->io.context);
}
static unsigned get(swd_wire_t *w)
{
    w->io.clock(w->io.context, false);
    w->io.delay(w->io.context);
    unsigned bit = w->io.read(w->io.context) ? 1U : 0U;
    w->io.clock(w->io.context, true);
    w->io.delay(w->io.context);
    return bit;
}
static unsigned parity32(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1U;
}
static void idle(swd_wire_t *w)
{
    for (unsigned i = 0; i < 8; ++i) put(w, 0);
    /* Change data only with the clock low. */
    w->io.clock(w->io.context, false);
    w->io.data(w->io.context, true);
}
void swd_wire_reset(swd_wire_t *w)
{
    w->io.output(w->io.context, true);
    for (unsigned i = 0; i < 64; ++i) put(w, 1);
    for (unsigned i = 0; i < 16; ++i) put(w, 0xE79EU >> i);
    for (unsigned i = 0; i < 64; ++i) put(w, 1);
    idle(w);
}
esp_err_t swd_wire_transfer(void *context, bool ap, bool read,
                            uint8_t address, uint32_t *value)
{
    swd_wire_t *w = context;
    if (!w || !value || (address & ~0xCU)) return ESP_ERR_INVALID_ARG;
    const unsigned fields = (ap ? 1U : 0U) | (read ? 2U : 0U) | (address & 0xCU);
    const unsigned request = 0x81U | (fields << 1) | (parity32(fields) << 5);
    w->last_request = (uint8_t)request;
    for (unsigned attempt = 0; attempt <= w->wait_retries; ++attempt) {
        for (unsigned i = 0; i < 8; ++i) put(w, request >> i);
        w->io.output(w->io.context, false);
        pulse(w);  /* host -> target turnaround */
        unsigned ack = get(w);
        ack |= get(w) << 1;
        ack |= get(w) << 2;
        w->last_ack = (uint8_t)ack;
        if (ack == 1) {
            esp_err_t result = ESP_OK;
            if (read) {
                uint32_t data = 0;
                for (unsigned i = 0; i < 32; ++i) data |= (uint32_t)get(w) << i;
                if (get(w) != parity32(data)) result = ESP_ERR_INVALID_CRC;
                else *value = data;
                pulse(w);
                w->io.output(w->io.context, true);
            } else {
                pulse(w);
                w->io.output(w->io.context, true);
                for (unsigned i = 0; i < 32; ++i) put(w, *value >> i);
                put(w, parity32(*value));
            }
            idle(w);
            return result;
        }
        /* ORUNDETECT stays clear: WAIT/FAULT have no data phase. An invalid
         * ACK needs backoff clocks before the host drives the bus again.
         */
        unsigned clocks = (ack == 2 || ack == 4) ? 1 : 34;
        while (clocks--) pulse(w);
        w->io.output(w->io.context, true);
        idle(w);
        if (ack == 4) return ESP_FAIL;
        if (ack != 2) return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_TIMEOUT;
}
