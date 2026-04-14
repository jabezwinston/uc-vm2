/*
 * 8051 Timer Interrupt - AT89S52
 * Timer0 interrupt toggles P1.0 approximately every 50ms.
 * Uses Timer0 mode 1 (16-bit) with reload in ISR.
 */
#include <8052.h>

volatile unsigned char toggle_count = 0;

void timer0_isr(void) __interrupt(1)
{
    /* Reload for ~50ms at 11.0592 MHz (12 clk/cycle):
     * 50ms = 50000 us, machine cycles = 50000/1.085 ≈ 46083 = 0xB3FF
     * Reload = 65536 - 46083 = 19453 = 0x4BFD */
    TH0 = 0x4B;
    TL0 = 0xFD;
    toggle_count++;
    P1_0 = !P1_0; /* Toggle P1.0 */
}

void main(void)
{
    /* Timer0 mode 1 (16-bit) */
    TMOD = 0x01;

    /* Initial load */
    TH0 = 0x4B;
    TL0 = 0xFD;

    /* Enable Timer0 interrupt */
    ET0 = 1;
    EA  = 1;

    /* Start Timer0 */
    TR0 = 1;

    /* Main loop — just wait */
    while (1) {
        P1 = (P1 & 0xFE) | (P1_0 & 0x01);
    }
}
