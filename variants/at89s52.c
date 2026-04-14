/*
 * ucvm - AT89S52 variant descriptor
 */
#include "src/mcs51/mcs51_cpu.h"
#include "src/mcs51/mcs51_periph.h"

/* GPIO: fire bridge callback on P0-P3 writes */
static void port_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val, void *ctx)
{
    (void)ctx;
    if (cpu->bridge_cb) {
        uint8_t port_id;
        switch (addr) {
        case SFR_P0: port_id = 0; break;
        case SFR_P1: port_id = 1; break;
        case SFR_P2: port_id = 2; break;
        case SFR_P3: port_id = 3; break;
        default: return;
        }
        cpu->bridge_cb(cpu->bridge_ctx, 1 /* GPIO */, port_id, val);
    }
}

static void at89s52_periph_init(mcs51_cpu_t *cpu)
{
    /* GPIO port hooks */
    mcs51_sfr_register(cpu, SFR_P0, NULL, port_write, NULL);
    mcs51_sfr_register(cpu, SFR_P1, NULL, port_write, NULL);
    mcs51_sfr_register(cpu, SFR_P2, NULL, port_write, NULL);
    mcs51_sfr_register(cpu, SFR_P3, NULL, port_write, NULL);

    /* Timer0/1 */
    cpu->periph_timer = mcs51_timer_init(cpu);

    /* UART */
    cpu->periph_uart = mcs51_uart_init(cpu);
}

const mcs51_variant_t mcs51_at89s52 = {
    .name           = "AT89S52",
    .code_size      = 8192,
    .iram_size      = 256,
    .xram_size      = 256,
    .has_timer2     = 1,
    .num_interrupts = 6,
    .periph_init    = at89s52_periph_init,
};
