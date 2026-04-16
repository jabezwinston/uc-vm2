/*
 * AVR Blink - ATMega328P
 * Toggles PB5 (Arduino pin 13) every 500ms.
 */
#include <avr/io.h>
#include <util/delay.h>

int main(void)
{
    DDRB |= (1 << PB5);

    while (1) {
        PORTB ^= (1 << PB5);
        _delay_ms(500);
    }

    return 0;
}
