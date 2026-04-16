/*
 * ucvm - MCS-51 Timer0/1 emulation
 *
 * Timer modes:
 *   Mode 0: 13-bit counter (TH:TL[4:0])
 *   Mode 1: 16-bit counter (TH:TL)
 *   Mode 2: 8-bit auto-reload (TL counts, TH holds reload value)
 *   Mode 3: Timer0 splits into two 8-bit counters
 *
 * Tick: one count per machine cycle (when TRx=1, Gate conditions met).
 * On overflow: set TFx in TCON.
 */
#include "mcs51_periph.h"
#include "mcs51_cpu.h"
#include <stdlib.h>

/* Timer0 tick — returns 1 if overflow occurred */
static int tick_timer(mcs51_cpu_t *cpu, int timer_num)
{
    uint8_t tmod = cpu->sfr[SFI(SFR_TMOD)];
    uint8_t tcon = cpu->sfr[SFI(SFR_TCON)];

    uint8_t mode, tr_bit, gate_bit, tf_bit;
    uint8_t *tl, *th;

    if (timer_num == 0) {
        mode    = tmod & 0x03;
        tr_bit  = TCON_TR0;
        gate_bit = 0; /* GATE0 = TMOD.3 — not emulating gate input */
        tf_bit  = TCON_TF0;
        tl = &cpu->sfr[SFI(SFR_TL0)];
        th = &cpu->sfr[SFI(SFR_TH0)];
    } else {
        mode    = (tmod >> 4) & 0x03;
        tr_bit  = TCON_TR1;
        gate_bit = 0;
        tf_bit  = TCON_TF1;
        tl = &cpu->sfr[SFI(SFR_TL1)];
        th = &cpu->sfr[SFI(SFR_TH1)];
    }

    (void)gate_bit;

    /* Check if timer is running */
    if (!(tcon & tr_bit))
        return 0;

    /* C/T bit: 0=timer (count machine cycles), 1=counter (external T0/T1 pin) */
    /* We only emulate timer mode (C/T=0) */

    uint16_t val;
    int overflow = 0;

    switch (mode) {
    case 0: /* 13-bit counter */
        val = (((uint16_t)*th) << 5) | (*tl & 0x1F);
        val++;
        if (val >= 0x2000) { /* 13-bit overflow */
            val = 0;
            overflow = 1;
        }
        *tl = (*tl & 0xE0) | (val & 0x1F);
        *th = (val >> 5) & 0xFF;
        break;

    case 1: /* 16-bit counter */
        val = ((uint16_t)*th << 8) | *tl;
        val++;
        if (val == 0) { /* 16-bit overflow */
            overflow = 1;
        }
        *tl = val & 0xFF;
        *th = (val >> 8) & 0xFF;
        break;

    case 2: /* 8-bit auto-reload */
        (*tl)++;
        if (*tl == 0) {
            *tl = *th; /* Reload from TH */
            overflow = 1;
        }
        break;

    case 3: /* Split mode */
        if (timer_num == 0) {
            /* TL0 uses Timer0 resources (TR0, TF0) */
            (*tl)++;
            if (*tl == 0) {
                overflow = 1;
            }
            /* TH0 uses Timer1 resources (TR1, TF1) — only if Timer1 not in mode 3 */
            if (tcon & TCON_TR1) {
                uint8_t th0 = cpu->sfr[SFI(SFR_TH0)];
                th0++;
                cpu->sfr[SFI(SFR_TH0)] = th0;
                if (th0 == 0) {
                    cpu->sfr[SFI(SFR_TCON)] |= TCON_TF1; /* Set TF1 */
                }
            }
        }
        /* Timer1 in mode 3 = stopped */
        break;
    }

    if (overflow) {
        cpu->sfr[SFI(SFR_TCON)] |= tf_bit;
    }

    return overflow;
}

/* SFR read/write hooks for timer registers — just use defaults */

mcs51_timer_t *mcs51_timer_init(mcs51_cpu_t *cpu)
{
    mcs51_timer_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    (void)cpu;
    return t;
}

void mcs51_timer_tick(mcs51_cpu_t *cpu, mcs51_timer_t *timer)
{
    (void)timer;
    tick_timer(cpu, 0);

    /* Timer1: in mode 2, overflow may generate baud rate for UART.
     * The UART's TI flag is set when SBUF TX completes.
     * For simplicity, we treat Timer1 overflow as a UART TX clock:
     * each Timer1 overflow advances the UART TX by one bit-time.
     * In mode 1 UART at 9600 baud, Timer1 overflows 9600 times/sec
     * (once per bit), and a full byte = 10 bit-times.
     * We simplify: UART TX completes on the first Timer1 overflow
     * after SBUF write (instant TX for emulation purposes). */
    int t1_ovf = tick_timer(cpu, 1);

    /* On Timer1 overflow in mode 2: handle UART TX completion */
    if (t1_ovf && cpu->periph_uart) {
        mcs51_uart_t *uart = cpu->periph_uart;
        if (uart->tx_active) {
            uart->tx_active = 0;
            cpu->sfr[SFI(SFR_SCON)] |= SCON_TI; /* Set TI — transmit complete */
        }
    }
}
