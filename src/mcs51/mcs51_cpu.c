/*
 * ucvm - MCS-51 CPU: init, reset, memory access, step, interrupt dispatch
 */
#include "mcs51_cpu.h"
#include "mcs51_periph.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- Init / Free / Reset ---------- */

mcs51_cpu_t *mcs51_cpu_init(const mcs51_variant_t *variant)
{
    mcs51_cpu_t *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return NULL;

    cpu->variant = variant;
    cpu->code_size = variant->code_size;
    cpu->xram_size = variant->xram_size;

    cpu->code = calloc(1, cpu->code_size);
    if (!cpu->code) { free(cpu); return NULL; }

    if (cpu->xram_size > 0) {
        cpu->xram = calloc(1, cpu->xram_size);
        if (!cpu->xram) { free(cpu->code); free(cpu); return NULL; }
    }

    /* Initialize peripherals */
    if (variant->periph_init)
        variant->periph_init(cpu);

    mcs51_cpu_reset(cpu);
    return cpu;
}

void mcs51_cpu_free(mcs51_cpu_t *cpu)
{
    if (!cpu) return;
    free(cpu->periph_timer);
    free(cpu->periph_uart);
    free(cpu->periph_gpio);
    free(cpu->periph_intc);
    free(cpu->xram);
    free(cpu->code);
    free(cpu);
}

void mcs51_cpu_reset(mcs51_cpu_t *cpu)
{
    cpu->pc = 0;
    cpu->cycles = 0;
    cpu->state = MCS51_STATE_RUNNING;

    memset(cpu->iram, 0, sizeof(cpu->iram));
    memset(cpu->sfr, 0, sizeof(cpu->sfr));

    /* Default SFR values after reset */
    cpu->sfr[SFI(SFR_SP)]  = 0x07;  /* SP starts at 0x07 */
    cpu->sfr[SFI(SFR_P0)]  = 0xFF;  /* Ports reset to 0xFF (all high/input) */
    cpu->sfr[SFI(SFR_P1)]  = 0xFF;
    cpu->sfr[SFI(SFR_P2)]  = 0xFF;
    cpu->sfr[SFI(SFR_P3)]  = 0xFF;

    /* Interrupt state */
    cpu->int_active[0] = 0;
    cpu->int_active[1] = 0;
    cpu->int_priority = -1;
}

/* ---------- SFR handler registration ---------- */

void mcs51_sfr_register(mcs51_cpu_t *cpu, uint8_t addr,
                         sfr_read_fn read, sfr_write_fn write, void *ctx)
{
    uint8_t idx = SFI(addr);
    cpu->sfr_read[idx]  = read;
    cpu->sfr_write[idx] = write;
    cpu->sfr_ctx[idx]   = ctx;
}

/* ---------- Memory access ---------- */

uint8_t mcs51_direct_read(mcs51_cpu_t *cpu, uint8_t addr)
{
    if (addr < 0x80) {
        return cpu->iram[addr];
    }
    /* SFR space */
    uint8_t idx = SFI(addr);
    if (cpu->sfr_read[idx])
        return cpu->sfr_read[idx](cpu, addr, cpu->sfr_ctx[idx]);
    return cpu->sfr[idx];
}

void mcs51_direct_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val)
{
    if (addr < 0x80) {
        cpu->iram[addr] = val;
        return;
    }
    /* SFR space */
    uint8_t idx = SFI(addr);
    cpu->sfr[idx] = val;
    if (cpu->sfr_write[idx])
        cpu->sfr_write[idx](cpu, addr, val, cpu->sfr_ctx[idx]);
}

uint8_t mcs51_indirect_read(mcs51_cpu_t *cpu, uint8_t addr)
{
    /* Indirect addressing accesses all 256 bytes of IRAM (including upper 128) */
    return cpu->iram[addr];
}

void mcs51_indirect_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val)
{
    cpu->iram[addr] = val;
}

