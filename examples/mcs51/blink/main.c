/*
 * 8051 Blink - AT89S52
 * Toggles all P1 pins with a delay loop.
 */
#include <8052.h>

void delay(void)
{
    volatile unsigned int i;
    for (i = 0; i < 5000; i++)
        ;
}

void main(void)
{
    P1 = 0x00;
    while (1) {
        P1 = ~P1;
        delay();
    }
}
