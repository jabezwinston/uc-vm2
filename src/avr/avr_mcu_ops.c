/*
 * ucvm - AVR io_mcu_ops_t implementation
 *
 * Provides arch-neutral access to AVR CPU and peripherals so that
 * the bridge, webserver, and platform code need no #ifdef.
 */
#include "avr_cpu.h"
#include "avr_periph.h"
#include "../io/io_bridge.h"
#include "../util/ihex.h"
#include <string.h>

/* ---- CPU management ---- */

static uint8_t avr_get_state(void *cpu)
{ return ((avr_cpu_t *)cpu)->state; }

static void avr_set_state(void *cpu, uint8_t state)
{ ((avr_cpu_t *)cpu)->state = state; }

static uint64_t avr_get_cycles(void *cpu)
{ return ((avr_cpu_t *)cpu)->cycles; }

static uint16_t avr_get_pc(void *cpu)
{ return ((avr_cpu_t *)cpu)->pc; }

static const char *avr_get_variant(void *cpu)
{
    avr_cpu_t *c = cpu;
    return c->variant ? c->variant->name : "avr";
}

static void avr_do_reset(void *cpu)
{ avr_cpu_reset(cpu); }

static int avr_load_fw(void *cpu, const char *path)
{
    avr_cpu_t *c = cpu;
    uint16_t *flash = (uint16_t *)c->flash;
    uint32_t words = c->flash_size / 2;
    memset(flash, 0xFF, words * 2);
    if (fw_load(path, flash, words) != 0)
        return -1;
    avr_cpu_reset(c);
    return 0;
}

/* ---- Bridge callback ---- */

static void avr_install_cb(void *cpu, io_bridge_cb_t cb, void *ctx)
{
    avr_cpu_t *c = cpu;
    c->bridge_cb  = cb;
    c->bridge_ctx = ctx;
}

/* ---- Peripheral I/O ---- */

static void avr_uart_push(void *cpu, uint8_t channel, uint8_t byte)
{
    (void)channel;
    avr_cpu_t *c = cpu;
    if (c->periph_uart)
        avr_uart_rx_push(c, c->periph_uart, byte);
}

static int avr_uart_pop(void *cpu, uint8_t channel)
{
    (void)channel;
    avr_cpu_t *c = cpu;
    return c->periph_uart ? avr_uart_tx_pop(c->periph_uart) : -1;
}

static uint8_t *avr_ext_pins(void *cpu)
{
    avr_gpio_t *g = ((avr_cpu_t *)cpu)->periph_gpio;
    return g ? g->ext_pins : NULL;
}

static void avr_i2c_attach(void *cpu, uint8_t instance, void *bus_ops)
{
    (void)instance;
    avr_cpu_t *c = cpu;
    if (c->periph_twi)
        avr_twi_set_bus(c->periph_twi, bus_ops);
}

/* ---- Public instance ---- */

const io_mcu_ops_t avr_mcu_ops = {
    .arch_name         = "avr",
    .get_state         = avr_get_state,
    .set_state         = avr_set_state,
    .get_cycles        = avr_get_cycles,
    .get_pc            = avr_get_pc,
    .get_variant       = avr_get_variant,
    .reset             = avr_do_reset,
    .load_firmware     = avr_load_fw,
    .install_bridge_cb = avr_install_cb,
    .uart_rx_push      = avr_uart_push,
    .uart_tx_pop       = avr_uart_pop,
    .gpio_ext_pins     = avr_ext_pins,
    .i2c_attach_bus    = avr_i2c_attach,
};
