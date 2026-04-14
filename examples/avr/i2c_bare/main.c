/*
 * Bare-C TWI (I2C) master example for ATMega328P
 *
 * Talks to a register-map slave at address 0x50 (EEPROM-like):
 *   1. Write bytes 0xAA, 0xBB, 0xCC to register 0x10
 *   2. Read them back from register 0x10
 *   3. Print results via UART
 *
 * Compiled with avr-gcc, no Arduino libraries.
 */
#include <avr/io.h>
#include <util/delay.h>

#define SLAVE_ADDR 0x50
#define F_SCL      100000UL

/* ---------- UART ---------- */

static void uart_init(void)
{
    uint16_t ubrr = (F_CPU / 16 / 9600) - 1;
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void uart_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = c;
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

static void uart_hex(uint8_t v)
{
    const char h[] = "0123456789ABCDEF";
    uart_putc(h[(v >> 4) & 0xF]);
    uart_putc(h[v & 0xF]);
}

/* ---------- TWI ---------- */

static void twi_init(void)
{
    /* Set bit rate: SCL = F_CPU / (16 + 2*TWBR*prescaler) */
    TWSR = 0;  /* prescaler = 1 */
    TWBR = (uint8_t)((F_CPU / F_SCL - 16) / 2);
}

static uint8_t twi_start(void)
{
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
    return TWSR & 0xF8;
}

static void twi_stop(void)
{
    TWCR = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN);
    /* TWSTO auto-clears; no TWINT set */
    while (TWCR & (1 << TWSTO));  /* Wait for STOP to complete */
}

static uint8_t twi_write(uint8_t data)
{
    TWDR = data;
    TWCR = (1 << TWINT) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
    return TWSR & 0xF8;
}

static uint8_t twi_read_ack(void)
{
    TWCR = (1 << TWINT) | (1 << TWEA) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
    return TWDR;
}

static uint8_t twi_read_nack(void)
{
    TWCR = (1 << TWINT) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
    return TWDR;
}

/* ---------- Main ---------- */

int main(void)
{
    uart_init();
    twi_init();

    uart_puts("I2C bare-C test\r\n");

    /* --- Write 3 bytes to slave register 0x10 --- */
    uart_puts("Write 0xAA,0xBB,0xCC to reg 0x10...\r\n");

    uint8_t status = twi_start();
    uart_puts("  START: 0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write((SLAVE_ADDR << 1) | 0);  /* SLA+W */
    uart_puts("  SLA+W: 0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write(0x10);  /* Register address */
    uart_puts("  REG:   0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write(0xAA);
    uart_puts("  DATA1: 0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write(0xBB);
    uart_puts("  DATA2: 0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write(0xCC);
    uart_puts("  DATA3: 0x"); uart_hex(status); uart_puts("\r\n");

    twi_stop();
    uart_puts("  STOP\r\n");

    /* --- Read back 3 bytes from register 0x10 --- */
    uart_puts("Read from reg 0x10...\r\n");

    /* Set register pointer: write reg address */
    status = twi_start();
    status = twi_write((SLAVE_ADDR << 1) | 0);  /* SLA+W */
    status = twi_write(0x10);                     /* Register address */

    /* Repeated START for read */
    status = twi_start();
    uart_puts("  REP START: 0x"); uart_hex(status); uart_puts("\r\n");

    status = twi_write((SLAVE_ADDR << 1) | 1);  /* SLA+R */
    uart_puts("  SLA+R: 0x"); uart_hex(status); uart_puts("\r\n");

    uint8_t d1 = twi_read_ack();
    uart_puts("  READ1: 0x"); uart_hex(d1); uart_puts("\r\n");

    uint8_t d2 = twi_read_ack();
    uart_puts("  READ2: 0x"); uart_hex(d2); uart_puts("\r\n");

    uint8_t d3 = twi_read_nack();
    uart_puts("  READ3: 0x"); uart_hex(d3); uart_puts("\r\n");

    twi_stop();

    /* --- Verify --- */
    uart_puts("Verify: ");
    if (d1 == 0xAA && d2 == 0xBB && d3 == 0xCC)
        uart_puts("PASS\r\n");
    else
        uart_puts("FAIL\r\n");

    /* Done — halt */
    while (1);
    return 0;
}
