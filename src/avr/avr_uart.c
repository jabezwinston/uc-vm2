/*
 * ucvm - AVR UART peripheral emulation
 *
 * Emulates USART0 for ATMega328P.
 * TX: writes to UDR0 go into a ring buffer, consumed by the I/O bridge.
 * RX: external data pushed into ring buffer, read via UDR0.
 * Status flags (UDRE, TXC, RXC) maintained automatically.
 */
#include "avr_periph.h"
#include "avr_cpu.h"
#include <stdlib.h>

/* I/O bridge types */
#define IO_BRIDGE_UART 2

/* UCSR0A bits */
#define UCSR_RXC   0x80  /* Receive Complete */
#define UCSR_TXC   0x40  /* Transmit Complete */
#define UCSR_UDRE  0x20  /* Data Register Empty */
#define UCSR_FE    0x10  /* Frame Error */
#define UCSR_DOR   0x08  /* Data OverRun */
#define UCSR_UPE   0x04  /* Parity Error */
#define UCSR_U2X   0x02  /* Double Speed */
#define UCSR_MPCM  0x01  /* Multi-processor Comm Mode */

/* UCSR0B bits */
#define UCSRB_RXCIE 0x80  /* RX Complete Interrupt Enable */
#define UCSRB_TXCIE 0x40  /* TX Complete Interrupt Enable */
#define UCSRB_UDRIE 0x20  /* Data Register Empty Interrupt Enable */
#define UCSRB_RXEN  0x10  /* Receiver Enable */
#define UCSRB_TXEN  0x08  /* Transmitter Enable */

/* Ring buffer helpers */
static inline uint8_t ring_count(uint8_t head, uint8_t tail)
{
    return (head - tail) & (UART_BUF_SIZE - 1);
}

static inline int ring_full(uint8_t head, uint8_t tail)
{
    return ring_count(head, tail) == (UART_BUF_SIZE - 1);
}

static inline int ring_empty(uint8_t head, uint8_t tail)
{
    return head == tail;
}

/* Update status register flags based on buffer state */
static void uart_update_status(avr_cpu_t *cpu, avr_uart_t *uart)
{
    const avr_uart_config_t *cfg = uart->config;
    uint8_t status = cpu->data[cfg->ucsra_io + 0x20];

    /* UDRE: set if TX buffer has space */
    if (!ring_full(uart->tx_head, uart->tx_tail))
        status |= UCSR_UDRE;
    else
        status &= ~UCSR_UDRE;

    /* RXC: set if RX buffer has data */
    if (!ring_empty(uart->rx_head, uart->rx_tail))
        status |= UCSR_RXC;
    else
        status &= ~UCSR_RXC;

    cpu->data[cfg->ucsra_io + 0x20] = status;

    /* Interrupt handling */
    uint8_t ucsrb = cpu->data[cfg->ucsrb_io + 0x20];

    if ((status & UCSR_UDRE) && (ucsrb & UCSRB_UDRIE))
        cpu->irq_pending |= (1u << cfg->udre_vec);

    if ((status & UCSR_RXC) && (ucsrb & UCSRB_RXCIE))
        cpu->irq_pending |= (1u << cfg->rxc_vec);
}

/* ---------- I/O handlers ---------- */

static uint8_t udr_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    avr_uart_t *uart = ctx;
    (void)io_addr;

    /* Read from RX buffer */
    if (ring_empty(uart->rx_head, uart->rx_tail))
        return 0;

    uint8_t byte = uart->rx_buf[uart->rx_tail];
    uart->rx_tail = (uart->rx_tail + 1) & (UART_BUF_SIZE - 1);

    uart_update_status(cpu, uart);
    return byte;
}

static void udr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_uart_t *uart = ctx;
    (void)io_addr;

    uint8_t ucsrb = cpu->data[uart->config->ucsrb_io + 0x20];
    if (!(ucsrb & UCSRB_TXEN))
        return; /* TX not enabled */

    /* Write to TX buffer */
    if (!ring_full(uart->tx_head, uart->tx_tail)) {
        uart->tx_buf[uart->tx_head] = val;
        uart->tx_head = (uart->tx_head + 1) & (UART_BUF_SIZE - 1);
    }

    /* Set TXC flag */
    cpu->data[uart->config->ucsra_io + 0x20] |= UCSR_TXC;

    /* Trigger bridge callback */
    if (cpu->bridge_cb)
        cpu->bridge_cb(cpu->bridge_ctx, IO_BRIDGE_UART, 0, val);

    uart_update_status(cpu, uart);
}

static uint8_t ucsra_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    avr_uart_t *uart = ctx;
    uart_update_status(cpu, uart);
    return cpu->data[io_addr + 0x20];
}

static void ucsra_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)ctx;
    /* Writing 1 to TXC clears it */
    if (val & UCSR_TXC)
        cpu->data[io_addr + 0x20] &= ~UCSR_TXC;
    /* U2X and MPCM are writable */
    uint8_t mask = UCSR_U2X | UCSR_MPCM;
    cpu->data[io_addr + 0x20] = (cpu->data[io_addr + 0x20] & ~mask) | (val & mask);
}

static uint8_t generic_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

static void ucsrb_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_uart_t *uart = ctx;
    (void)io_addr; (void)val;
    uart_update_status(cpu, uart);
}

/* ---------- Init ---------- */

avr_uart_t *avr_uart_init(avr_cpu_t *cpu, const avr_uart_config_t *config)
{
    avr_uart_t *uart = calloc(1, sizeof(*uart));
    if (!uart) return NULL;
    uart->config = config;

    /* UDR: read → RX buffer, write → TX buffer */
    avr_io_register(cpu, config->udr_io, udr_read, udr_write, uart);

    /* UCSR0A: status register with special write behavior */
    avr_io_register(cpu, config->ucsra_io, ucsra_read, ucsra_write, uart);

    /* UCSR0B: control register */
    avr_io_register(cpu, config->ucsrb_io, generic_read, ucsrb_write, uart);

    /* UCSR0C, UBRRL, UBRRH: simple read/write */
    avr_io_register(cpu, config->ucsrc_io, generic_read, NULL, NULL);
    avr_io_register(cpu, config->ubrrl_io, generic_read, NULL, NULL);
    avr_io_register(cpu, config->ubrrh_io, generic_read, NULL, NULL);

    /* Initialize UDRE flag (transmitter starts empty) */
    cpu->data[config->ucsra_io + 0x20] = UCSR_UDRE;

    return uart;
}

/* ---------- External interface ---------- */

void avr_uart_rx_push(avr_cpu_t *cpu, avr_uart_t *uart, uint8_t byte)
{
    if (!uart) return;
    uint8_t ucsrb = cpu->data[uart->config->ucsrb_io + 0x20];
    if (!(ucsrb & UCSRB_RXEN))
        return; /* RX not enabled */

    if (!ring_full(uart->rx_head, uart->rx_tail)) {
        uart->rx_buf[uart->rx_head] = byte;
        uart->rx_head = (uart->rx_head + 1) & (UART_BUF_SIZE - 1);
    }
    uart_update_status(cpu, uart);
}

int avr_uart_tx_pop(avr_uart_t *uart)
{
    if (!uart || ring_empty(uart->tx_head, uart->tx_tail))
        return -1;
    uint8_t byte = uart->tx_buf[uart->tx_tail];
    uart->tx_tail = (uart->tx_tail + 1) & (UART_BUF_SIZE - 1);
    return byte;
}
