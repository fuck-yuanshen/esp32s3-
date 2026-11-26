#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "stm32f1_flash.h"
#include "swd_wire.h"

/* The wire fixture records the line direction on each rising edge and only
 * returns bits during the target's response phase. */
typedef struct {
    char edges[256];
    size_t edge_count;
    uint8_t reply[64];
    size_t reply_count;
    size_t reply_index;
    bool driving;
    bool data;
} wire_fixture_t;

static void clock_line(void *context, bool high)
{
    wire_fixture_t *f = context;
    if (high) {
        assert(f->edge_count < sizeof(f->edges));
        f->edges[f->edge_count++] = f->driving ? (f->data ? '1' : '0') : '.';
    }
}
static void write_line(void *context, bool high)
{
    wire_fixture_t *f = context;
    assert(f->driving);
    f->data = high;
}
static bool read_line(void *context)
{
    wire_fixture_t *f = context;
    assert(!f->driving && f->reply_index < f->reply_count);
    return f->reply[f->reply_index++] != 0;
}
static void set_output(void *context, bool output)
{
    ((wire_fixture_t *)context)->driving = output;
}
static void delay_line(void *context)
{
    (void)context;
}
static void append_reply(wire_fixture_t *f, uint32_t value, unsigned bits)
{
    for (unsigned i = 0; i < bits; ++i) f->reply[f->reply_count++] = (value >> i) & 1U;
}
static void test_wire(void)
{
    wire_fixture_t f = {0};
    swd_wire_t w = {
        .io = {&f, clock_line, write_line, read_line, set_output, delay_line},
        .wait_retries = 1,
    };
    swd_wire_reset(&w);
    assert(f.edge_count == 152 && f.driving && f.data);
    assert(memcmp(f.edges + 64, "0111100111100111", 16) == 0);
    assert(memcmp(f.edges + 144, "00000000", 8) == 0);

    f.edge_count = 0;
    append_reply(&f, 2, 3); /* WAIT followed by OK */
    append_reply(&f, 1, 3);
    append_reply(&f, 0xA5A50001U, 32);
    append_reply(&f, 1, 1);
    uint32_t data = 0;
    assert(swd_wire_transfer(&w, false, true, 0, &data) == ESP_OK);
    assert(data == 0xA5A50001U && f.reply_index == f.reply_count);
    assert(memcmp(f.edges, "10100101.....00000000" "10100101", 29) == 0);
    assert(f.driving && f.data);

    f.edge_count = f.reply_index = f.reply_count = 0;
    append_reply(&f, 1, 3);
    data = 0x80000000U;
    assert(swd_wire_transfer(&w, true, false, 0xC, &data) == ESP_OK);
    assert(memcmp(f.edges, "11011101.....", 13) == 0);
    assert(f.edges[13] == '0' && f.edges[44] == '1' && f.edges[45] == '1');

    f.edge_count = f.reply_index = f.reply_count = 0;
    append_reply(&f, 1, 3);
    append_reply(&f, 1, 32);
    append_reply(&f, 0, 1);
    assert(swd_wire_transfer(&w, false, true, 0, &data) == ESP_ERR_INVALID_CRC);
    assert(data == 0x80000000U);

    f.edge_count = f.reply_index = f.reply_count = 0;
    append_reply(&f, 4, 3);
    assert(swd_wire_transfer(&w, false, true, 0, &data) == ESP_FAIL);
    assert(f.edge_count == 21 && f.driving);

    f.edge_count = f.reply_index = f.reply_count = 0;
    append_reply(&f, 2, 3);
    append_reply(&f, 2, 3);
    assert(swd_wire_transfer(&w, false, true, 0, &data) == ESP_ERR_TIMEOUT);
    assert(f.edge_count == 42 && f.driving);
    assert(w.last_request == 0xA5 && w.last_ack == 2);

    f.edge_count = f.reply_index = f.reply_count = 0;
    append_reply(&f, 7, 3); /* A pulled-up data line with no target response. */
    assert(swd_wire_transfer(&w, false, true, 0, &data) == ESP_ERR_INVALID_RESPONSE);
    assert(w.last_request == 0xA5 && w.last_ack == 7);
    assert(f.edge_count == 54 && f.driving);
}