uint8_t mcs51_bit_read(mcs51_cpu_t *cpu, uint8_t bitaddr)
{
    if (bitaddr < 0x80) {
        /* Bit-addressable IRAM: bytes 0x20-0x2F */
        uint8_t byte_addr = 0x20 + (bitaddr >> 3);
        uint8_t bit_pos   = bitaddr & 0x07;
        return (cpu->iram[byte_addr] >> bit_pos) & 1;
    }
    /* SFR bit: only SFRs at addresses divisible by 8 are bit-addressable */
    uint8_t sfr_addr = bitaddr & 0xF8;
    uint8_t bit_pos  = bitaddr & 0x07;
    return (mcs51_direct_read(cpu, sfr_addr) >> bit_pos) & 1;
}

void mcs51_bit_write(mcs51_cpu_t *cpu, uint8_t bitaddr, uint8_t val)
{
    if (bitaddr < 0x80) {
        uint8_t byte_addr = 0x20 + (bitaddr >> 3);
        uint8_t bit_pos   = bitaddr & 0x07;
        if (val)
            cpu->iram[byte_addr] |= (1 << bit_pos);
        else
            cpu->iram[byte_addr] &= ~(1 << bit_pos);
        return;
    }
    uint8_t sfr_addr = bitaddr & 0xF8;
    uint8_t bit_pos  = bitaddr & 0x07;
    uint8_t cur = mcs51_direct_read(cpu, sfr_addr);
    if (val)
        cur |= (1 << bit_pos);
    else
        cur &= ~(1 << bit_pos);
    mcs51_direct_write(cpu, sfr_addr, cur);
}

uint8_t mcs51_xdata_read(mcs51_cpu_t *cpu, uint16_t addr)
{
    if (addr < cpu->xram_size)
        return cpu->xram[addr];
    return 0xFF;
}

void mcs51_xdata_write(mcs51_cpu_t *cpu, uint16_t addr, uint8_t val)
{
    if (addr < cpu->xram_size)
        cpu->xram[addr] = val;
}

uint8_t mcs51_code_read(mcs51_cpu_t *cpu, uint16_t addr)
{
    if (addr < cpu->code_size)
        return cpu->code[addr];
    return 0xFF;
}

/* ---------- Stack operations (8051: push increments SP first, then stores) ---------- */

void mcs51_push(mcs51_cpu_t *cpu, uint8_t val)
{
    uint8_t sp = cpu->sfr[SFI(SFR_SP)] + 1;
    cpu->sfr[SFI(SFR_SP)] = sp;
    cpu->iram[sp] = val;
}

uint8_t mcs51_pop(mcs51_cpu_t *cpu)
{
    uint8_t sp = cpu->sfr[SFI(SFR_SP)];
    uint8_t val = cpu->iram[sp];
    cpu->sfr[SFI(SFR_SP)] = sp - 1;
    return val;
}

/* ---------- Interrupt dispatch ---------- */

/* Interrupt source table: priority order, vector address, IE bit, flag location */
static const struct {
    uint16_t vector;
    uint8_t  ie_bit;    /* bit in IE register */
    uint8_t  ip_bit;    /* bit in IP register */
    uint8_t  flag_sfr;  /* SFR containing the interrupt flag */
    uint8_t  flag_bit;  /* bit position in flag SFR */
    uint8_t  auto_clear;/* 1 if hardware auto-clears flag */
} int_sources[MCS51_NUM_INTERRUPTS] = {
    { INT_VEC_EX0,    IE_EX0, IP_PX0, SFR_TCON, 1, 1 }, /* IE0 in TCON.1 */
    { INT_VEC_T0,     IE_ET0, IP_PT0, SFR_TCON, 5, 1 }, /* TF0 in TCON.5 */
    { INT_VEC_EX1,    IE_EX1, IP_PX1, SFR_TCON, 3, 1 }, /* IE1 in TCON.3 */
    { INT_VEC_T1,     IE_ET1, IP_PT1, SFR_TCON, 7, 1 }, /* TF1 in TCON.7 */
    { INT_VEC_SERIAL, IE_ES,  IP_PS,  SFR_SCON, 0, 0 }, /* RI|TI — not auto-cleared */
    { INT_VEC_T2,     IE_ET2, IP_PT2, SFR_T2CON,7, 0 }, /* TF2|EXF2 — not auto-cleared */
};

