/*
 * ucvm - Microcontroller Virtual Machine
 * AVR instruction decoder and executor
 *
 * Implements all ATMega328P / ATtiny85 instructions with cycle-accurate timing.
 * Decoded via hierarchical switch on opcode bits [15:12].
 */
#include "avr_cpu.h"
#include <string.h>
#include <stdio.h>

/* Stack operations (defined in avr_cpu.c) */
extern void avr_push(avr_cpu_t *cpu, uint8_t val);
extern uint8_t avr_pop(avr_cpu_t *cpu);

/* ---------- SREG flag helpers ---------- */

static inline void set_flag(avr_cpu_t *cpu, uint8_t flag, uint8_t val)
{
    if (val)
        cpu->sreg |= flag;
    else
        cpu->sreg &= ~flag;
}

/* Update flags for ADD/ADC: C, Z, N, V, S, H */
static inline void flags_add(avr_cpu_t *cpu, uint8_t Rd, uint8_t Rr, uint8_t R)
{
    uint8_t Rd3 = (Rd >> 3) & 1, Rr3 = (Rr >> 3) & 1, R3 = (R >> 3) & 1;
    uint8_t Rd7 = (Rd >> 7) & 1, Rr7 = (Rr >> 7) & 1, R7 = (R >> 7) & 1;

    set_flag(cpu, SREG_H, (Rd3 & Rr3) | (Rr3 & (~R3 & 1)) | ((~R3 & 1) & Rd3));
    set_flag(cpu, SREG_V, (Rd7 & Rr7 & (~R7 & 1)) | ((~Rd7 & 1) & (~Rr7 & 1) & R7));
    set_flag(cpu, SREG_N, R7);
    set_flag(cpu, SREG_Z, R == 0);
    set_flag(cpu, SREG_C, (Rd7 & Rr7) | (Rr7 & (~R7 & 1)) | ((~R7 & 1) & Rd7));
    set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1); /* N xor V */
}

/* Update flags for SUB/SBC/CP/CPC/NEG: C, Z, N, V, S, H */
static inline void flags_sub(avr_cpu_t *cpu, uint8_t Rd, uint8_t Rr, uint8_t R, int keep_z)
{
    uint8_t Rd3 = (Rd >> 3) & 1, Rr3 = (Rr >> 3) & 1, R3 = (R >> 3) & 1;
    uint8_t Rd7 = (Rd >> 7) & 1, Rr7 = (Rr >> 7) & 1, R7 = (R >> 7) & 1;

    set_flag(cpu, SREG_H, ((~Rd3 & 1) & Rr3) | (Rr3 & R3) | (R3 & (~Rd3 & 1)));
    set_flag(cpu, SREG_V, (Rd7 & (~Rr7 & 1) & (~R7 & 1)) | ((~Rd7 & 1) & Rr7 & R7));
    set_flag(cpu, SREG_N, R7);
    if (keep_z) {
        /* SBC/CPC: only clear Z, never set it */
        if (R != 0) cpu->sreg &= ~SREG_Z;
    } else {
        set_flag(cpu, SREG_Z, R == 0);
    }
    set_flag(cpu, SREG_C, ((~Rd7 & 1) & Rr7) | (Rr7 & R7) | (R7 & (~Rd7 & 1)));
    set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
}

/* Update flags for AND/ANDI/OR/ORI/EOR/COM: Z, N, V=0, S */
static inline void flags_logic(avr_cpu_t *cpu, uint8_t R)
{
    set_flag(cpu, SREG_V, 0);
    set_flag(cpu, SREG_N, (R >> 7) & 1);
    set_flag(cpu, SREG_Z, R == 0);
    set_flag(cpu, SREG_S, (R >> 7) & 1); /* S = N xor V = N since V=0 */
}

/* Update flags for INC: Z, N, V, S (no C affected) */
static inline void flags_inc(avr_cpu_t *cpu, uint8_t R)
{
    set_flag(cpu, SREG_V, R == 0x80);
    set_flag(cpu, SREG_N, (R >> 7) & 1);
    set_flag(cpu, SREG_Z, R == 0);
    set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
}

/* Update flags for DEC: Z, N, V, S (no C affected) */
static inline void flags_dec(avr_cpu_t *cpu, uint8_t R)
{
    set_flag(cpu, SREG_V, R == 0x7F);
    set_flag(cpu, SREG_N, (R >> 7) & 1);
    set_flag(cpu, SREG_Z, R == 0);
    set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
}

/* ---------- Operand extraction macros ---------- */

