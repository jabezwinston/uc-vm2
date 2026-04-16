/*
 * ucvm - MCS-51 io_mcu_ops_t implementation
 */
#include "mcs51_cpu.h"
#include "mcs51_periph.h"
#include "../io/io_bridge.h"
#include "../util/ihex.h"
#include <string.h>

/* ---- CPU management ---- */

static uint8_t mcs51_get_state(void *cpu)
{ return ((mcs51_cpu_t *)cpu)->state; }

static void mcs51_set_state(void *cpu, uint8_t state)
{ ((mcs51_cpu_t *)cpu)->state = state; }

static uint64_t mcs51_get_cycles(void *cpu)
{ return ((mcs51_cpu_t *)cpu)->cycles; }

static uint16_t mcs51_get_pc(void *cpu)
{ return ((mcs51_cpu_t *)cpu)->pc; }

static const char *mcs51_get_variant(void *cpu)
{
    mcs51_cpu_t *c = cpu;
    return c->variant ? c->variant->name : "8051";
}

static void mcs51_do_reset(void *cpu)
{ mcs51_cpu_reset(cpu); }

static int mcs51_load_fw(void *cpu, const char *path)
{
    mcs51_cpu_t *c = cpu;
    memset(c->code, 0xFF, c->code_size);
    if (fw_load_bytes(path, c->code, c->code_size) != 0)
        return -1;
    mcs51_cpu_reset(c);
    return 0;
}

/* ---- Bridge callback ---- */

static void mcs51_install_cb(void *cpu, io_bridge_cb_t cb, void *ctx)
{
    mcs51_cpu_t *c = cpu;
    c->bridge_cb  = cb;
    c->bridge_ctx = ctx;
}

/* ---- Peripheral I/O ---- */

static void mcs51_uart_push(void *cpu, uint8_t channel, uint8_t byte)
{
    (void)channel;
    mcs51_cpu_t *c = cpu;
    if (c->periph_uart)
        mcs51_uart_rx_push(c, c->periph_uart, byte);
}

static int mcs51_uart_pop(void *cpu, uint8_t channel)
{
    (void)channel;
    mcs51_cpu_t *c = cpu;
    return c->periph_uart ? mcs51_uart_tx_pop(c->periph_uart) : -1;
}

static uint8_t *mcs51_ext_pins(void *cpu)
{
    mcs51_gpio_t *g = ((mcs51_cpu_t *)cpu)->periph_gpio;
    return g ? g->ext_pins : NULL;
}

static void mcs51_i2c_attach(void *cpu, uint8_t instance, void *bus_ops)
{
    (void)cpu; (void)instance; (void)bus_ops;
}

/* ---- Public instance ---- */

const io_mcu_ops_t mcs51_mcu_ops = {
    .arch_name         = "8051",
    .get_state         = mcs51_get_state,
    .set_state         = mcs51_set_state,
    .get_cycles        = mcs51_get_cycles,
    .get_pc            = mcs51_get_pc,
    .get_variant       = mcs51_get_variant,
    .reset             = mcs51_do_reset,
    .load_firmware     = mcs51_load_fw,
    .install_bridge_cb = mcs51_install_cb,
    .uart_rx_push      = mcs51_uart_push,
    .uart_tx_pop       = mcs51_uart_pop,
    .gpio_ext_pins     = mcs51_ext_pins,
    .i2c_attach_bus    = mcs51_i2c_attach,
};
