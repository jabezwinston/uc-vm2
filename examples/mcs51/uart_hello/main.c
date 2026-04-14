/*
 * 8051 UART Hello - AT89S52
 * Configures UART in mode 1 (8-bit, 9600 baud with 11.0592 MHz crystal),
 * sends "Hello from 8051!" via serial port.
 */
#include <8052.h>

void uart_init(void)
{
    SCON = 0x50;  /* Mode 1, 8-bit UART, REN enabled */
    TMOD |= 0x20; /* Timer1 mode 2 (8-bit auto-reload) */
    TH1 = 0xFD;   /* 9600 baud with 11.0592 MHz crystal */
    TL1 = 0xFD;
    TR1 = 1;       /* Start Timer1 */
}

void uart_putchar(char c)
{
    SBUF = c;
    while (!TI)
        ;
    TI = 0;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putchar(*s++);
}

void main(void)
{
    uart_init();
    uart_puts("Hello from 8051!\r\n");

    /* Echo loop */
    while (1) {
        if (RI) {
            char c = SBUF;
            RI = 0;
            uart_putchar(c);
        }
    }
}
