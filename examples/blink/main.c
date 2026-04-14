/*
 * AVR Blink - ATMega328P
 * Toggles PB5 (Arduino pin 13) with a delay loop.
 */
#include <avr/io.h>

static void delay(void)
{
    for (volatile uint32_t i = 0; i < 50000; i++)
        ;
}

int main(void)
{
    /* Set PB5 as output */
    DDRB |= (1 << PB5);

    while (1) {
        PORTB ^= (1 << PB5);
        delay();
    }

    return 0;
}
