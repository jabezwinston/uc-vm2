/*
 * ucvm - AT89S52 variant descriptor
 */
#include "mcs51_cpu.h"
#include "mcs51_periph.h"

static void at89s52_periph_init(mcs51_cpu_t *cpu)
{
    /* GPIO ports P0-P3 (read hooks for ext_pins, write hooks for bridge) */
    mcs51_gpio_init(cpu);

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
