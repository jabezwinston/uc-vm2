/*
 * ucvm - Microcontroller Virtual Machine
 * AVR CPU: init, memory access, step wrapper, interrupt dispatch
 */
#include "avr_cpu.h"
#include "avr_periph.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- Special I/O addresses ---------- */
#define IO_SREG  0x3F  /* I/O addr of SREG */
#define IO_SPL   0x3D  /* I/O addr of SPL */
#define IO_SPH   0x3E  /* I/O addr of SPH */
#define IO_MCUCR 0x35  /* MCU control register (sleep modes) */

/* Data space offset for I/O registers */
#define IO_BASE  0x20

/* ---------- Internal I/O handlers for SREG/SP ---------- */

static uint8_t sreg_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)io_addr; (void)ctx;
    return cpu->sreg;
}

static void sreg_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)io_addr; (void)ctx;
    cpu->sreg = val;
    cpu->data[IO_SREG + IO_BASE] = val;
}

static uint8_t spl_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)io_addr; (void)ctx;
    return cpu->sp & 0xFF;
}

static void spl_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)io_addr; (void)ctx;
    cpu->sp = (cpu->sp & 0xFF00) | val;
    cpu->data[IO_SPL + IO_BASE] = val;
}

static uint8_t sph_read(avr_cpu_t *cpu, uint8_t io_addr, void *ctx)
{
    (void)io_addr; (void)ctx;
    return (cpu->sp >> 8) & 0xFF;
}

static void sph_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx)
{
    (void)io_addr; (void)ctx;
    cpu->sp = (cpu->sp & 0x00FF) | ((uint16_t)val << 8);
    cpu->data[IO_SPH + IO_BASE] = val;
}

/* ---------- Init / Free / Reset ---------- */

avr_cpu_t *avr_cpu_init(const avr_variant_t *variant,
                        const uint16_t *flash, uint32_t flash_size)
{
    size_t alloc_size = sizeof(avr_cpu_t) + variant->data_size;
    avr_cpu_t *cpu = calloc(1, alloc_size);
    if (!cpu)
        return NULL;

    cpu->variant    = variant;
    cpu->flash      = flash;
    cpu->flash_size = flash_size;

    /* Register internal I/O handlers for SREG and SP */
    avr_io_register(cpu, IO_SREG, sreg_read, sreg_write, NULL);
    avr_io_register(cpu, IO_SPL,  spl_read,  spl_write,  NULL);
    avr_io_register(cpu, IO_SPH,  sph_read,  sph_write,  NULL);

    /* Initialize peripherals (variant-specific) */
    if (variant->periph_init)
        variant->periph_init(cpu);

    /* Reset to initial state */
    avr_cpu_reset(cpu);

    return cpu;
}

void avr_cpu_free(avr_cpu_t *cpu)
{
    if (!cpu)
        return;
    free(cpu->periph_timer);
    free(cpu->periph_gpio);
    free(cpu->periph_uart);
    free(cpu);
}

void avr_cpu_reset(avr_cpu_t *cpu)
{
    cpu->pc    = 0;
    cpu->sreg  = 0;
    /* SP initializes to top of SRAM */
    cpu->sp    = cpu->variant->data_size - 1;
    cpu->cycles     = 0;
    cpu->state      = AVR_STATE_RUNNING;
    cpu->skip_next  = 0;
    cpu->irq_pending = 0;

    /* Clear data memory (registers + I/O + SRAM) */
    memset(cpu->data, 0, cpu->variant->data_size);

    /* Sync SP to data space */
    cpu->data[IO_SPL + IO_BASE] = cpu->sp & 0xFF;
    cpu->data[IO_SPH + IO_BASE] = (cpu->sp >> 8) & 0xFF;
}

/* ---------- I/O handler registration ---------- */

void avr_io_register(avr_cpu_t *cpu, uint8_t io_addr,
                     io_read_fn read, io_write_fn write, void *ctx)
{
    if (io_addr < AVR_IO_MAX) {
        cpu->io_read[io_addr]  = read;
        cpu->io_write[io_addr] = write;
        cpu->io_ctx[io_addr]   = ctx;
    }
}

/* ---------- Memory access ---------- */

AVR_HOT uint8_t avr_io_read(avr_cpu_t *cpu, uint8_t io_addr)
{
    if (io_addr < AVR_IO_MAX && cpu->io_read[io_addr])
        return cpu->io_read[io_addr](cpu, io_addr, cpu->io_ctx[io_addr]);
    /* No handler: read directly from data space */
    return cpu->data[io_addr + IO_BASE];
}

AVR_HOT void avr_io_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val)
{
    /* Always store to data space */
    cpu->data[io_addr + IO_BASE] = val;
    if (io_addr < AVR_IO_MAX && cpu->io_write[io_addr])
        cpu->io_write[io_addr](cpu, io_addr, val, cpu->io_ctx[io_addr]);
}