/* An edge-driven DPIDR target advances on clocks, not read callbacks. It
 * checks request setup at the falling edge, bus release, ACK placement and
 * the 32-bit response independently of the queued-bit fixture above. */
typedef struct {
    unsigned cycle;
    bool high, driving, host_bit, target_bit;
} edge_target_t;

static unsigned id_bit(unsigned cycle)
{
    if (cycle >= 9 && cycle < 12) return (1U >> (cycle - 9)) & 1U;
    if (cycle >= 12 && cycle < 44) return (0x1BA01477U >> (cycle - 12)) & 1U;
    if (cycle == 44) return 0; /* DPIDR has even population count. */
    return 1;
}
static void edge_clock(void *context, bool high)
{
    edge_target_t *f = context;
    if (!high) {
        if (f->cycle < 8) {
            assert(f->driving);
            assert(f->host_bit == (((0xA5U >> f->cycle) & 1U) != 0));
        } else if (f->cycle <= 45) assert(!f->driving);
        else assert(f->driving);
        f->target_bit = id_bit(f->cycle) != 0;
    } else {
        assert(!f->high);
        f->cycle++;
    }
    f->high = high;
}
static void edge_data(void *context, bool high)
{
    edge_target_t *f = context;
    assert(f->driving);
    f->host_bit = high;
}
static bool edge_read(void *context)
{
    edge_target_t *f = context;
    assert(!f->high && !f->driving && f->cycle >= 9 && f->cycle <= 44);
    return f->target_bit;
}
static void edge_direction(void *context, bool driving)
{
    ((edge_target_t *)context)->driving = driving;
}
static void test_edge_target(void)
{
    edge_target_t f = {.driving = true, .host_bit = true};
    swd_wire_t w = {.io = {&f, edge_clock, edge_data, edge_read,
                           edge_direction, delay_line}};
    uint32_t id = 0;
    assert(swd_wire_transfer(&w, false, true, 0, &id) == ESP_OK);
    assert(id == 0x1BA01477U && w.last_ack == 1 && f.cycle == 54);
    assert(f.driving && !f.high && f.host_bit);
}

/* This target models posted MEM-AP reads and the flash register sequence
 * separately from the GPIO waveform. */
typedef struct {
    uint32_t address, csw, posted, control, status, flash_cr, demcr, word, obr;
    uint32_t erase_address;
    unsigned keys, halfwords, erases;
} target_fixture_t;

static uint32_t mem_read(const target_fixture_t *f)
{
    switch (f->address) {
    case 0xE000EDF0U: return 1U << 17;
    case 0xE000EDFCU: return f->demcr;
    case 0xE000ED00U: return 0x411FC231U;
    case 0xE0042000U: return 0x20036410U;
    case 0x1FFFF7E0U: return 64;
    case 0x1FFFF800U: return 0x5AA5U;
    case 0x4002200CU: return f->status;
    case 0x40022010U: return f->flash_cr;
    case 0x4002201CU: return f->obr;
    case STM32F1_FLASH_BASE: return f->word;
    default: assert(false); return 0;
    }
}
static void mem_write(target_fixture_t *f, uint32_t value)
{
    switch (f->address) {
    case 0xE000EDF0U:
        assert(value == 0xA05F0003U || value == 0xA05F0000U);
        break;
    case 0xE000EDFCU:
        f->demcr = value;
        break;
    case 0x40022004U:
        if (f->keys++ == 0) assert(value == 0x45670123U);
        else {
            assert(value == 0xCDEF89ABU);
            f->flash_cr = 0;
        }
        break;
    case 0x4002200CU:
        f->status &= ~value;
        break;
    case 0x40022010U:
        if (value == 0x42U) {
            assert(f->flash_cr == 2);
            assert(f->erase_address == STM32F1_FLASH_BASE);
            f->word = UINT32_MAX;
            f->erases++;
        }
        f->flash_cr = value;
        break;
    case 0x40022014U:
        f->erase_address = value;
        break;
    case STM32F1_FLASH_BASE:
    case STM32F1_FLASH_BASE + 2:
        assert(f->flash_cr == 1 && (f->csw & 7) == 1);
        if (f->address & 2) {
            assert((value & 0xFFFFU) == 0);
            f->word = (f->word & 0xFFFFU) | value;
        } else {
            assert(value <= 0xFFFFU);
            f->word = (f->word & 0xFFFF0000U) | value;
        }
        f->halfwords++;
        break;
    default: assert(false); break;
    }
}
static esp_err_t dap_transfer(void *context, bool ap, bool read, uint8_t reg, uint32_t *value)
{
    target_fixture_t *f = context;
    if (!ap) {
        if (read && reg == 0) *value = 0x1BA01477;
        else if (read && reg == 4) *value = f->control;
        else if (read && reg == 12) *value = f->posted;
        else if (!read && reg == 4) {
            assert(*value == 0x50000000U);
            f->control = 0xF0000000U;
        } else if (!read && reg == 0) {
            assert(*value == 0x1EU);
            f->control &= ~0xB2U;
        } else assert(!read && reg == 8 && *value == 0);
    } else if (read) {
        assert(reg == 12 && (f->csw & 7) == 2);
        *value = f->posted;
        f->posted = mem_read(f);
    } else if (reg == 0) f->csw = *value;
    else if (reg == 4) f->address = *value;
    else {
        assert(reg == 12);
        mem_write(f, *value);
    }
    return ESP_OK;
}

