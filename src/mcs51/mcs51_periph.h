/*
 * ucvm - MCS-51 peripheral emulation: Timer0/1, UART, GPIO, Interrupt controller
 */
#ifndef MCS51_PERIPH_H
#define MCS51_PERIPH_H

#include <stdint.h>

typedef struct mcs51_cpu mcs51_cpu_t;

/* ========== Timer 0/1 ========== */

typedef struct {
    /* Internal prescaler state (not needed for standard 8051 where timer ticks every machine cycle) */
    uint8_t  t1_baud_mode; /* 1 if Timer1 is in mode 2 generating baud rate */
} mcs51_timer_t;

mcs51_timer_t *mcs51_timer_init(mcs51_cpu_t *cpu);

/* Tick Timer0 and Timer1 by one machine cycle */
void mcs51_timer_tick(mcs51_cpu_t *cpu, mcs51_timer_t *timer);

/* ========== UART ========== */

#define MCS51_UART_BUF_SIZE 32

typedef struct {
    /* TX ring buffer */
    uint8_t tx_buf[MCS51_UART_BUF_SIZE];
    uint8_t tx_head, tx_tail;
    /* RX ring buffer */
    uint8_t rx_buf[MCS51_UART_BUF_SIZE];
    uint8_t rx_head, rx_tail;
    uint8_t tx_active; /* TX in progress */
} mcs51_uart_t;

mcs51_uart_t *mcs51_uart_init(mcs51_cpu_t *cpu);
void mcs51_uart_rx_push(mcs51_cpu_t *cpu, mcs51_uart_t *uart, uint8_t byte);
int  mcs51_uart_tx_pop(mcs51_uart_t *uart);

#endif /* MCS51_PERIPH_H */