AVR_HOT uint8_t avr_data_read(avr_cpu_t *cpu, uint16_t addr)
{
    if (addr < 0x20) {
        /* Register file: direct access */
        return cpu->data[addr];
    }
    if (addr < (uint16_t)(0x20 + AVR_IO_MAX) && addr < cpu->variant->sram_start) {
        /* I/O register space (standard + extended) */
        uint8_t io_addr = addr - IO_BASE;
        if (cpu->io_read[io_addr])
            return cpu->io_read[io_addr](cpu, io_addr, cpu->io_ctx[io_addr]);
        return cpu->data[addr];
    }
    /* SRAM (or unmapped) */
    if (addr < cpu->variant->data_size)
        return cpu->data[addr];
    return 0xFF; /* unmapped reads return 0xFF */
}

AVR_HOT void avr_data_write(avr_cpu_t *cpu, uint16_t addr, uint8_t val)
{
    if (addr < 0x20) {
        /* Register file: direct write */
        cpu->data[addr] = val;
        return;
    }
    if (addr < (uint16_t)(0x20 + AVR_IO_MAX) && addr < cpu->variant->sram_start) {
        /* I/O register space */
        uint8_t io_addr = addr - IO_BASE;
        cpu->data[addr] = val;
        if (cpu->io_write[io_addr])
            cpu->io_write[io_addr](cpu, io_addr, val, cpu->io_ctx[io_addr]);
        return;
    }
    /* SRAM */
    if (addr < cpu->variant->data_size)
        cpu->data[addr] = val;
    /* Writes to unmapped space are silently ignored */
}

uint8_t avr_flash_read_byte(avr_cpu_t *cpu, uint16_t byte_addr)
{
    if (byte_addr >= cpu->flash_size)
        return 0xFF;
    uint16_t word = cpu->flash[byte_addr >> 1];
    if (byte_addr & 1)
        return (word >> 8) & 0xFF;
    return word & 0xFF;
}

/* ---------- Stack operations ---------- */

AVR_HOT void avr_push(avr_cpu_t *cpu, uint8_t val)
{
    if (cpu->sp >= cpu->variant->sram_start)
        cpu->data[cpu->sp] = val;
    cpu->sp--;
    /* Sync SP to I/O registers */
    cpu->data[IO_SPL + IO_BASE] = cpu->sp & 0xFF;
    cpu->data[IO_SPH + IO_BASE] = (cpu->sp >> 8) & 0xFF;
}

AVR_HOT uint8_t avr_pop(avr_cpu_t *cpu)
{
    cpu->sp++;
    /* Sync SP to I/O registers */
    cpu->data[IO_SPL + IO_BASE] = cpu->sp & 0xFF;
    cpu->data[IO_SPH + IO_BASE] = (cpu->sp >> 8) & 0xFF;
    if (cpu->sp < cpu->variant->data_size)
        return cpu->data[cpu->sp];
    return 0xFF;
}

/* ---------- Interrupt dispatch ---------- */

void avr_cpu_check_irq(avr_cpu_t *cpu)
{
    /* Interrupts only fire if global interrupt enable (I flag) is set */
    if (!(cpu->sreg & SREG_I))
        return;
    if (!cpu->irq_pending)
        return;

    /* Find lowest-numbered pending interrupt (highest priority) */
    for (uint8_t vec = 1; vec < cpu->variant->num_vectors; vec++) {
        if (cpu->irq_pending & (1u << vec)) {
            /* Clear pending flag */
            cpu->irq_pending &= ~(1u << vec);

            /* Disable global interrupts */
            cpu->sreg &= ~SREG_I;
            cpu->data[IO_SREG + IO_BASE] = cpu->sreg;

            /* Push PC (return address) onto stack: PCL first, then PCH */
            avr_push(cpu, cpu->pc & 0xFF);
            avr_push(cpu, (cpu->pc >> 8) & 0xFF);

            /* Jump to vector address */
            cpu->pc = vec * cpu->variant->vector_size;
            cpu->cycles += 4; /* Interrupt response takes 4 cycles */

            /* Wake from sleep if sleeping */
            if (cpu->state == AVR_STATE_SLEEPING)
                cpu->state = AVR_STATE_RUNNING;

            return;
        }
    }
}

/* ---------- CPU step ---------- */

/* Peripheral tick interval — trades precision for speed.
 * 32 cycles at 16 MHz = 2 µs.  Timer prescaler minimums are
 * /1 (match every cycle) but real firmware rarely needs
 * sub-2µs timer accuracy. */
#define PERIPH_TICK_INTERVAL 32

AVR_HOT uint8_t avr_cpu_step(avr_cpu_t *cpu)
{
    if (cpu->state != AVR_STATE_RUNNING)
        return 0;

    uint8_t cycles = avr_decode_execute(cpu);
    cpu->cycles += cycles;

    /* Batch peripheral ticks — avoids 3 function calls per instruction */
    cpu->periph_accum += cycles;
    if (cpu->periph_accum >= PERIPH_TICK_INTERVAL) {
        uint16_t elapsed = cpu->periph_accum;
        cpu->periph_accum = 0;
        if (cpu->periph_timer)
            avr_timer0_tick(cpu, cpu->periph_timer, elapsed);
        if (cpu->periph_twi)
            avr_twi_tick(cpu, cpu->periph_twi, elapsed);
        /* Inline IRQ check — skip function call when nothing pending */
        if (cpu->irq_pending && (cpu->sreg & SREG_I))
            avr_cpu_check_irq(cpu);
    }

    return cycles;
}
