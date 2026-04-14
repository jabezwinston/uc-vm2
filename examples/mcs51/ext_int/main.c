/*
 * 8051 External Interrupt - AT89S52
 * INT0 (P3.2) triggers an interrupt that toggles P1.
 * INT0 configured for edge-triggered mode.
 */
#include <8052.h>

volatile unsigned char int_count = 0;

void ext_int0_isr(void) __interrupt(0)
{
    int_count++;
    P1 = int_count;
}

void main(void)
{
    P1 = 0x00;

    /* INT0 edge-triggered */
    IT0 = 1;

    /* Enable EX0 + global interrupts */
    EX0 = 1;
    EA  = 1;

    while (1) {
        /* Main loop does nothing — interrupt drives P1 */
    }
}
