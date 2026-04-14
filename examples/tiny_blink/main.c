/*
 * AVR Blink - ATtiny85
 * Toggles PB0 with a delay loop.
 */
#include <avr/io.h>

static void delay(void)
{
    for (volatile uint16_t i = 0; i < 10000; i++)
        ;
}

int main(void)
{
    /* Set PB0 as output */
    DDRB |= (1 << PB0);

    while (1) {
        PORTB ^= (1 << PB0);
        delay();
    }

    return 0;
}
