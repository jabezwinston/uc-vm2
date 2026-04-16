/*
 * ucvm - AVR GPIO peripheral emulation
 *
 * Handles PINx/DDRx/PORTx register triplets.
 * Writing to PINx toggles corresponding PORTx bits (328P feature).
 * PORTx changes trigger the I/O bridge callback.
 */
#include "avr_periph.h"
#include "avr_cpu.h"
#include <stdlib.h>
#include <string.h>

/* ---------- I/O handlers ---------- */

static uint8_t pin_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    avr_gpio_t *gpio = ctx;
    /* Find which port this is */
    for (uint8_t i = 0; i < gpio->num_ports; i++) {
        if (gpio->ports[i].pin_io == io_addr) {
            uint8_t ddr  = cpu->data[gpio->ports[i].ddr_io + 0x20];
            uint8_t port = cpu->data[gpio->ports[i].port_io + 0x20];
            uint8_t ext  = gpio->ext_pins[i];
            /* Output pins return PORTx value, input pins return external value */
            return (ddr & port) | (~ddr & ext);
        }
    }
    return cpu->data[io_addr + 0x20];
}

static void pin_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_gpio_t *gpio = ctx;
    /* Writing to PINx toggles PORTx bits (where val bits are 1) */
    for (uint8_t i = 0; i < gpio->num_ports; i++) {
        if (gpio->ports[i].pin_io == io_addr) {
            uint8_t port_io = gpio->ports[i].port_io;
            cpu->data[port_io + 0x20] ^= val;
            /* Don't store val to PINx data space — PINx is read-only storage */
            cpu->data[io_addr + 0x20] = 0;
            /* Trigger bridge callback */
            if (cpu->bridge_cb) {
                cpu->bridge_cb(cpu->bridge_ctx, IO_PERIPH_GPIO,
                               gpio->ports[i].port_id,
                               cpu->data[port_io + 0x20]);
            }
            return;
        }
    }
}

static uint8_t port_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

static void port_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_gpio_t *gpio = ctx;
    /* Trigger bridge callback on port change */
    for (uint8_t i = 0; i < gpio->num_ports; i++) {
        if (gpio->ports[i].port_io == io_addr) {
            if (cpu->bridge_cb) {
                cpu->bridge_cb(cpu->bridge_ctx, IO_PERIPH_GPIO,
                               gpio->ports[i].port_id, val);
            }
            return;
        }
    }
}

static uint8_t ddr_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

static void ddr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)cpu; (void)io_addr; (void)val; (void)ctx;
}

/* ---------- Init ---------- */

avr_gpio_t *avr_gpio_init(avr_cpu_t *cpu,
                           const avr_gpio_port_config_t *ports, uint8_t num_ports)
{
    avr_gpio_t *gpio = calloc(1, sizeof(*gpio));
    if (!gpio) return NULL;
    gpio->ports = ports;
    gpio->num_ports = num_ports;

    for (uint8_t i = 0; i < num_ports; i++) {
        avr_io_register(cpu, ports[i].pin_io,  pin_read,  pin_write,  gpio);
        avr_io_register(cpu, ports[i].ddr_io,  ddr_read,  ddr_write,  gpio);
        avr_io_register(cpu, ports[i].port_io, port_read,  port_write, gpio);
    }

    return gpio;
}