/* 5-bit Rd from bits 8:4 */
#define RD5(op)    (((op) >> 4) & 0x1F)
/* 5-bit Rr from bits 9,3:0 */
#define RR5(op)    ((((op) >> 5) & 0x10) | ((op) & 0x0F))
/* 4-bit Rd (upper regs r16-r31) from bits 7:4 */
#define RD4(op)    ((((op) >> 4) & 0x0F) + 16)
/* 8-bit immediate from bits 11:8,3:0 */
#define K8(op)     ((((op) >> 4) & 0xF0) | ((op) & 0x0F))
/* 6-bit immediate for ADIW/SBIW from bits 7:6,3:0 */
#define K6(op)     ((((op) >> 2) & 0x30) | ((op) & 0x0F))
/* 2-bit register pair selector for ADIW/SBIW: 0→R24, 1→R26, 2→R28, 3→R30 */
#define RDW2(op)   (24 + ((((op) >> 4) & 0x03) << 1))
/* 6-bit I/O address from bits 10:9,3:0 for IN/OUT */
#define IO6(op)    ((((op) >> 5) & 0x30) | ((op) & 0x0F))
/* 5-bit I/O address from bits 7:3 for CBI/SBI/SBIC/SBIS */
#define IO5(op)    (((op) >> 3) & 0x1F)
/* 3-bit bit number from bits 2:0 */
#define BIT3(op)   ((op) & 0x07)
/* 7-bit signed offset for branches from bits 9:3 */
#define BR7(op)    ((int8_t)(((op) >> 3) & 0x7F) | ((((op) >> 3) & 0x40) ? 0x80 : 0))
/* Sign-extend 7-bit to int */
#define BR7_SE(op) (((int16_t)((int8_t)(((((op) >> 3) & 0x7F) | (((op) & 0x0200) ? 0x80 : 0)) << 1))) >> 1)
/* 12-bit signed offset for RJMP/RCALL from bits 11:0 */
#define K12(op)    ((int16_t)((int16_t)((op) << 4)) >> 4)
/* 6-bit displacement for LDD/STD from bits 13,11:10,2:0 */
#define Q6(op)     ((((op) >> 8) & 0x20) | (((op) >> 7) & 0x18) | ((op) & 0x07))

/* ---------- Main decode/execute function ---------- */

