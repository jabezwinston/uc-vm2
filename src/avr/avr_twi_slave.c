/*
 * ucvm - Virtual I2C slave device for PC testing
 *
 * Emulates a simple register-map I2C slave (like an EEPROM or sensor):
 *   - 7-bit address configurable
 *   - 256-byte register map
 *   - Master write: first byte = register address, subsequent bytes = data
 *   - Master read: reads sequential bytes from current register address
 *
 * This implements the avr_twi_bus_t interface so it can be attached
 * to the TWI peripheral via avr_twi_set_bus().
 */
#include "avr_periph.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t  addr;          /* 7-bit slave address */
    uint8_t  regs[256];     /* Register map */
    uint8_t  reg_ptr;       /* Current register pointer */
    uint8_t  addressed;     /* 1 if currently addressed */
    uint8_t  first_byte;    /* 1 if next write is register address */
    int      verbose;       /* Log transactions to stderr */
} virt_i2c_slave_t;

static int virt_start(void *ctx)
{
    virt_i2c_slave_t *s = ctx;
    s->addressed = 0;
    s->first_byte = 1;
    return 0;
}

static int virt_write(void *ctx, uint8_t byte)
{
    virt_i2c_slave_t *s = ctx;

    if (!s->addressed) {
        /* Address byte: check if it matches our address */
        uint8_t addr = byte >> 1;
        uint8_t rw = byte & 1;
        if (addr == s->addr) {
            s->addressed = 1;
            s->first_byte = (rw == 0) ? 1 : 0; /* write mode → next byte is reg addr */
            if (s->verbose)
                fprintf(stderr, "[I2C] Slave 0x%02X addressed (%s)\n",
                        addr, rw ? "READ" : "WRITE");
            return 1; /* ACK */
        }
        return 0; /* NACK — not our address */
    }

    /* Data write */
    if (s->first_byte) {
        s->reg_ptr = byte;
        s->first_byte = 0;
        if (s->verbose)
            fprintf(stderr, "[I2C] Reg addr = 0x%02X\n", byte);
    } else {
        if (s->verbose)
            fprintf(stderr, "[I2C] Write reg[0x%02X] = 0x%02X\n",
                    s->reg_ptr, byte);
        s->regs[s->reg_ptr] = byte;
        s->reg_ptr++;
    }
    return 1; /* ACK */
}

static int virt_read(void *ctx, int ack)
{
    virt_i2c_slave_t *s = ctx;
    (void)ack;

    if (!s->addressed)
        return 0xFF;

    uint8_t val = s->regs[s->reg_ptr];
    if (s->verbose)
        fprintf(stderr, "[I2C] Read reg[0x%02X] = 0x%02X\n",
                s->reg_ptr, val);
    s->reg_ptr++;
    return val;
}

static void virt_stop(void *ctx)
{
    virt_i2c_slave_t *s = ctx;
    s->addressed = 0;
    if (s->verbose)
        fprintf(stderr, "[I2C] STOP\n");
}

/* Public API */

static virt_i2c_slave_t g_slave;
static avr_twi_bus_t g_bus;

avr_twi_bus_t *avr_twi_create_virtual_slave(uint8_t addr, int verbose)
{
    memset(&g_slave, 0, sizeof(g_slave));
    g_slave.addr = addr;
    g_slave.verbose = verbose;

    /* Pre-fill some test data so reads return something useful */
    for (int i = 0; i < 256; i++)
        g_slave.regs[i] = (uint8_t)i;

    g_bus.start      = virt_start;
    g_bus.write_byte = virt_write;
    g_bus.read_byte  = virt_read;
    g_bus.stop       = virt_stop;
    g_bus.ctx        = &g_slave;

    return &g_bus;
}

/* Get a pointer to the slave's register map (for test verification) */
uint8_t *avr_twi_virtual_slave_regs(void)
{
    return g_slave.regs;
}
