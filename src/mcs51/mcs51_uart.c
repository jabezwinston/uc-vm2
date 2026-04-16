/*
 * ucvm - MCS-51 UART emulation
 *
 * SCON (0x98): SM0, SM1, SM2, REN, TB8, RB8, TI, RI
 * SBUF (0x99): write = TX data, read = RX data
 *
 * Mode 1 (8-bit UART) is the primary target.
 * TX: write to SBUF triggers TX, TI set on completion (driven by Timer1 overflow).
 * RX: data pushed via mcs51_uart_rx_push(), RI set on receive complete.
 */
#include "mcs51_periph.h"
#include "mcs51_cpu.h"
#include <stdlib.h>

static inline int ring_empty(uint8_t h, uint8_t t) { return h == t; }
static inline int ring_full(uint8_t h, uint8_t t) { return ((h + 1) & (MCS51_UART_BUF_SIZE - 1)) == t; }

/* SFR write hook for SBUF (TX) */
static void sbuf_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val, void *ctx)
{
    (void)addr;
    mcs51_uart_t *uart = ctx;

    /* Check TX is enabled (any mode except mode 0 needs SM1 or SM0) */
    /* For simplicity, always allow TX */

    /* Put in TX buffer */
    if (!ring_full(uart->tx_head, uart->tx_tail)) {
        uart->tx_buf[uart->tx_head] = val;
        uart->tx_head = (uart->tx_head + 1) & (MCS51_UART_BUF_SIZE - 1);
    }
    uart->tx_active = 1;

    /* Fire bridge callback immediately for console output */
    if (cpu->bridge_cb)
        cpu->bridge_cb(cpu->bridge_ctx, IO_PERIPH_UART, 0, val);
}

/* SFR read hook for SBUF (RX) */
static uint8_t sbuf_read(mcs51_cpu_t *cpu, uint8_t addr, void *ctx)
{
    (void)addr; (void)cpu;
    mcs51_uart_t *uart = ctx;

    if (ring_empty(uart->rx_head, uart->rx_tail))
        return 0;

    uint8_t val = uart->rx_buf[uart->rx_tail];
    uart->rx_tail = (uart->rx_tail + 1) & (MCS51_UART_BUF_SIZE - 1);
    return val;
}

mcs51_uart_t *mcs51_uart_init(mcs51_cpu_t *cpu)
{
    mcs51_uart_t *uart = calloc(1, sizeof(*uart));
    if (!uart) return NULL;

    mcs51_sfr_register(cpu, SFR_SBUF, sbuf_read, sbuf_write, uart);
    return uart;
}

void mcs51_uart_rx_push(mcs51_cpu_t *cpu, mcs51_uart_t *uart, uint8_t byte)
{
    if (!uart) return;
    uint8_t scon = cpu->sfr[SFI(SFR_SCON)];
    if (!(scon & SCON_REN)) return; /* RX not enabled */

    if (!ring_full(uart->rx_head, uart->rx_tail)) {
        uart->rx_buf[uart->rx_head] = byte;
        uart->rx_head = (uart->rx_head + 1) & (MCS51_UART_BUF_SIZE - 1);
    }
    /* Store in SBUF (the read register) and set RI */
    cpu->sfr[SFI(SFR_SBUF)] = byte;
    cpu->sfr[SFI(SFR_SCON)] |= SCON_RI;
}

int mcs51_uart_tx_pop(mcs51_uart_t *uart)
{
    if (!uart || ring_empty(uart->tx_head, uart->tx_tail))
        return -1;
    uint8_t val = uart->tx_buf[uart->tx_tail];
    uart->tx_tail = (uart->tx_tail + 1) & (MCS51_UART_BUF_SIZE - 1);
    return val;
}
