/*
 * ucvm - AVR TWI (Two-Wire Interface / I2C) peripheral emulation
 *
 * Cycle-accurate emulation of the ATMega328P TWI hardware:
 *   - Full master transmitter/receiver mode
 *   - All TWSR status codes
 *   - TWINT flag management with cycle-counted delays
 *   - I2C bus abstraction for external device communication
 *
 * Register map (data space → I/O index = data_addr - 0x20):
 *   TWBR  0xB8 → 0x98   Bit rate
 *   TWSR  0xB9 → 0x99   Status + prescaler
 *   TWAR  0xBA → 0x9A   Slave address
 *   TWDR  0xBB → 0x9B   Data
 *   TWCR  0xBC → 0x9C   Control
 *   TWAMR 0xBD → 0x9D   Address mask
 *
 * Timing: each SCL period = 16 + 2*TWBR*PrescalerValue CPU cycles
 *   START:   1 SCL period
 *   Address: 9 SCL periods (8 bits + ACK)
 *   Data:    9 SCL periods
 *   STOP:    1 SCL period (does NOT set TWINT)
 */
#include "avr_cpu.h"
#include "avr_periph.h"
#include <stdlib.h>
#include <string.h>

/* ---------- TWCR bit positions ---------- */
#define TWINT  7
#define TWEA   6
#define TWSTA  5
#define TWSTO  4
#define TWWC   3
#define TWEN   2
/* bit 1 reserved */
#define TWIE   0

/* ---------- TWI status codes (TWSR & 0xF8) ---------- */

/* Master Transmitter */
#define TW_START           0x08
#define TW_REP_START       0x10
#define TW_MT_SLA_ACK      0x18
#define TW_MT_SLA_NACK     0x20
#define TW_MT_DATA_ACK     0x28
#define TW_MT_DATA_NACK    0x30
#define TW_MT_ARB_LOST     0x38

/* Master Receiver */
#define TW_MR_SLA_ACK      0x40
#define TW_MR_SLA_NACK     0x48
#define TW_MR_DATA_ACK     0x50
#define TW_MR_DATA_NACK    0x58

/* Misc */
#define TW_NO_INFO         0xF8
#define TW_BUS_ERROR       0x00

/* ---------- Prescaler lookup ---------- */

static const uint8_t prescaler_values[] = { 1, 4, 16, 64 };

/* Get the SCL period in CPU cycles */
static uint32_t scl_period(avr_cpu_t *cpu, const avr_twi_config_t *cfg)
{
    uint8_t twbr = cpu->data[cfg->twbr_io + 0x20];
    uint8_t twsr = cpu->data[cfg->twsr_io + 0x20];
    uint8_t ps = prescaler_values[twsr & 0x03];
    uint32_t period = 16 + 2 * (uint32_t)twbr * ps;
    if (period < 16) period = 16; /* sanity */
    return period;
}

/* ---------- Register read/write handlers ---------- */

static uint8_t twcr_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

static uint8_t twsr_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    avr_twi_t *twi = ctx;
    /* Status in bits 7:3, prescaler in bits 1:0, bit 2 always 0 */
    uint8_t ps_bits = cpu->data[io_addr + 0x20] & 0x03;
    return (twi->status & 0xF8) | ps_bits;
}

static uint8_t twdr_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)ctx;
    return cpu->data[io_addr + 0x20];
}

static void twbr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)ctx;
    cpu->data[io_addr + 0x20] = val;
}

static void twsr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)ctx;
    /* Only prescaler bits (1:0) are writable; status bits are read-only */
    cpu->data[io_addr + 0x20] = (cpu->data[io_addr + 0x20] & 0xFC) | (val & 0x03);
}

static void twar_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)ctx;
    cpu->data[io_addr + 0x20] = val;
}

static void twdr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_twi_t *twi = ctx;
    uint8_t twcr = cpu->data[twi->config->twcr_io + 0x20];

    /* Writing TWDR when TWINT is low sets TWWC (write collision) */
    if (!(twcr & (1 << TWINT))) {
        cpu->data[twi->config->twcr_io + 0x20] |= (1 << TWWC);
    } else {
        /* Clear TWWC on valid write */
        cpu->data[twi->config->twcr_io + 0x20] &= ~(1 << TWWC);
    }
    cpu->data[io_addr + 0x20] = val;
}

static void twamr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)ctx;
    cpu->data[io_addr + 0x20] = val & 0xFE; /* bit 0 reserved */
}

/* ---------- Set TWINT and optionally trigger interrupt ---------- */