uint8_t avr_decode_execute(avr_cpu_t *cpu)
{
    uint16_t pc = cpu->pc;
    if (pc >= cpu->flash_size / 2) {
        fprintf(stderr, "FAULT: PC=0x%04X out of flash bounds\n", pc);
        cpu->state = AVR_STATE_HALTED;
        return 1;
    }
    uint16_t opcode = cpu->flash[pc];
    uint8_t cycles = 1; /* default 1 cycle */

    /* Handle skip mode (from CPSE/SBRC/SBRS/SBIC/SBIS) */
    if (cpu->skip_next) {
        cpu->skip_next = 0;
        /* Determine instruction width to know how many words to skip */
        uint16_t top4 = opcode >> 12;
        int is_32bit = 0;
        if (top4 == 0x9) {
            uint16_t lo4 = opcode & 0x0F;
            /* LDS/STS: 1001 00xd dddd 0000 */
            if (((opcode >> 9) & 0x07) == 0 && lo4 == 0)
                is_32bit = 1;
            /* JMP: 1001 010k kkkk 110k */
            if ((opcode & 0xFE0E) == 0x940C)
                is_32bit = 1;
            /* CALL: 1001 010k kkkk 111k */
            if ((opcode & 0xFE0E) == 0x940E)
                is_32bit = 1;
        }
        cpu->pc = pc + (is_32bit ? 2 : 1);
        return is_32bit ? 2 : 1;
    }

    /* Advance PC past this instruction (may be adjusted by branches) */
    cpu->pc = pc + 1;

    switch (opcode >> 12) {

    /* ---- 0x0: NOP, MOVW, MULS, MULSU/FMUL*, CPC, SBC, ADD, CPSE ---- */
    case 0x0: {
        uint16_t sub = (opcode >> 8) & 0x0F;
        if (opcode == 0x0000) {
            /* NOP */
            cycles = 1;
        } else if (sub == 0x01) {
            /* MOVW Rd+1:Rd, Rr+1:Rr */
            if (!(cpu->variant->flags & AVR_FLAG_HAS_MOVW)) {
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
            uint8_t d = ((opcode >> 4) & 0x0F) << 1;
            uint8_t r = (opcode & 0x0F) << 1;
            AVR_R(cpu, d)   = AVR_R(cpu, r);
            AVR_R(cpu, d+1) = AVR_R(cpu, r+1);
            cycles = 1;
        } else if (sub == 0x02) {
            /* MULS Rd, Rr (d,r = 16-31) */
            if (!(cpu->variant->flags & AVR_FLAG_HAS_MUL)) {
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
            uint8_t d = ((opcode >> 4) & 0x0F) + 16;
            uint8_t r = (opcode & 0x0F) + 16;
            int16_t res = (int8_t)AVR_R(cpu, d) * (int8_t)AVR_R(cpu, r);
            AVR_R(cpu, 0) = res & 0xFF;
            AVR_R(cpu, 1) = (res >> 8) & 0xFF;
            set_flag(cpu, SREG_C, (res >> 15) & 1);
            set_flag(cpu, SREG_Z, res == 0);
            cycles = 2;
        } else if (sub == 0x03) {
            /* MULSU / FMUL / FMULS / FMULSU */
            if (!(cpu->variant->flags & AVR_FLAG_HAS_MUL)) {
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
            uint8_t d = ((opcode >> 4) & 0x07) + 16;
            uint8_t r = (opcode & 0x07) + 16;
            uint8_t type = (opcode >> 3) & 0x03;
            int16_t res;
            switch (type) {
            case 0: /* MULSU */
                res = (int8_t)AVR_R(cpu, d) * (uint8_t)AVR_R(cpu, r);
                AVR_R(cpu, 0) = res & 0xFF;
                AVR_R(cpu, 1) = (res >> 8) & 0xFF;
                set_flag(cpu, SREG_C, (res >> 15) & 1);
                set_flag(cpu, SREG_Z, res == 0);
                break;
            case 1: /* FMUL */
                res = (uint8_t)AVR_R(cpu, d) * (uint8_t)AVR_R(cpu, r);
                set_flag(cpu, SREG_C, (res >> 15) & 1);
                res <<= 1;
                AVR_R(cpu, 0) = res & 0xFF;
                AVR_R(cpu, 1) = (res >> 8) & 0xFF;
                set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
                break;
            case 2: /* FMULS */
                res = (int8_t)AVR_R(cpu, d) * (int8_t)AVR_R(cpu, r);
                set_flag(cpu, SREG_C, (res >> 15) & 1);
                res <<= 1;
                AVR_R(cpu, 0) = res & 0xFF;
                AVR_R(cpu, 1) = (res >> 8) & 0xFF;
                set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
                break;
            case 3: /* FMULSU */
                res = (int8_t)AVR_R(cpu, d) * (uint8_t)AVR_R(cpu, r);
                set_flag(cpu, SREG_C, (res >> 15) & 1);
                res <<= 1;
                AVR_R(cpu, 0) = res & 0xFF;
                AVR_R(cpu, 1) = (res >> 8) & 0xFF;
                set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
                break;
            }
            cycles = 2;
        } else if ((opcode & 0xFC00) == 0x0400) {
            /* CPC Rd, Rr */
            uint8_t d = RD5(opcode), r = RR5(opcode);
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
            uint8_t R = Rd - Rr - C;
            flags_sub(cpu, Rd, Rr, R, 1); /* keep_z = 1 for CPC */
            cycles = 1;
        } else if ((opcode & 0xFC00) == 0x0800) {
            /* SBC Rd, Rr */
            uint8_t d = RD5(opcode), r = RR5(opcode);
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
            uint8_t R = Rd - Rr - C;
            AVR_R(cpu, d) = R;
            flags_sub(cpu, Rd, Rr, R, 1);
            cycles = 1;
        } else if ((opcode & 0xFC00) == 0x0C00) {
            /* ADD Rd, Rr  (also LSL when Rd==Rr) */
            uint8_t d = RD5(opcode), r = RR5(opcode);
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t R = Rd + Rr;
            AVR_R(cpu, d) = R;
            flags_add(cpu, Rd, Rr, R);
            cycles = 1;
        } else if ((opcode & 0xFC00) == 0x1000) {
            /* CPSE Rd, Rr */
            uint8_t d = RD5(opcode), r = RR5(opcode);
            if (AVR_R(cpu, d) == AVR_R(cpu, r))
                cpu->skip_next = 1;
            cycles = 1;
        } else {
            /* Unknown opcode in 0x0xxx range */
            cpu->state = AVR_STATE_HALTED;
            return 1;
        }
        break;
    }

    /* ---- 0x1: CP, SUB, ADC, CPSE (cont) ---- */
    case 0x1: {
        uint16_t sub = (opcode >> 10) & 0x03;
        uint8_t d = RD5(opcode), r = RR5(opcode);
        switch (sub) {
        case 0: { /* CPSE: 0001 00rd dddd rrrr — already handled above at 0x1000 */
            /* But 0x1000-0x13FF is actually CPSE if top nibble is 0x1 with bits 11:10 = 00 */
            if (AVR_R(cpu, d) == AVR_R(cpu, r))
                cpu->skip_next = 1;
            cycles = 1;
            break;
        }
        case 1: { /* CP Rd, Rr */
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t R = Rd - Rr;
            flags_sub(cpu, Rd, Rr, R, 0);
            cycles = 1;
            break;
        }
        case 2: { /* SUB Rd, Rr */
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t R = Rd - Rr;
            AVR_R(cpu, d) = R;
            flags_sub(cpu, Rd, Rr, R, 0);
            cycles = 1;
            break;
        }
        case 3: { /* ADC Rd, Rr  (also ROL when Rd==Rr) */
            uint8_t Rd = AVR_R(cpu, d), Rr = AVR_R(cpu, r);
            uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
            uint8_t R = Rd + Rr + C;
            AVR_R(cpu, d) = R;
            flags_add(cpu, Rd, Rr, R);
            cycles = 1;
            break;
        }
        }
        break;
    }

    /* ---- 0x2: AND, EOR, OR, MOV ---- */
    case 0x2: {
        uint16_t sub = (opcode >> 10) & 0x03;
        uint8_t d = RD5(opcode), r = RR5(opcode);
        switch (sub) {
        case 0: { /* AND Rd, Rr  (also TST when Rd==Rr) */
            uint8_t R = AVR_R(cpu, d) & AVR_R(cpu, r);
            AVR_R(cpu, d) = R;
            flags_logic(cpu, R);
            break;
        }
        case 1: { /* EOR Rd, Rr  (also CLR when Rd==Rr) */
            uint8_t R = AVR_R(cpu, d) ^ AVR_R(cpu, r);
            AVR_R(cpu, d) = R;
            flags_logic(cpu, R);
            break;
        }
        case 2: { /* OR Rd, Rr */
            uint8_t R = AVR_R(cpu, d) | AVR_R(cpu, r);
            AVR_R(cpu, d) = R;
            flags_logic(cpu, R);
            break;
        }
        case 3: { /* MOV Rd, Rr */
            AVR_R(cpu, d) = AVR_R(cpu, r);
            break;
        }
        }
        cycles = 1;
        break;
    }

    /* ---- 0x3: CPI Rd, K ---- */
    case 0x3: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        uint8_t Rd = AVR_R(cpu, d);
        uint8_t R = Rd - K;
        flags_sub(cpu, Rd, K, R, 0);
        cycles = 1;
        break;
    }

    /* ---- 0x4: SBCI Rd, K ---- */
    case 0x4: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        uint8_t Rd = AVR_R(cpu, d);
        uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
        uint8_t R = Rd - K - C;
        AVR_R(cpu, d) = R;
        flags_sub(cpu, Rd, K, R, 1);
        cycles = 1;
        break;
    }

    /* ---- 0x5: SUBI Rd, K ---- */
    case 0x5: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        uint8_t Rd = AVR_R(cpu, d);
        uint8_t R = Rd - K;
        AVR_R(cpu, d) = R;
        flags_sub(cpu, Rd, K, R, 0);
        cycles = 1;
        break;
    }

    /* ---- 0x6: ORI Rd, K  (also SBR) ---- */
    case 0x6: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        uint8_t R = AVR_R(cpu, d) | K;
        AVR_R(cpu, d) = R;
        flags_logic(cpu, R);
        cycles = 1;
        break;
    }

    /* ---- 0x7: ANDI Rd, K  (also CBR) ---- */
    case 0x7: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        uint8_t R = AVR_R(cpu, d) & K;
        AVR_R(cpu, d) = R;
        flags_logic(cpu, R);
        cycles = 1;
        break;
    }

    /* ---- 0x8, 0xA: LDD/STD with Y+q or Z+q ---- */
    case 0x8:
    case 0xA: {
        uint8_t d = RD5(opcode);
        uint8_t q = Q6(opcode);
        if (opcode & 0x0200) {
            /* STD */
            uint16_t addr;
            if (opcode & 0x0008)
                addr = AVR_Y(cpu) + q; /* STD Y+q, Rr */
            else
                addr = AVR_Z(cpu) + q; /* STD Z+q, Rr */
            avr_data_write(cpu, addr, AVR_R(cpu, d));
            cycles = 2;
        } else {
            /* LDD */
            uint16_t addr;
            if (opcode & 0x0008)
                addr = AVR_Y(cpu) + q; /* LDD Rd, Y+q */
            else
                addr = AVR_Z(cpu) + q; /* LDD Rd, Z+q */
            AVR_R(cpu, d) = avr_data_read(cpu, addr);
            cycles = 2;
        }
        break;
    }

    /* ---- 0x9: Complex group ---- */
    case 0x9: {
        /* Sub-decode based on bits 11:9 and lower bits */
        uint16_t top7 = (opcode >> 9) & 0x07;

        if (top7 == 0 || top7 == 1) {
            /* 1001 000d dddd xxxx — LD variants / LDS / LPM / POP
             * 1001 001r rrrr xxxx — ST variants / STS / PUSH */
            uint8_t d = RD5(opcode);
            uint8_t lo4 = opcode & 0x0F;
            int is_store = (opcode >> 9) & 1;

            if (!is_store) {
                /* Load operations */
                switch (lo4) {
                case 0x0: {
                    /* LDS Rd, k (32-bit instruction) */
                    uint16_t addr = cpu->flash[cpu->pc];
                    cpu->pc++;
                    AVR_R(cpu, d) = avr_data_read(cpu, addr);
                    cycles = 2;
                    break;
                }
                case 0x1: { /* LD Rd, Z+ */
                    uint16_t z = AVR_Z(cpu);
                    AVR_R(cpu, d) = avr_data_read(cpu, z);
                    AVR_SET_Z(cpu, z + 1);
                    cycles = 2;
                    break;
                }
                case 0x2: { /* LD Rd, -Z */
                    uint16_t z = AVR_Z(cpu) - 1;
                    AVR_SET_Z(cpu, z);
                    AVR_R(cpu, d) = avr_data_read(cpu, z);
                    cycles = 2;
                    break;
                }
                case 0x4: { /* LPM Rd, Z */
                    if (!(cpu->variant->flags & AVR_FLAG_HAS_LPM_RD)) {
                        cpu->state = AVR_STATE_HALTED;
                        return 1;
                    }
                    AVR_R(cpu, d) = avr_flash_read_byte(cpu, AVR_Z(cpu));
                    cycles = 3;
                    break;
                }
                case 0x5: { /* LPM Rd, Z+ */
                    if (!(cpu->variant->flags & AVR_FLAG_HAS_LPM_RD)) {
                        cpu->state = AVR_STATE_HALTED;
                        return 1;
                    }
                    uint16_t z = AVR_Z(cpu);
                    AVR_R(cpu, d) = avr_flash_read_byte(cpu, z);
                    AVR_SET_Z(cpu, z + 1);
                    cycles = 3;
                    break;
                }
                case 0x9: { /* LD Rd, Y+ */
                    uint16_t y = AVR_Y(cpu);
                    AVR_R(cpu, d) = avr_data_read(cpu, y);
                    AVR_SET_Y(cpu, y + 1);
                    cycles = 2;
                    break;
                }
                case 0xA: { /* LD Rd, -Y */
                    uint16_t y = AVR_Y(cpu) - 1;
                    AVR_SET_Y(cpu, y);
                    AVR_R(cpu, d) = avr_data_read(cpu, y);
                    cycles = 2;
                    break;
                }
                case 0xC: { /* LD Rd, X */
                    AVR_R(cpu, d) = avr_data_read(cpu, AVR_X(cpu));
                    cycles = 2;
                    break;
                }
                case 0xD: { /* LD Rd, X+ */
                    uint16_t x = AVR_X(cpu);
                    AVR_R(cpu, d) = avr_data_read(cpu, x);
                    AVR_SET_X(cpu, x + 1);
                    cycles = 2;
                    break;
                }
                case 0xE: { /* LD Rd, -X */
                    uint16_t x = AVR_X(cpu) - 1;
                    AVR_SET_X(cpu, x);
                    AVR_R(cpu, d) = avr_data_read(cpu, x);
                    cycles = 2;
                    break;
                }
                case 0xF: { /* POP Rd */
                    AVR_R(cpu, d) = avr_pop(cpu);
                    cycles = 2;
                    break;
                }
                default:
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
            } else {
                /* Store operations: 1001 001r rrrr xxxx */
                switch (lo4) {
                case 0x0: {
                    /* STS k, Rr (32-bit instruction) */
                    uint16_t addr = cpu->flash[cpu->pc];
                    cpu->pc++;
                    avr_data_write(cpu, addr, AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                case 0x1: { /* ST Z+, Rr */
                    uint16_t z = AVR_Z(cpu);
                    avr_data_write(cpu, z, AVR_R(cpu, d));
                    AVR_SET_Z(cpu, z + 1);
                    cycles = 2;
                    break;
                }
                case 0x2: { /* ST -Z, Rr */
                    uint16_t z = AVR_Z(cpu) - 1;
                    AVR_SET_Z(cpu, z);
                    avr_data_write(cpu, z, AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                case 0x9: { /* ST Y+, Rr */
                    uint16_t y = AVR_Y(cpu);
                    avr_data_write(cpu, y, AVR_R(cpu, d));
                    AVR_SET_Y(cpu, y + 1);
                    cycles = 2;
                    break;
                }
                case 0xA: { /* ST -Y, Rr */
                    uint16_t y = AVR_Y(cpu) - 1;
                    AVR_SET_Y(cpu, y);
                    avr_data_write(cpu, y, AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                case 0xC: { /* ST X, Rr */
                    avr_data_write(cpu, AVR_X(cpu), AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                case 0xD: { /* ST X+, Rr */
                    uint16_t x = AVR_X(cpu);
                    avr_data_write(cpu, x, AVR_R(cpu, d));
                    AVR_SET_X(cpu, x + 1);
                    cycles = 2;
                    break;
                }
                case 0xE: { /* ST -X, Rr */
                    uint16_t x = AVR_X(cpu) - 1;
                    AVR_SET_X(cpu, x);
                    avr_data_write(cpu, x, AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                case 0xF: { /* PUSH Rr */
                    avr_push(cpu, AVR_R(cpu, d));
                    cycles = 2;
                    break;
                }
                default:
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
            }
        } else if (top7 == 2) {
            /* 1001 010d dddd xxxx — single-register operations */
            uint8_t d = RD5(opcode);
            uint8_t lo4 = opcode & 0x0F;
            switch (lo4) {
            case 0x0: { /* COM Rd */
                uint8_t R = 0xFF - AVR_R(cpu, d);
                AVR_R(cpu, d) = R;
                flags_logic(cpu, R);
                set_flag(cpu, SREG_C, 1);
                cycles = 1;
                break;
            }
            case 0x1: { /* NEG Rd */
                uint8_t Rd = AVR_R(cpu, d);
                uint8_t R = 0 - Rd;
                AVR_R(cpu, d) = R;
                flags_sub(cpu, 0, Rd, R, 0);
                set_flag(cpu, SREG_C, R != 0);
                cycles = 1;
                break;
            }
            case 0x2: { /* SWAP Rd */
                uint8_t val = AVR_R(cpu, d);
                AVR_R(cpu, d) = ((val >> 4) & 0x0F) | ((val << 4) & 0xF0);
                cycles = 1;
                break;
            }
            case 0x3: { /* INC Rd */
                uint8_t R = AVR_R(cpu, d) + 1;
                AVR_R(cpu, d) = R;
                flags_inc(cpu, R);
                cycles = 1;
                break;
            }
            case 0x5: { /* ASR Rd */
                uint8_t Rd = AVR_R(cpu, d);
                uint8_t R = (Rd >> 1) | (Rd & 0x80);
                AVR_R(cpu, d) = R;
                set_flag(cpu, SREG_C, Rd & 1);
                set_flag(cpu, SREG_N, (R >> 7) & 1);
                set_flag(cpu, SREG_Z, R == 0);
                set_flag(cpu, SREG_V, (Rd & 1) ^ ((R >> 7) & 1));
                set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
                cycles = 1;
                break;
            }
            case 0x6: { /* LSR Rd */
                uint8_t Rd = AVR_R(cpu, d);
                uint8_t R = Rd >> 1;
                AVR_R(cpu, d) = R;
                set_flag(cpu, SREG_C, Rd & 1);
                set_flag(cpu, SREG_N, 0);
                set_flag(cpu, SREG_Z, R == 0);
                set_flag(cpu, SREG_V, Rd & 1); /* V = N xor C = 0 xor C = C */
                set_flag(cpu, SREG_S, Rd & 1); /* S = N xor V = 0 xor C = C */
                cycles = 1;
                break;
            }
            case 0x7: { /* ROR Rd */
                uint8_t Rd = AVR_R(cpu, d);
                uint8_t C = (cpu->sreg & SREG_C) ? 0x80 : 0;
                uint8_t R = (Rd >> 1) | C;
                AVR_R(cpu, d) = R;
                set_flag(cpu, SREG_C, Rd & 1);
                set_flag(cpu, SREG_N, (R >> 7) & 1);
                set_flag(cpu, SREG_Z, R == 0);
                set_flag(cpu, SREG_V, ((R >> 7) & 1) ^ (Rd & 1));
                set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
                cycles = 1;
                break;
            }
            case 0x8: {
                /* Various: BSET/BCLR, RET/RETI, SLEEP, WDR, BREAK, LPM, SPM, IJMP, ICALL */
                if ((opcode & 0xFF8F) == 0x9408) {
                    /* BSET s: 1001 0100 0sss 1000 */
                    uint8_t s = (opcode >> 4) & 0x07;
                    cpu->sreg |= (1 << s);
                    cycles = 1;
                } else if ((opcode & 0xFF8F) == 0x9488) {
                    /* BCLR s: 1001 0100 1sss 1000 */
                    uint8_t s = (opcode >> 4) & 0x07;
                    cpu->sreg &= ~(1 << s);
                    cycles = 1;
                } else if (opcode == 0x9508) {
                    /* RET: SP+1→PCH, SP+1→PCL */
                    uint8_t pch = avr_pop(cpu);
                    uint8_t pcl = avr_pop(cpu);
                    cpu->pc = ((uint16_t)pch << 8) | pcl;
                    cycles = 4;
                } else if (opcode == 0x9518) {
                    /* RETI: SP+1→PCH, SP+1→PCL */
                    uint8_t pch = avr_pop(cpu);
                    uint8_t pcl = avr_pop(cpu);
                    cpu->pc = ((uint16_t)pch << 8) | pcl;
                    cpu->sreg |= SREG_I;
                    cycles = 4;
                } else if (opcode == 0x9588) {
                    /* SLEEP */
                    cpu->state = AVR_STATE_SLEEPING;
                    cycles = 1;
                } else if (opcode == 0x95A8) {
                    /* WDR (watchdog reset - NOP in emulator) */
                    cycles = 1;
                } else if (opcode == 0x9598) {
                    /* BREAK */
                    cpu->state = AVR_STATE_BREAK;
                    cycles = 1;
                } else if (opcode == 0x95C8) {
                    /* LPM (R0 ← flash[Z]) */
                    AVR_R(cpu, 0) = avr_flash_read_byte(cpu, AVR_Z(cpu));
                    cycles = 3;
                } else if (opcode == 0x95E8) {
                    /* SPM (store program memory - NOP in emulator) */
                    cycles = 1;
                } else {
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
                break;
            }
            case 0x9: {
                if (opcode == 0x9409) {
                    /* IJMP: 1001 0100 0000 1001 */
                    cpu->pc = AVR_Z(cpu);
                    cycles = 2;
                } else if (opcode == 0x9509) {
                    /* ICALL: 1001 0101 0000 1001 */
                    avr_push(cpu, cpu->pc & 0xFF);
                    avr_push(cpu, (cpu->pc >> 8) & 0xFF);
                    cpu->pc = AVR_Z(cpu);
                    cycles = 3;
                } else {
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
                break;
            }
            case 0xA: { /* DEC Rd */
                uint8_t R = AVR_R(cpu, d) - 1;
                AVR_R(cpu, d) = R;
                flags_dec(cpu, R);
                cycles = 1;
                break;
            }
            case 0xC:
            case 0xD: {
                /* JMP k: 1001 010k kkkk 110k  (32-bit) */
                if (!(cpu->variant->flags & AVR_FLAG_HAS_JMP_CALL)) {
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
                uint32_t k = ((uint32_t)(opcode & 0x01F0) << 13) |
                             ((uint32_t)(opcode & 0x0001) << 16) |
                             cpu->flash[cpu->pc];
                cpu->pc = k;
                /* Note: pc was already incremented past first word,
                 * but JMP sets it absolutely */
                cycles = 3;
                break;
            }
            case 0xE:
            case 0xF: {
                /* CALL k: 1001 010k kkkk 111k  (32-bit) */
                if (!(cpu->variant->flags & AVR_FLAG_HAS_JMP_CALL)) {
                    cpu->state = AVR_STATE_HALTED;
                    return 1;
                }
                uint16_t ret = cpu->pc + 1; /* return to word after 2nd word */
                uint32_t k = ((uint32_t)(opcode & 0x01F0) << 13) |
                             ((uint32_t)(opcode & 0x0001) << 16) |
                             cpu->flash[cpu->pc];
                avr_push(cpu, ret & 0xFF);
                avr_push(cpu, (ret >> 8) & 0xFF);
                cpu->pc = k;
                cycles = 4;
                break;
            }
            default:
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
        } else if (top7 == 3) {
            /* 1001 011x — ADIW / SBIW */
            if (!(cpu->variant->flags & AVR_FLAG_HAS_ADIW)) {
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
            uint8_t d = RDW2(opcode);
            uint8_t K = K6(opcode);
            uint16_t Rdw = AVR_REGW(cpu, d);
            uint16_t R;
            if (opcode & 0x0100) {
                /* SBIW */
                R = Rdw - K;
                set_flag(cpu, SREG_C, (R & 0x8000) && !(Rdw & 0x8000));
                /* C is set if result bit 15 is set and Rdw bit 15 was 0 ...
                 * actually: C = Rdw_high_bit15 NOT AND R15 ... let me use the standard formula */
                set_flag(cpu, SREG_C, (~Rdw & R) >> 15);
                set_flag(cpu, SREG_V, (Rdw & ~R) >> 15);
            } else {
                /* ADIW */
                R = Rdw + K;
                set_flag(cpu, SREG_C, (~R & Rdw) >> 15);
                set_flag(cpu, SREG_V, (R & ~Rdw) >> 15);
            }
            AVR_SET_REGW(cpu, d, R);
            set_flag(cpu, SREG_N, (R >> 15) & 1);
            set_flag(cpu, SREG_Z, R == 0);
            set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
            cycles = 2;
        } else if (top7 == 4) {
            /* 1001 100x — CBI / SBIC */
            uint8_t A = IO5(opcode);
            uint8_t b = BIT3(opcode);
            if (opcode & 0x0100) {
                /* SBIC A, b: skip if bit in I/O register is clear */
                if (!(avr_io_read(cpu, A) & (1 << b)))
                    cpu->skip_next = 1;
                cycles = 1;
            } else {
                /* CBI A, b: clear bit in I/O register */
                uint8_t val = avr_io_read(cpu, A) & ~(1 << b);
                avr_io_write(cpu, A, val);
                cycles = 2;
            }
        } else if (top7 == 5) {
            /* 1001 101x — SBI / SBIS */
            uint8_t A = IO5(opcode);
            uint8_t b = BIT3(opcode);
            if (opcode & 0x0100) {
                /* SBIS A, b: skip if bit in I/O register is set */
                if (avr_io_read(cpu, A) & (1 << b))
                    cpu->skip_next = 1;
                cycles = 1;
            } else {
                /* SBI A, b: set bit in I/O register */
                uint8_t val = avr_io_read(cpu, A) | (1 << b);
                avr_io_write(cpu, A, val);
                cycles = 2;
            }
        } else if (top7 == 6 || top7 == 7) {
            /* 1001 11rd dddd rrrr — MUL Rd, Rr (unsigned) */
            if (!(cpu->variant->flags & AVR_FLAG_HAS_MUL)) {
                cpu->state = AVR_STATE_HALTED;
                return 1;
            }
            uint8_t d = RD5(opcode), r = RR5(opcode);
            uint16_t res = (uint16_t)AVR_R(cpu, d) * (uint16_t)AVR_R(cpu, r);
            AVR_R(cpu, 0) = res & 0xFF;
            AVR_R(cpu, 1) = (res >> 8) & 0xFF;
            set_flag(cpu, SREG_C, (res >> 15) & 1);
            set_flag(cpu, SREG_Z, res == 0);
            cycles = 2;
        }
        break;
    }

    /* ---- 0xB: IN / OUT ---- */
    case 0xB: {
        uint8_t d = RD5(opcode);
        uint8_t A = IO6(opcode);
        if (opcode & 0x0800) {
            /* OUT A, Rr */
            avr_io_write(cpu, A, AVR_R(cpu, d));
        } else {
            /* IN Rd, A */
            AVR_R(cpu, d) = avr_io_read(cpu, A);
        }
        cycles = 1;
        break;
    }

    /* ---- 0xC: RJMP k ---- */
    case 0xC: {
        int16_t k = K12(opcode);
        cpu->pc = pc + 1 + k;
        cycles = 2;
        break;
    }

    /* ---- 0xD: RCALL k ---- */
    case 0xD: {
        int16_t k = K12(opcode);
        uint16_t ret = pc + 1;
        avr_push(cpu, ret & 0xFF);
        avr_push(cpu, (ret >> 8) & 0xFF);
        cpu->pc = pc + 1 + k;
        cycles = 3;
        break;
    }

    /* ---- 0xE: LDI Rd, K ---- */
    case 0xE: {
        uint8_t d = RD4(opcode);
        uint8_t K = K8(opcode);
        AVR_R(cpu, d) = K;
        cycles = 1;
        break;
    }

    /* ---- 0xF: Branches, BST, BLD, SBRC, SBRS ---- */
    case 0xF: {
        uint16_t sub = (opcode >> 10) & 0x03;
        switch (sub) {
        case 0: {
            /* BRBS s, k: 1111 00kk kkkk ksss — branch if bit in SREG is set */
            uint8_t s = BIT3(opcode);
            int8_t k = (int8_t)((opcode >> 3) & 0x7F);
            if (k & 0x40) k |= 0x80; /* sign extend */
            if (cpu->sreg & (1 << s)) {
                cpu->pc = pc + 1 + k;
                cycles = 2;
            } else {
                cycles = 1;
            }
            break;
        }
        case 1: {
            /* BRBC s, k: 1111 01kk kkkk ksss — branch if bit in SREG is clear */
            uint8_t s = BIT3(opcode);
            int8_t k = (int8_t)((opcode >> 3) & 0x7F);
            if (k & 0x40) k |= 0x80;
            if (!(cpu->sreg & (1 << s))) {
                cpu->pc = pc + 1 + k;
                cycles = 2;
            } else {
                cycles = 1;
            }
            break;
        }
        case 2: {
            if (opcode & 0x0200) {
                /* BST Rd, b: 1111 101d dddd 0bbb */
                uint8_t d = RD5(opcode);
                uint8_t b = BIT3(opcode);
                set_flag(cpu, SREG_T, (AVR_R(cpu, d) >> b) & 1);
            } else {
                /* BLD Rd, b: 1111 100d dddd 0bbb */
                uint8_t d = RD5(opcode);
                uint8_t b = BIT3(opcode);
                if (cpu->sreg & SREG_T)
                    AVR_R(cpu, d) |= (1 << b);
                else
                    AVR_R(cpu, d) &= ~(1 << b);
            }
            cycles = 1;
            break;
        }
        case 3: {
            uint8_t r = RD5(opcode);
            uint8_t b = BIT3(opcode);
            if (opcode & 0x0200) {
                /* SBRS Rr, b: 1111 111r rrrr 0bbb */
                if (AVR_R(cpu, r) & (1 << b))
                    cpu->skip_next = 1;
            } else {
                /* SBRC Rr, b: 1111 110r rrrr 0bbb */
                if (!(AVR_R(cpu, r) & (1 << b)))
                    cpu->skip_next = 1;
            }
            cycles = 1;
            break;
        }
        }
        break;
    }

    default:
        cpu->state = AVR_STATE_HALTED;
        return 1;
    }

    return cycles;
}
