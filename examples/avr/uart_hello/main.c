/*
 * AVR UART Hello - ATMega328P
 * Prints "Hello 1" .. "Hello 10" on USART0 at 9600 baud.
 */
#include <avr/io.h>
#include <util/delay.h>

#define F_CPU   16000000UL
#define BAUD    9600
#define UBRR_VAL ((F_CPU / 16 / BAUD) - 1)

static void uart_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL);
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
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

static void uart_putint(uint8_t n)
{
    if (n >= 10)
        uart_putchar('0' + n / 10);
    uart_putchar('0' + n % 10);
}

int main(void)
{
    uart_init();

    for (uint8_t i = 1; i <= 10; i++) {
        uart_puts("Hello ");
        uart_putint(i);
        uart_puts("\r\n");
        _delay_ms(1);
    }

    /* Echo loop */
    while (1) {
        if (UCSR0A & (1 << RXC0)) {
            char c = UDR0;
            uart_putchar(c);
        }
    }

    return 0;
}