static void twi_set_twint(avr_cpu_t *cpu, avr_twi_t *twi)
{
    uint8_t twcr_addr = twi->config->twcr_io;
    cpu->data[twcr_addr + 0x20] |= (1 << TWINT);

    /* If TWIE is set, queue interrupt */
    if (cpu->data[twcr_addr + 0x20] & (1 << TWIE)) {
        cpu->irq_pending |= (1u << twi->config->twi_vec);
    }
}

/* ---------- Complete a pending TWI operation ---------- */

static void twi_complete_op(avr_cpu_t *cpu, avr_twi_t *twi)
{
    uint8_t op = twi->pending_op;
    twi->pending_op = TWI_OP_NONE;
    twi->cycles_remaining = 0;

    switch (op) {
    case TWI_OP_START:
        /* START/repeated START condition sent */
        if (twi->bus && twi->bus->start)
            twi->bus->start(twi->bus->ctx);

        if (twi->bus_state == TWI_IDLE)
            twi->status = TW_START;      /* 0x08 */
        else
            twi->status = TW_REP_START;  /* 0x10 */

        twi->bus_state = TWI_START_SENT;
        twi_set_twint(cpu, twi);
        break;

    case TWI_OP_SLA_W: {
        /* SLA+W transmitted */
        int ack = 0;
        if (twi->bus && twi->bus->write_byte)
            ack = twi->bus->write_byte(twi->bus->ctx, twi->pending_data);

        twi->status = ack ? TW_MT_SLA_ACK : TW_MT_SLA_NACK;  /* 0x18 / 0x20 */
        twi->bus_state = TWI_MT_ADDR_SENT;
        twi->slave_rw = 0;
        twi_set_twint(cpu, twi);
        break;
    }

    case TWI_OP_SLA_R: {
        /* SLA+R transmitted */
        int ack = 0;
        if (twi->bus && twi->bus->write_byte)
            ack = twi->bus->write_byte(twi->bus->ctx, twi->pending_data);

        twi->status = ack ? TW_MR_SLA_ACK : TW_MR_SLA_NACK;  /* 0x40 / 0x48 */
        twi->bus_state = TWI_MR_ADDR_SENT;
        twi->slave_rw = 1;
        twi_set_twint(cpu, twi);
        break;
    }

    case TWI_OP_DATA_TX: {
        /* Data byte transmitted in master TX mode */
        int ack = 0;
        if (twi->bus && twi->bus->write_byte)
            ack = twi->bus->write_byte(twi->bus->ctx, twi->pending_data);

        twi->status = ack ? TW_MT_DATA_ACK : TW_MT_DATA_NACK;  /* 0x28 / 0x30 */
        twi->bus_state = TWI_MT_DATA_SENT;
        twi_set_twint(cpu, twi);
        break;
    }

    case TWI_OP_DATA_RX: {
        /* Data byte received in master RX mode */
        int byte = 0xFF;
        if (twi->bus && twi->bus->read_byte)
            byte = twi->bus->read_byte(twi->bus->ctx, twi->pending_ack);
        if (byte < 0) byte = 0xFF;

        /* Store received byte in TWDR */
        cpu->data[twi->config->twdr_io + 0x20] = (uint8_t)byte;

        twi->status = twi->pending_ack ? TW_MR_DATA_ACK : TW_MR_DATA_NACK;  /* 0x50 / 0x58 */
        twi->bus_state = TWI_MR_DATA_RECEIVED;
        twi_set_twint(cpu, twi);
        break;
    }

    case TWI_OP_STOP:
        /* STOP condition — does NOT set TWINT */
        if (twi->bus && twi->bus->stop)
            twi->bus->stop(twi->bus->ctx);

        twi->bus_state = TWI_IDLE;
        twi->status = TW_NO_INFO;  /* 0xF8 */

        /* Auto-clear TWSTO bit */
        cpu->data[twi->config->twcr_io + 0x20] &= ~(1 << TWSTO);
        break;
    }
}

/* ---------- TWCR write — the main action trigger ---------- */

