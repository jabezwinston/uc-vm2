/*
 * ucvm - AVR Timer0 peripheral emulation
 *
 * Supports Normal, CTC, Fast PWM, and Phase Correct PWM modes.
 * Prescaler divides CPU clock by 1/8/64/256/1024.
 */
#include "avr_periph.h"
#include "avr_cpu.h"
#include <stdlib.h>

/* Prescaler divisor lookup */
static const uint16_t prescaler_div[] = {
    0, 1, 8, 64, 256, 1024, 0, 0  /* 0=stopped, 6/7=external (not emulated) */
};

/* TIFR bit positions (same on both 328P and tiny85) */
#define TOV0_BIT  0  /* bit in TIFR for overflow */
#define OCF0A_BIT 1  /* bit in TIFR for compare A */
#define OCF0B_BIT 2  /* bit in TIFR for compare B */

/* I/O read handler: returns register value from data space */
static uint8_t timer_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

/* I/O write handler: store and process side effects */
static void timer_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_timer0_t *t = ctx;
    const avr_timer0_config_t *cfg = t->config;

    if (io_addr == cfg->tifr) {
        /* Writing 1 to a TIFR bit clears it */
        cpu->data[io_addr + 0x20] &= ~val;
        return;
    }
    /* Default: value already written by avr_io_write */
    (void)t;
}

avr_timer0_t *avr_timer0_init(avr_cpu_t *cpu, const avr_timer0_config_t *config)
{
    avr_timer0_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->config = config;
    t->counting_up = 1;

    /* Register I/O handlers */
    avr_io_register(cpu, config->tccr0a, timer_read, timer_write, t);
    avr_io_register(cpu, config->tccr0b, timer_read, timer_write, t);
    avr_io_register(cpu, config->tcnt0,  timer_read, timer_write, t);
    avr_io_register(cpu, config->ocr0a,  timer_read, timer_write, t);
    avr_io_register(cpu, config->ocr0b,  timer_read, timer_write, t);
    avr_io_register(cpu, config->timsk,  timer_read, timer_write, t);
    avr_io_register(cpu, config->tifr,   timer_read, timer_write, t);

    return t;
}

AVR_HOT void avr_timer0_tick(avr_cpu_t *cpu, avr_timer0_t *timer, uint16_t elapsed)
{
    const avr_timer0_config_t *cfg = timer->config;

    /* Read current config from data space */
    uint8_t tccr0a = cpu->data[cfg->tccr0a + 0x20];
    uint8_t tccr0b = cpu->data[cfg->tccr0b + 0x20];
    uint8_t cs = tccr0b & 0x07;

    if (cs == 0 || cs >= 6)
        return; /* Timer stopped or external clock (not emulated) */

    uint16_t div = prescaler_div[cs];
    timer->prescaler_count += elapsed;

    while (timer->prescaler_count >= div) {
        timer->prescaler_count -= div;

        /* Read timer registers */
        uint8_t tcnt  = cpu->data[cfg->tcnt0 + 0x20];
        uint8_t ocr0a = cpu->data[cfg->ocr0a + 0x20];
        uint8_t ocr0b = cpu->data[cfg->ocr0b + 0x20];
        uint8_t tifr  = cpu->data[cfg->tifr  + 0x20];
        uint8_t timsk = cpu->data[cfg->timsk + 0x20];

        /* Determine mode: WGM0[2:0] = {WGM02 from TCCR0B bit 3, WGM01:0 from TCCR0A bits 1:0} */
        uint8_t wgm = (tccr0a & 0x03) | ((tccr0b & 0x08) >> 1);
        /* wgm: 0=Normal, 1=PWM Phase Correct, 2=CTC, 3=Fast PWM,
         *      5=PWM PC top=OCR0A, 7=Fast PWM top=OCR0A */

        uint8_t top;
        switch (wgm) {
        case 2: case 5: case 7:
            top = ocr0a;
            break;
        default:
            top = 0xFF;
            break;
        }

        uint8_t old_tcnt = tcnt;

        if (wgm == 1 || wgm == 5) {
            /* Phase Correct PWM: count up then down */
            if (timer->counting_up) {
                tcnt++;
                if (tcnt >= top) {
                    tcnt = top;
                    timer->counting_up = 0;
                }
            } else {
                tcnt--;
                if (tcnt == 0) {
                    timer->counting_up = 1;
                    tifr |= (1 << TOV0_BIT); /* overflow at BOTTOM */
                }
            }
        } else {
            /* Normal, CTC, Fast PWM: count up */
            uint16_t next = (uint16_t)tcnt + 1;
            if (next > top) {
                /* Overflow / wrap */
                tcnt = 0;
                /* TOV0 set at MAX for Normal/CTC, at TOP for Fast PWM */
                if (wgm == 0 || wgm == 3) {
                    /* Normal / Fast PWM TOP=0xFF: overflow at 0xFF→0 */
                    tifr |= (1 << TOV0_BIT);
                } else if (wgm == 7) {
                    /* Fast PWM TOP=OCR0A: overflow at OCR0A→0 */
                    tifr |= (1 << TOV0_BIT);
                }
                /* CTC (wgm=2): clear at match, TOV0 set at real MAX */
            } else {
                tcnt = (uint8_t)next;
            }
            /* CTC overflow at 0xFF (rare — only if OCR0A > counter ever reaches 0xFF) */
            if (wgm == 2 && old_tcnt == 0xFF && tcnt == 0)
                tifr |= (1 << TOV0_BIT);
        }

        /* Compare match checks */
        if (tcnt == ocr0a && old_tcnt != ocr0a)
            tifr |= (1 << OCF0A_BIT);
        if (tcnt == ocr0b && old_tcnt != ocr0b)
            tifr |= (1 << OCF0B_BIT);

        /* Write back */
        cpu->data[cfg->tcnt0 + 0x20] = tcnt;
        cpu->data[cfg->tifr + 0x20]  = tifr;

        /* Set interrupt pending flags.
         * Auto-clear TIFR flags when queueing interrupt — simulates the
         * real AVR hardware behavior of clearing flags on ISR entry. */
        if ((tifr & (1 << TOV0_BIT)) && (timsk & (1 << cfg->toie_bit))) {
            cpu->irq_pending |= (1u << cfg->tov_vec);
            tifr &= ~(1 << TOV0_BIT);
            cpu->data[cfg->tifr + 0x20] = tifr;
        }
        if ((tifr & (1 << OCF0A_BIT)) && (timsk & (1 << cfg->ocie0a_bit))) {
            cpu->irq_pending |= (1u << cfg->oca_vec);
            tifr &= ~(1 << OCF0A_BIT);
            cpu->data[cfg->tifr + 0x20] = tifr;
        }
    }
}
