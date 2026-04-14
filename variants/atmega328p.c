/*
 * ucvm - ATMega328P variant descriptor and peripheral init
 *
 * Register addresses from avr/iom328p.h:
 *   I/O regs (IN/OUT accessible): data addr = io_addr + 0x20
 *   Extended I/O (LDS/STS only): data addr directly
 *   Handler table index = data_addr - 0x20
 */
#include "src/core/avr_cpu.h"
#include "src/periph/avr_periph.h"
#include <stdlib.h>

/* ---------- Timer0 register addresses ---------- */
/* TIFR0 = I/O 0x15  → index 0x15
 * TCCR0A = I/O 0x24 → index 0x24
 * TCCR0B = I/O 0x25 → index 0x25
 * TCNT0  = I/O 0x26 → index 0x26
 * OCR0A  = I/O 0x27 → index 0x27
 * OCR0B  = I/O 0x28 → index 0x28
 * TIMSK0 = mem 0x6E → index 0x4E
 */
static const avr_timer0_config_t timer0_config = {
    .tccr0a    = 0x24,
    .tccr0b    = 0x25,
    .tcnt0     = 0x26,
    .ocr0a     = 0x27,
    .ocr0b     = 0x28,
    .timsk     = 0x4E,  /* 0x6E - 0x20 */
    .tifr      = 0x15,
    .toie_bit  = 0,     /* TOIE0 is bit 0 of TIMSK0 */
    .ocie0a_bit = 1,    /* OCIE0A is bit 1 of TIMSK0 */
    .tov_vec   = 16,    /* TIMER0_OVF_vect */
    .oca_vec   = 14,    /* TIMER0_COMPA_vect */
};

/* ---------- GPIO port configs ---------- */
/* PINB=0x03, DDRB=0x04, PORTB=0x05 (8 pins)
 * PINC=0x06, DDRC=0x07, PORTC=0x08 (7 pins: PC0-PC6)
 * PIND=0x09, DDRD=0x0A, PORTD=0x0B (8 pins)
 */
static const avr_gpio_port_config_t gpio_ports[] = {
    { .pin_io = 0x03, .ddr_io = 0x04, .port_io = 0x05, .port_id = 0, .num_pins = 8 }, /* PORTB */
    { .pin_io = 0x06, .ddr_io = 0x07, .port_io = 0x08, .port_id = 1, .num_pins = 7 }, /* PORTC */
    { .pin_io = 0x09, .ddr_io = 0x0A, .port_io = 0x0B, .port_id = 2, .num_pins = 8 }, /* PORTD */
};

/* ---------- UART config ---------- */
/* UCSR0A = mem 0xC0 → index 0xA0
 * UCSR0B = mem 0xC1 → index 0xA1
 * UCSR0C = mem 0xC2 → index 0xA2
 * UBRR0L = mem 0xC4 → index 0xA4
 * UBRR0H = mem 0xC5 → index 0xA5
 * UDR0   = mem 0xC6 → index 0xA6
 */
static const avr_uart_config_t uart_config = {
    .udr_io   = 0xA6,
    .ucsra_io = 0xA0,
    .ucsrb_io = 0xA1,
    .ucsrc_io = 0xA2,
    .ubrrl_io = 0xA4,
    .ubrrh_io = 0xA5,
    .rxc_vec  = 18,   /* USART_RX_vect */
    .udre_vec = 19,   /* USART_UDRE_vect */
    .txc_vec  = 20,   /* USART_TX_vect */
};

/* ---------- Peripheral init ---------- */

static void atmega328p_periph_init(avr_cpu_t *cpu)
{
    cpu->periph_timer = avr_timer0_init(cpu, &timer0_config);
    cpu->periph_gpio  = avr_gpio_init(cpu, gpio_ports, 3);
    cpu->periph_uart  = avr_uart_init(cpu, &uart_config);
}

/* ---------- Variant descriptor ---------- */

const avr_variant_t avr_atmega328p = {
    .name        = "ATmega328P",
    .flash_size  = 32768,  /* 32KB */
    .data_size   = 2304,   /* 0x0000..0x08FF */
    .sram_start  = 0x0100, /* after 32 regs + 64 I/O + 160 ext I/O */
    .eeprom_size = 1024,
    .num_vectors = 26,
    .vector_size = 2,      /* 2 words per vector (JMP instruction) */
    .flags       = AVR_FLAG_HAS_MUL | AVR_FLAG_HAS_JMP_CALL |
                   AVR_FLAG_HAS_MOVW | AVR_FLAG_HAS_LPM_RD |
                   AVR_FLAG_HAS_ADIW | AVR_FLAG_HAS_BREAK,
    .periph_init = atmega328p_periph_init,
};
