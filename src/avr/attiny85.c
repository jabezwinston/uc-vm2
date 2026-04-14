/*
 * ucvm - ATtiny85 variant descriptor and peripheral init
 *
 * Register addresses from avr/iotn85.h.
 * All I/O regs are in the standard I/O space (IN/OUT accessible).
 * No extended I/O. SRAM starts at 0x60.
 */
#include "avr_cpu.h"
#include "avr_periph.h"
#include <stdlib.h>

/* ---------- Timer0 register addresses ---------- */
/* TCCR0A = I/O 0x2A → index 0x2A
 * TCCR0B = I/O 0x33 → index 0x33
 * TCNT0  = I/O 0x32 → index 0x32
 * OCR0A  = I/O 0x29 → index 0x29
 * OCR0B  = I/O 0x28 → index 0x28
 * TIMSK  = I/O 0x39 → index 0x39
 * TIFR   = I/O 0x38 → index 0x38
 */
static const avr_timer0_config_t timer0_config = {
    .tccr0a     = 0x2A,
    .tccr0b     = 0x33,
    .tcnt0      = 0x32,
    .ocr0a      = 0x29,
    .ocr0b      = 0x28,
    .timsk      = 0x39,
    .tifr       = 0x38,
    .toie_bit   = 1,    /* TOIE0 is bit 1 of TIMSK */
    .ocie0a_bit = 4,    /* OCIE0A is bit 4 of TIMSK */
    .tov_vec    = 5,    /* TIM0_OVF_vect */
    .oca_vec    = 10,   /* TIM0_COMPA_vect */
};

/* ---------- GPIO port configs ---------- */
/* Only PORTB: PINB=0x16, DDRB=0x17, PORTB=0x18 (6 pins: PB0-PB5) */
static const avr_gpio_port_config_t gpio_ports[] = {
    { .pin_io = 0x16, .ddr_io = 0x17, .port_io = 0x18, .port_id = 0, .num_pins = 6 },
};

/* ---------- Peripheral init ---------- */

static void attiny85_periph_init(avr_cpu_t *cpu)
{
    cpu->periph_timer = avr_timer0_init(cpu, &timer0_config);
    cpu->periph_gpio  = avr_gpio_init(cpu, gpio_ports, 1);
    /* No UART on ATtiny85 */
    cpu->periph_uart  = NULL;
}

/* ---------- Variant descriptor ---------- */

const avr_variant_t avr_attiny85 = {
    .name        = "ATtiny85",
    .flash_size  = 8192,   /* 8KB */
    .data_size   = 608,    /* 0x0000..0x025F */
    .sram_start  = 0x0060, /* after 32 regs + 64 I/O, no extended I/O */
    .eeprom_size = 512,
    .num_vectors = 15,
    .vector_size = 1,      /* 1 word per vector (RJMP instruction) */
    .flags       = AVR_FLAG_HAS_MOVW | AVR_FLAG_HAS_LPM_RD |
                   AVR_FLAG_HAS_ADIW | AVR_FLAG_HAS_BREAK,
    /* No MUL, no JMP/CALL on ATtiny85 */
    .periph_init = attiny85_periph_init,
};
