/*
 * ucvm - MCS-51 GPIO port emulation
 *
 * Models quasi-bidirectional ports P0-P3:
 *   - Write: sets output latch in SFR, fires bridge callback
 *   - Read: returns (latch & ext_pins) — external pull-low overrides weak pullup
 *
 * ext_pins[] defaults to 0xFF (all high, no external override).
 * The ESP32 I/O bridge updates ext_pins when physical pins change.
 */
#include "mcs51_periph.h"
#include "mcs51_cpu.h"
#include <stdlib.h>
#include <string.h>

/* Port read hook: combine output latch with external pin state.
 * On real 8051, reading a port pin reads the actual pin level.
 * pin = latch AND ext (when latch=1/weak pullup, ext can pull low). */
static uint8_t port_read(mcs51_cpu_t *cpu, uint8_t addr, void *ctx)
{
    mcs51_gpio_t *gpio = ctx;
    uint8_t latch = cpu->sfr[SFI(addr)];
    uint8_t port_id;

    switch (addr) {
    case SFR_P0: port_id = 0; break;
    case SFR_P1: port_id = 1; break;
    case SFR_P2: port_id = 2; break;
    case SFR_P3: port_id = 3; break;
    default: return latch;
    }

    return latch & gpio->ext_pins[port_id];
}

/* Port write hook: store latch in SFR (already done by caller),
 * then fire bridge callback for output notification. */
static void port_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val, void *ctx)
{
    (void)ctx;
    if (!cpu->bridge_cb) return;

    uint8_t port_id;
    switch (addr) {
    case SFR_P0: port_id = 0; break;
    case SFR_P1: port_id = 1; break;
    case SFR_P2: port_id = 2; break;
    case SFR_P3: port_id = 3; break;
    default: return;
    }
    cpu->bridge_cb(cpu->bridge_ctx, IO_PERIPH_GPIO, port_id, val);
}

mcs51_gpio_t *mcs51_gpio_init(mcs51_cpu_t *cpu)
{
    mcs51_gpio_t *gpio = calloc(1, sizeof(*gpio));
    if (!gpio) return NULL;

    /* Default: all external pins high (no override) */
    memset(gpio->ext_pins, 0xFF, sizeof(gpio->ext_pins));

    /* Register read + write hooks for all 4 ports */
    mcs51_sfr_register(cpu, SFR_P0, port_read, port_write, gpio);
    mcs51_sfr_register(cpu, SFR_P1, port_read, port_write, gpio);
    mcs51_sfr_register(cpu, SFR_P2, port_read, port_write, gpio);
    mcs51_sfr_register(cpu, SFR_P3, port_read, port_write, gpio);

    cpu->periph_gpio = gpio;
    return gpio;
}