void mcs51_cpu_check_irq(mcs51_cpu_t *cpu)
{
    uint8_t ie = cpu->sfr[SFI(SFR_IE)];
    if (!(ie & IE_EA))
        return; /* Global interrupts disabled */

    uint8_t ip = cpu->sfr[SFI(SFR_IP)];

    /* Check each source in priority order */
    for (int i = 0; i < MCS51_NUM_INTERRUPTS; i++) {
        /* Check if enabled */
        if (!(ie & int_sources[i].ie_bit))
            continue;

        /* Check if flag is set */
        uint8_t flag_sfr = cpu->sfr[SFI(int_sources[i].flag_sfr)];
        uint8_t flag_bit = int_sources[i].flag_bit;

        /* Special case: serial interrupt = RI | TI */
        uint8_t flag_set;
        if (i == 4) { /* Serial */
            flag_set = (flag_sfr & (SCON_RI | SCON_TI)) != 0;
        } else if (i == 5) { /* Timer 2 */
            flag_set = (flag_sfr & 0xC0) != 0; /* TF2 | EXF2 */
        } else {
            flag_set = (flag_sfr >> flag_bit) & 1;
        }

        if (!flag_set)
            continue;

        /* Determine priority level */
        uint8_t prio = (ip & int_sources[i].ip_bit) ? 1 : 0;

        /* Can only interrupt if higher priority than current */
        if (cpu->int_active[1] && prio <= 1)
            continue; /* High-priority ISR active, can't preempt */
        if (cpu->int_active[0] && prio == 0)
            continue; /* Low-priority ISR active, same level can't preempt */

        /* Accept interrupt */
        /* Auto-clear flag if applicable */
        if (int_sources[i].auto_clear) {
            cpu->sfr[SFI(int_sources[i].flag_sfr)] &= ~(1 << flag_bit);
        }

        /* Push PC */
        mcs51_push(cpu, cpu->pc & 0xFF);
        mcs51_push(cpu, (cpu->pc >> 8) & 0xFF);

        /* Jump to vector */
        cpu->pc = int_sources[i].vector;

        /* Mark priority level as active */
        cpu->int_active[prio] = 1;

        cpu->cycles += 2; /* Interrupt response takes 2 machine cycles */

        /* Wake from idle/sleep */
        if (cpu->state == MCS51_STATE_SLEEPING)
            cpu->state = MCS51_STATE_RUNNING;

        return; /* Only service one interrupt per check */
    }
}

/* ---------- CPU step ---------- */

uint8_t mcs51_cpu_step(mcs51_cpu_t *cpu)
{
    if (cpu->state != MCS51_STATE_RUNNING)
        return 0;

    uint16_t old_pc = cpu->pc;
    uint8_t cycles = mcs51_decode_execute(cpu);
    cpu->cycles += cycles;

    if (cpu->state == MCS51_STATE_HALTED) {
        fprintf(stderr, "MCS51 HALT: PC=0x%04X opcode=0x%02X cycles=%llu\n",
                old_pc, cpu->code[old_pc], (unsigned long long)cpu->cycles);
    }

    /* Update parity flag in PSW */
    uint8_t p = parity8(ACC(cpu));
    if (p)
        cpu->sfr[SFI(SFR_PSW)] |= PSW_P;
    else
        cpu->sfr[SFI(SFR_PSW)] &= ~PSW_P;

    /* Tick timer peripherals — one tick per machine cycle */
    if (cpu->periph_timer) {
        for (uint8_t c = 0; c < cycles; c++)
            mcs51_timer_tick(cpu, cpu->periph_timer);
    }

    /* Check interrupts */
    mcs51_cpu_check_irq(cpu);

    return cycles;
}
