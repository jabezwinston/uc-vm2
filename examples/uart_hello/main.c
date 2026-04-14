/*
 * AVR UART Hello - ATMega328P
 * Configures USART0 at 9600 baud, prints "Hello from ucvm!"
 */
#include <avr/io.h>

#define F_CPU   16000000UL
#define BAUD    9600
#define UBRR_VAL ((F_CPU / 16 / BAUD) - 1)

static void uart_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL);
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); /* 8N1 */
}

static void uart_putchar(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putchar(*s++);
}

int main(void)
{
    uart_init();
    uart_puts("Hello from ucvm!\r\n");

    /* Echo loop */
    while (1) {
        if (UCSR0A & (1 << RXC0)) {
            char c = UDR0;
            uart_putchar(c);
        }
    }

    return 0;
}