static int64_t simulated_us;
int64_t esp_timer_get_time(void) { return simulated_us; }
void vTaskDelay(TickType_t ticks) { simulated_us += ticks * 1000; }

static void test_flash(void)
{
    target_fixture_t f = {.demcr = 0x10, .flash_cr = 0x80, .status = 0x14, .obr = 2};
    arm_dap_t dap = {.context = &f, .transfer = dap_transfer};
    stm32f1_target_t target = {.dap = &dap};
    assert(arm_dap_connect(&dap) == ESP_OK);
    assert(stm32f1_prepare(&target) == ESP_OK);
    assert(f.demcr == 0x11);
    assert(stm32f1_halt_identify(&target) == ESP_ERR_INVALID_STATE);
    assert(stm32f1_finish(&target) == ESP_OK && f.flash_cr == 0x80 && f.keys == 0);
    f.obr = 0;
    assert(stm32f1_halt_identify(&target) == ESP_OK);
    assert(target.device_id == 0x410 && target.flash_bytes == 65536 && target.page_bytes == 1024);
    assert(stm32f1_unlock(&target) == ESP_OK && f.status == 0 && f.keys == 2);
    assert(stm32f1_erase_page(&target, STM32F1_FLASH_BASE + 65536) == ESP_ERR_INVALID_ARG);
    assert(stm32f1_erase_page(&target, STM32F1_FLASH_BASE + 2) == ESP_ERR_INVALID_ARG);
    assert(f.erases == 0);
    assert(stm32f1_erase_page(&target, STM32F1_FLASH_BASE) == ESP_OK);
    const uint8_t data[] = {0x42, 0x10, 0x11, 0x82};
    assert(stm32f1_program(&target, STM32F1_FLASH_BASE, data, sizeof(data)) == ESP_OK);
    assert(f.erases == 1 && f.halfwords == 2 && f.word == 0x82111042U);
    assert(stm32f1_verify(&target, STM32F1_FLASH_BASE, data, sizeof(data)) == ESP_OK);
    f.word ^= 1;
    assert(stm32f1_verify(&target, STM32F1_FLASH_BASE, data, sizeof(data)) == ESP_ERR_INVALID_CRC);
    f.control |= 0x20;
    uint32_t value;
    assert(arm_dap_read32(&dap, STM32F1_FLASH_BASE, &value) == ESP_FAIL);
    assert((f.control & 0x20) == 0);
    assert(stm32f1_finish(&target) == ESP_OK && f.flash_cr == 0x80 && f.demcr == 0x10);
    assert(stm32f1_disable_debug(&target) == ESP_OK);
}

int main(void)
{
    test_wire();
    test_edge_target();
    test_flash();
    puts("SWD wire, DAP and STM32F1 flash checks passed");
    return 0;
}