static void twcr_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    avr_twi_t *twi = ctx;
    uint8_t old = cpu->data[io_addr + 0x20];

    /* TWEN check — if TWI disabled, just store and return */
    if (!(val & (1 << TWEN))) {
        cpu->data[io_addr + 0x20] = val & ~((1 << TWINT) | (1 << TWSTO));
        twi->pending_op = TWI_OP_NONE;
        twi->cycles_remaining = 0;
        twi->bus_state = TWI_IDLE;
        twi->status = TW_NO_INFO;
        return;
    }

    /* Writing 1 to TWINT clears it and triggers the next operation.
     * This is the firmware's "go" signal — it works regardless of
     * whether TWINT was already set (first START) or was set by
     * hardware (subsequent operations). */
    int clearing_twint = (val & (1 << TWINT)) != 0;

    /* Build the new TWCR value:
     * - TWINT: cleared if writing 1 to it
     * - TWWC: read-only (preserve from old)
     * - TWSTO: auto-cleared by hardware on completion, writable here
     * - Others: writable */
    uint8_t new_twcr = val & ~(1 << TWWC);  /* TWWC not writable */
    new_twcr = (new_twcr & ~(1 << TWWC)) | (old & (1 << TWWC));

    if (clearing_twint)
        new_twcr &= ~(1 << TWINT);  /* Clear TWINT */

    cpu->data[io_addr + 0x20] = new_twcr;

    if (!clearing_twint)
        return;

    /* --- TWINT was cleared: initiate the next TWI operation --- */

    uint32_t scl = scl_period(cpu, twi->config);

    /* STOP requested? */
    if (val & (1 << TWSTO)) {
        twi->pending_op = TWI_OP_STOP;
        twi->cycles_remaining = scl;  /* 1 SCL period */
        /* If also START requested (repeated), STOP then START */
        if (val & (1 << TWSTA)) {
            /* Do STOP immediately, then queue START */
            twi_complete_op(cpu, twi);
            twi->pending_op = TWI_OP_START;
            twi->cycles_remaining = scl;
        }
        return;
    }

    /* START requested? */
    if (val & (1 << TWSTA)) {
        twi->pending_op = TWI_OP_START;
        twi->cycles_remaining = scl;  /* 1 SCL period */
        return;
    }

    /* After START: firmware loads SLA+R/W into TWDR and clears TWINT */
    if (twi->bus_state == TWI_START_SENT) {
        uint8_t sla = cpu->data[twi->config->twdr_io + 0x20];
        twi->pending_data = sla;
        if (sla & 0x01) {
            twi->pending_op = TWI_OP_SLA_R;
        } else {
            twi->pending_op = TWI_OP_SLA_W;
        }
        twi->cycles_remaining = 9 * scl;  /* 9 SCL periods */
        return;
    }

    /* In master TX mode: send data byte from TWDR */
    if (twi->bus_state == TWI_MT_ADDR_SENT ||
        twi->bus_state == TWI_MT_DATA_SENT) {
        twi->pending_data = cpu->data[twi->config->twdr_io + 0x20];
        twi->pending_op = TWI_OP_DATA_TX;
        twi->cycles_remaining = 9 * scl;
        return;
    }

    /* In master RX mode: receive data byte */
    if (twi->bus_state == TWI_MR_ADDR_SENT ||
        twi->bus_state == TWI_MR_DATA_RECEIVED) {
        twi->pending_ack = (val & (1 << TWEA)) ? 1 : 0;
        twi->pending_op = TWI_OP_DATA_RX;
        twi->cycles_remaining = 9 * scl;
        return;
    }
}

/* ---------- Public API ---------- */

avr_twi_t *avr_twi_init(avr_cpu_t *cpu, const avr_twi_config_t *config)
{
    avr_twi_t *twi = calloc(1, sizeof(*twi));
    if (!twi) return NULL;

    twi->config = config;
    twi->status = TW_NO_INFO;  /* 0xF8 */
    twi->bus_state = TWI_IDLE;

    /* Register I/O handlers */
    avr_io_register(cpu, config->twbr_io,  NULL,      twbr_write, twi);
    avr_io_register(cpu, config->twsr_io,  twsr_read, twsr_write, twi);
    avr_io_register(cpu, config->twar_io,  NULL,      twar_write, twi);
    avr_io_register(cpu, config->twdr_io,  twdr_read, twdr_write, twi);
    avr_io_register(cpu, config->twcr_io,  twcr_read, twcr_write, twi);
    avr_io_register(cpu, config->twamr_io, NULL,      twamr_write, twi);

    /* Initial register values */
    cpu->data[config->twsr_io + 0x20] = TW_NO_INFO;  /* TWSR = 0xF8 */

    cpu->periph_twi = twi;
    return twi;
}

void avr_twi_tick(avr_cpu_t *cpu, avr_twi_t *twi, uint16_t cycles)
{
    if (twi->pending_op == TWI_OP_NONE || twi->cycles_remaining == 0)
        return;

    if (twi->cycles_remaining <= (uint32_t)cycles) {
        twi_complete_op(cpu, twi);
    } else {
        twi->cycles_remaining -= cycles;
    }
}

void avr_twi_set_bus(avr_twi_t *twi, avr_twi_bus_t *bus)
{
    twi->bus = bus;
}
