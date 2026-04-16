/*
 * ucvm - AVR instruction decoder and executor
 *
 * Two-phase design:
 *   1. avr_predecode() — run once at firmware load, classifies each flash
 *      word into a compact (op, a, b) tuple with pre-extracted operands.
 *   2. avr_cpu_run()   — threaded interpreter using GCC computed goto
 *      (falls back to switch on non-GCC compilers).  Each handler reads
 *      pre-extracted operands from the decode cache, eliminating per-step
 *      bit manipulation.  Each handler dispatches directly to the next,
 *      giving the branch predictor distinct source sites.
 */
#include "avr_cpu.h"
#include "avr_periph.h"
#include <string.h>

/* Stack operations (defined in avr_cpu.c) */
extern void avr_push(avr_cpu_t *cpu, uint8_t val);
extern uint8_t avr_pop(avr_cpu_t *cpu);

/* ---------- SREG flag helpers ---------- */

static inline void set_flag(avr_cpu_t *cpu, uint8_t flag, uint8_t val)
{
    if (val) cpu->sreg |= flag;
    else     cpu->sreg &= ~flag;
}

static inline void flags_add(avr_cpu_t *cpu, uint8_t Rd, uint8_t Rr, uint8_t R)
{
    uint8_t carry3 = (Rd & Rr) | (Rr & ~R) | (~R & Rd);
    uint8_t carry7 = carry3;
    uint8_t N = (R >> 7) & 1;
    uint8_t V = (((Rd ^ R) & (Rr ^ R)) >> 7) & 1;
    cpu->sreg = (cpu->sreg & (SREG_T | SREG_I))
        | ((carry3 & 0x08) ? SREG_H : 0)
        | (V ? SREG_V : 0)
        | (N ? SREG_N : 0)
        | (R == 0 ? SREG_Z : 0)
        | ((carry7 & 0x80) ? SREG_C : 0)
        | ((N ^ V) ? SREG_S : 0);
}

static inline void flags_sub(avr_cpu_t *cpu, uint8_t Rd, uint8_t Rr, uint8_t R, int keep_z)
{
    uint8_t borrow3 = (~Rd & Rr) | (Rr & R) | (R & ~Rd);
    uint8_t N = (R >> 7) & 1;
    uint8_t V = (((Rd ^ Rr) & (Rd ^ R)) >> 7) & 1;
    uint8_t Z = keep_z ? ((cpu->sreg & SREG_Z) && (R == 0)) : (R == 0);
    uint8_t borrow7 = (~Rd & Rr) | (Rr & R) | (R & ~Rd);
    cpu->sreg = (cpu->sreg & (SREG_T | SREG_I))
        | ((borrow3 & 0x08) ? SREG_H : 0)
        | (V ? SREG_V : 0)
        | (N ? SREG_N : 0)
        | (Z ? SREG_Z : 0)
        | ((borrow7 & 0x80) ? SREG_C : 0)
        | ((N ^ V) ? SREG_S : 0);
}

static inline void flags_logic(avr_cpu_t *cpu, uint8_t R)
{
    uint8_t N = (R >> 7) & 1;
    cpu->sreg = (cpu->sreg & (SREG_T | SREG_I | SREG_H | SREG_C))
        | (N ? SREG_N : 0)
        | (R == 0 ? SREG_Z : 0)
        | (N ? SREG_S : 0);
}

static inline void flags_inc(avr_cpu_t *cpu, uint8_t R)
{
    uint8_t N = (R >> 7) & 1;
    uint8_t V = (R == 0x80);
    cpu->sreg = (cpu->sreg & (SREG_T | SREG_I | SREG_H | SREG_C))
        | (V ? SREG_V : 0)
        | (N ? SREG_N : 0)
        | (R == 0 ? SREG_Z : 0)
        | ((N ^ V) ? SREG_S : 0);
}

static inline void flags_dec(avr_cpu_t *cpu, uint8_t R)
{
    uint8_t N = (R >> 7) & 1;
    uint8_t V = (R == 0x7F);
    cpu->sreg = (cpu->sreg & (SREG_T | SREG_I | SREG_H | SREG_C))
        | (V ? SREG_V : 0)
        | (N ? SREG_N : 0)
        | (R == 0 ? SREG_Z : 0)
        | ((N ^ V) ? SREG_S : 0);
}

/* ---------- Operand extraction macros (used by predecode) ---------- */

#define RD5(op)    (((op) >> 4) & 0x1F)
#define RR5(op)    ((((op) >> 5) & 0x10) | ((op) & 0x0F))
#define RD4(op)    ((((op) >> 4) & 0x0F) + 16)
#define K8(op)     ((((op) >> 4) & 0xF0) | ((op) & 0x0F))
#define K6(op)     ((((op) >> 2) & 0x30) | ((op) & 0x0F))
#define RDW2(op)   (24 + ((((op) >> 4) & 0x03) << 1))
#define IO6(op)    ((((op) >> 5) & 0x30) | ((op) & 0x0F))
#define IO5(op)    (((op) >> 3) & 0x1F)
#define BIT3(op)   ((op) & 0x07)
#define K12(op)    ((int16_t)((int16_t)((op) << 4)) >> 4)
#define Q6(op)     ((((op) >> 8) & 0x20) | (((op) >> 7) & 0x18) | ((op) & 0x07))

/* ================================================================== */
/*  Phase 1: Pre-decode all flash words                               */
/* ================================================================== */

void avr_predecode(avr_cpu_t *cpu)
{
    uint32_t num_words = cpu->flash_size / 2;
    const uint16_t *flash = cpu->flash;
    avr_decoded_t *cache = cpu->decode_cache;
    uint8_t vf = cpu->variant->flags;

    if (!cache) return;

    for (uint32_t i = 0; i < num_words; i++) {
        uint16_t op = flash[i];
        avr_decoded_t *d = &cache[i];
        d->op = AVR_OP_FAULT;
        d->a = 0;
        d->b = 0;

        switch (op >> 12) {

        case 0x0: {
            uint8_t sub = (op >> 8) & 0x0F;
            if (op == 0x0000) {
                d->op = AVR_OP_NOP;
            } else if (sub == 0x01) {
                d->op = (vf & AVR_FLAG_HAS_MOVW) ? AVR_OP_MOVW : AVR_OP_FAULT;
                d->a = ((op >> 4) & 0x0F) << 1;
                d->b = (op & 0x0F) << 1;
            } else if (sub == 0x02) {
                d->op = (vf & AVR_FLAG_HAS_MUL) ? AVR_OP_MULS : AVR_OP_FAULT;
                d->a = ((op >> 4) & 0x0F) + 16;
                d->b = (op & 0x0F) + 16;
            } else if (sub == 0x03) {
                if (!(vf & AVR_FLAG_HAS_MUL)) break;
                d->a = ((op >> 4) & 0x07) + 16;
                d->b = (op & 0x07) + 16;
                switch ((op >> 3) & 0x03) {
                case 0: d->op = AVR_OP_MULSU; break;
                case 1: d->op = AVR_OP_FMUL; break;
                case 2: d->op = AVR_OP_FMULS; break;
                case 3: d->op = AVR_OP_FMULSU; break;
                }
            } else if ((op & 0xFC00) == 0x0400) {
                d->op = AVR_OP_CPC; d->a = RD5(op); d->b = RR5(op);
            } else if ((op & 0xFC00) == 0x0800) {
                d->op = AVR_OP_SBC; d->a = RD5(op); d->b = RR5(op);
            } else if ((op & 0xFC00) == 0x0C00) {
                d->op = AVR_OP_ADD; d->a = RD5(op); d->b = RR5(op);
            } else if ((op & 0xFC00) == 0x1000) {
                d->op = AVR_OP_CPSE; d->a = RD5(op); d->b = RR5(op);
            }
            break;
        }

        case 0x1: {
            uint8_t sub = (op >> 10) & 0x03;
            d->a = RD5(op); d->b = RR5(op);
            switch (sub) {
            case 0: d->op = AVR_OP_CPSE; break;
            case 1: d->op = AVR_OP_CP;   break;
            case 2: d->op = AVR_OP_SUB;  break;
            case 3: d->op = AVR_OP_ADC;  break;
            }
            break;
        }

        case 0x2: {
            uint8_t sub = (op >> 10) & 0x03;
            d->a = RD5(op); d->b = RR5(op);
            switch (sub) {
            case 0: d->op = AVR_OP_AND; break;
            case 1: d->op = AVR_OP_EOR; break;
            case 2: d->op = AVR_OP_OR;  break;
            case 3: d->op = AVR_OP_MOV; break;
            }
            break;
        }

        case 0x3:
            d->op = AVR_OP_CPI; d->a = RD4(op); d->b = K8(op);
            break;
        case 0x4:
            d->op = AVR_OP_SBCI; d->a = RD4(op); d->b = K8(op);
            break;
        case 0x5:
            d->op = AVR_OP_SUBI; d->a = RD4(op); d->b = K8(op);
            break;
        case 0x6:
            d->op = AVR_OP_ORI; d->a = RD4(op); d->b = K8(op);
            break;
        case 0x7:
            d->op = AVR_OP_ANDI; d->a = RD4(op); d->b = K8(op);
            break;

        case 0x8:
        case 0xA: {
            d->a = RD5(op);
            d->b = Q6(op);
            if (op & 0x0200) {
                d->op = (op & 0x0008) ? AVR_OP_STD_Y : AVR_OP_STD_Z;
            } else {
                d->op = (op & 0x0008) ? AVR_OP_LDD_Y : AVR_OP_LDD_Z;
            }
            break;
        }

        case 0x9: {
            uint16_t top7 = (op >> 9) & 0x07;

            if (top7 == 0 || top7 == 1) {
                /* LD/ST variants, LDS/STS, LPM, POP, PUSH */
                d->a = RD5(op);
                uint8_t lo4 = op & 0x0F;
                int is_store = (op >> 9) & 1;

                if (!is_store) {
                    switch (lo4) {
                    case 0x0: /* LDS (32-bit) */
                        d->op = AVR_OP_LDS;
                        if (i + 1 < num_words) {
                            d->b = flash[i + 1];
                            cache[i + 1].op = AVR_OP_DATA;
                            cache[i + 1].a = 0; cache[i + 1].b = 0;
                        }
                        i++; /* skip data word */
                        break;
                    case 0x1: d->op = AVR_OP_LD_ZP; break;
                    case 0x2: d->op = AVR_OP_LD_MZ; break;
                    case 0x4:
                        d->op = (vf & AVR_FLAG_HAS_LPM_RD) ? AVR_OP_LPM_RD : AVR_OP_FAULT;
                        break;
                    case 0x5:
                        d->op = (vf & AVR_FLAG_HAS_LPM_RD) ? AVR_OP_LPM_RDP : AVR_OP_FAULT;
                        break;
                    case 0x9: d->op = AVR_OP_LD_YP; break;
                    case 0xA: d->op = AVR_OP_LD_MY; break;
                    case 0xC: d->op = AVR_OP_LD_X;  break;
                    case 0xD: d->op = AVR_OP_LD_XP; break;
                    case 0xE: d->op = AVR_OP_LD_MX; break;
                    case 0xF: d->op = AVR_OP_POP;   break;
                    default:  d->op = AVR_OP_FAULT;  break;
                    }
                } else {
                    switch (lo4) {
                    case 0x0: /* STS (32-bit) */
                        d->op = AVR_OP_STS;
                        if (i + 1 < num_words) {
                            d->b = flash[i + 1];
                            cache[i + 1].op = AVR_OP_DATA;
                            cache[i + 1].a = 0; cache[i + 1].b = 0;
                        }
                        i++;
                        break;
                    case 0x1: d->op = AVR_OP_ST_ZP; break;
                    case 0x2: d->op = AVR_OP_ST_MZ; break;
                    case 0x9: d->op = AVR_OP_ST_YP; break;
                    case 0xA: d->op = AVR_OP_ST_MY; break;
                    case 0xC: d->op = AVR_OP_ST_X;  break;
                    case 0xD: d->op = AVR_OP_ST_XP; break;
                    case 0xE: d->op = AVR_OP_ST_MX; break;
                    case 0xF: d->op = AVR_OP_PUSH;  break;
                    default:  d->op = AVR_OP_FAULT;  break;
                    }
                }
            } else if (top7 == 2) {
                /* Single-register ops: 1001 010d dddd xxxx */
                d->a = RD5(op);
                uint8_t lo4 = op & 0x0F;
                switch (lo4) {
                case 0x0: d->op = AVR_OP_COM;  break;
                case 0x1: d->op = AVR_OP_NEG;  break;
                case 0x2: d->op = AVR_OP_SWAP; break;
                case 0x3: d->op = AVR_OP_INC;  break;
                case 0x5: d->op = AVR_OP_ASR;  break;
                case 0x6: d->op = AVR_OP_LSR;  break;
                case 0x7: d->op = AVR_OP_ROR;  break;
                case 0x8:
                    if ((op & 0xFF8F) == 0x9408) {
                        d->op = AVR_OP_BSET;
                        d->a = 1 << ((op >> 4) & 0x07);
                    } else if ((op & 0xFF8F) == 0x9488) {
                        d->op = AVR_OP_BCLR;
                        d->a = 1 << ((op >> 4) & 0x07);
                    } else if (op == 0x9508) {
                        d->op = AVR_OP_RET;
                    } else if (op == 0x9518) {
                        d->op = AVR_OP_RETI;
                    } else if (op == 0x9588) {
                        d->op = AVR_OP_SLEEP;
                    } else if (op == 0x95A8) {
                        d->op = AVR_OP_WDR;
                    } else if (op == 0x9598) {
                        d->op = AVR_OP_BREAK;
                    } else if (op == 0x95C8) {
                        d->op = AVR_OP_LPM_R0;
                    } else if (op == 0x95E8) {
                        d->op = AVR_OP_SPM;
                    }
                    break;
                case 0x9:
                    if (op == 0x9409)      d->op = AVR_OP_IJMP;
                    else if (op == 0x9509) d->op = AVR_OP_ICALL;
                    break;
                case 0xA: d->op = AVR_OP_DEC; break;
                case 0xC: case 0xD: {
                    /* JMP (32-bit) */
                    if (!(vf & AVR_FLAG_HAS_JMP_CALL)) break;
                    d->op = AVR_OP_JMP;
                    if (i + 1 < num_words) {
                        uint32_t k = ((uint32_t)(op & 0x01F0) << 13) |
                                     ((uint32_t)(op & 0x0001) << 16) |
                                     flash[i + 1];
                        d->b = (uint16_t)k;
                        cache[i + 1].op = AVR_OP_DATA;
                        cache[i + 1].a = 0; cache[i + 1].b = 0;
                    }
                    i++;
                    break;
                }
                case 0xE: case 0xF: {
                    /* CALL (32-bit) */
                    if (!(vf & AVR_FLAG_HAS_JMP_CALL)) break;
                    d->op = AVR_OP_CALL;
                    if (i + 1 < num_words) {
                        uint32_t k = ((uint32_t)(op & 0x01F0) << 13) |
                                     ((uint32_t)(op & 0x0001) << 16) |
                                     flash[i + 1];
                        d->b = (uint16_t)k;
                        cache[i + 1].op = AVR_OP_DATA;
                        cache[i + 1].a = 0; cache[i + 1].b = 0;
                    }
                    i++;
                    break;
                }
                default: break;
                }
            } else if (top7 == 3) {
                /* ADIW / SBIW */
                if (!(vf & AVR_FLAG_HAS_ADIW)) break;
                d->a = RDW2(op);
                d->b = K6(op);
                d->op = (op & 0x0100) ? AVR_OP_SBIW : AVR_OP_ADIW;
            } else if (top7 == 4) {
                /* CBI / SBIC */
                d->a = IO5(op);
                d->b = 1 << BIT3(op);
                d->op = (op & 0x0100) ? AVR_OP_SBIC : AVR_OP_CBI;
            } else if (top7 == 5) {
                /* SBI / SBIS */
                d->a = IO5(op);
                d->b = 1 << BIT3(op);
                d->op = (op & 0x0100) ? AVR_OP_SBIS : AVR_OP_SBI;
            } else if (top7 == 6 || top7 == 7) {
                /* MUL Rd, Rr */
                if (!(vf & AVR_FLAG_HAS_MUL)) break;
                d->op = AVR_OP_MUL;
                d->a = RD5(op);
                d->b = RR5(op);
            }
            break;
        }

        case 0xB: {
            d->a = RD5(op);
            d->b = IO6(op);
            d->op = (op & 0x0800) ? AVR_OP_OUT : AVR_OP_IN;
            break;
        }

        case 0xC: {
            d->op = AVR_OP_RJMP;
            d->b = (uint16_t)(i + 1 + K12(op));
            break;
        }

        case 0xD: {
            d->op = AVR_OP_RCALL;
            d->b = (uint16_t)(i + 1 + K12(op));
            break;
        }

        case 0xE:
            d->op = AVR_OP_LDI; d->a = RD4(op); d->b = K8(op);
            break;

        case 0xF: {
            uint8_t sub = (op >> 10) & 0x03;
            switch (sub) {
            case 0: {
                /* BRBS: branch if SREG bit set */
                d->op = AVR_OP_BRBS;
                d->a = 1 << BIT3(op);
                int8_t k = (int8_t)((op >> 3) & 0x7F);
                if (k & 0x40) k |= 0x80;
                d->b = (uint16_t)(i + 1 + k);
                break;
            }
            case 1: {
                /* BRBC: branch if SREG bit clear */
                d->op = AVR_OP_BRBC;
                d->a = 1 << BIT3(op);
                int8_t k = (int8_t)((op >> 3) & 0x7F);
                if (k & 0x40) k |= 0x80;
                d->b = (uint16_t)(i + 1 + k);
                break;
            }
            case 2:
                d->a = RD5(op);
                d->b = 1 << BIT3(op);
                d->op = (op & 0x0200) ? AVR_OP_BST : AVR_OP_BLD;
                break;
            case 3:
                d->a = RD5(op);
                d->b = 1 << BIT3(op);
                d->op = (op & 0x0200) ? AVR_OP_SBRS : AVR_OP_SBRC;
                break;
            }
            break;
        }

        default:
            d->op = AVR_OP_FAULT;
            break;
        }
    }
}

/* ================================================================== */
/*  Phase 2: Threaded interpreter                                     */
/* ================================================================== */

#define PERIPH_TICK_INTERVAL 64

/* Peripheral tick — kept out-of-line to reduce code size at each
 * handler dispatch site.  IRAM to avoid flash-cache miss on call. */
AVR_HOT static void __attribute__((noinline)) avr_tick_peripherals(avr_cpu_t *cpu)
{
    uint16_t elapsed = cpu->periph_accum;
    cpu->periph_accum = 0;
    if (cpu->periph_timer)
        avr_timer0_tick(cpu, cpu->periph_timer, elapsed);
    if (cpu->periph_twi)
        avr_twi_tick(cpu, cpu->periph_twi, elapsed);
    if (cpu->irq_pending && (cpu->sreg & SREG_I))
        avr_cpu_check_irq(cpu);
}

/* Helper: is a decoded opcode a 32-bit instruction? (for skip width) */
static inline int avr_is_32bit(uint8_t op)
{
    return op == AVR_OP_LDS || op == AVR_OP_STS ||
           op == AVR_OP_JMP || op == AVR_OP_CALL;
}

/* Computed-goto support */
#if defined(__GNUC__) || defined(__clang__)
#define USE_THREADED 1
#else
#define USE_THREADED 0
#endif

/*
 * Hot locals kept in registers across handlers:
 *   pc     — program counter (avoids volatile-like struct write per step)
 *   pa     — periph_accum   (avoids struct read/write per step)
 *   state  — CPU state       (avoids volatile struct read per step)
 *   cycles — total cycles    (already register-promoted)
 *   steps  — step counter
 *
 * Written back to struct at peripheral tick and at function exit.
 */
#if USE_THREADED
#define H(op) op:
#define NEXT(cyc) do { \
    cycles += (cyc); \
    pa += (cyc); \
    if (__builtin_expect(pa >= PERIPH_TICK_INTERVAL, 0)) { \
        cpu->pc = pc; cpu->periph_accum = pa; cpu->cycles = cycles; \
        avr_tick_peripherals(cpu); \
        pa = cpu->periph_accum; \
        pc = cpu->pc; /* IRQ may have redirected PC */ \
    } \
    if (__builtin_expect(--steps <= 0 || state != AVR_STATE_RUNNING, 0)) \
        goto done; \
    d = &cache[pc]; \
    goto *htab[d->op]; \
} while(0)
#else
#define H(op) case op:
#define NEXT(cyc) do { \
    cycles += (cyc); \
    pa += (cyc); \
    if (pa >= PERIPH_TICK_INTERVAL) { \
        cpu->pc = pc; cpu->periph_accum = pa; cpu->cycles = cycles; \
        avr_tick_peripherals(cpu); \
        pa = cpu->periph_accum; \
        pc = cpu->pc; \
    } \
    if (--steps <= 0 || state != AVR_STATE_RUNNING) \
        goto done; \
    break; \
} while(0)
#endif

AVR_HOT uint32_t avr_cpu_run(avr_cpu_t *cpu, int max_steps)
{
    if (cpu->state != AVR_STATE_RUNNING || max_steps <= 0)
        return 0;

    avr_decoded_t *cache = cpu->decode_cache;
    avr_decoded_t *d;
    int steps = max_steps;
    uint64_t cycles = cpu->cycles;
    uint64_t start = cycles;
    uint32_t flash_words = cpu->flash_size / 2;

    /* Hot locals — avoid struct reads in inner loop */
    uint16_t pc = cpu->pc;
    uint16_t pa = cpu->periph_accum;
    uint8_t state = cpu->state;

#if USE_THREADED
    static const void * const htab[AVR_OP_COUNT] = {
        [AVR_OP_ADD]   = &&AVR_OP_ADD,   [AVR_OP_ADC]   = &&AVR_OP_ADC,
        [AVR_OP_SUB]   = &&AVR_OP_SUB,   [AVR_OP_SBC]   = &&AVR_OP_SBC,
        [AVR_OP_CP]    = &&AVR_OP_CP,    [AVR_OP_CPC]   = &&AVR_OP_CPC,
        [AVR_OP_CPSE]  = &&AVR_OP_CPSE,
        [AVR_OP_SUBI]  = &&AVR_OP_SUBI,  [AVR_OP_SBCI]  = &&AVR_OP_SBCI,
        [AVR_OP_CPI]   = &&AVR_OP_CPI,   [AVR_OP_LDI]   = &&AVR_OP_LDI,
        [AVR_OP_AND]   = &&AVR_OP_AND,   [AVR_OP_OR]    = &&AVR_OP_OR,
        [AVR_OP_EOR]   = &&AVR_OP_EOR,   [AVR_OP_ANDI]  = &&AVR_OP_ANDI,
        [AVR_OP_ORI]   = &&AVR_OP_ORI,
        [AVR_OP_COM]   = &&AVR_OP_COM,   [AVR_OP_NEG]   = &&AVR_OP_NEG,
        [AVR_OP_SWAP]  = &&AVR_OP_SWAP,
        [AVR_OP_INC]   = &&AVR_OP_INC,   [AVR_OP_DEC]   = &&AVR_OP_DEC,
        [AVR_OP_ASR]   = &&AVR_OP_ASR,   [AVR_OP_LSR]   = &&AVR_OP_LSR,
        [AVR_OP_ROR]   = &&AVR_OP_ROR,
        [AVR_OP_MOV]   = &&AVR_OP_MOV,   [AVR_OP_MOVW]  = &&AVR_OP_MOVW,
        [AVR_OP_ADIW]  = &&AVR_OP_ADIW,  [AVR_OP_SBIW]  = &&AVR_OP_SBIW,
        [AVR_OP_MUL]   = &&AVR_OP_MUL,   [AVR_OP_MULS]  = &&AVR_OP_MULS,
        [AVR_OP_MULSU] = &&AVR_OP_MULSU,
        [AVR_OP_FMUL]  = &&AVR_OP_FMUL,  [AVR_OP_FMULS] = &&AVR_OP_FMULS,
        [AVR_OP_FMULSU]= &&AVR_OP_FMULSU,
        [AVR_OP_IN]    = &&AVR_OP_IN,    [AVR_OP_OUT]   = &&AVR_OP_OUT,
        [AVR_OP_CBI]   = &&AVR_OP_CBI,   [AVR_OP_SBI]   = &&AVR_OP_SBI,
        [AVR_OP_SBIC]  = &&AVR_OP_SBIC,  [AVR_OP_SBIS]  = &&AVR_OP_SBIS,
        [AVR_OP_RJMP]  = &&AVR_OP_RJMP,  [AVR_OP_RCALL] = &&AVR_OP_RCALL,
        [AVR_OP_JMP]   = &&AVR_OP_JMP,   [AVR_OP_CALL]  = &&AVR_OP_CALL,
        [AVR_OP_IJMP]  = &&AVR_OP_IJMP,  [AVR_OP_ICALL] = &&AVR_OP_ICALL,
        [AVR_OP_RET]   = &&AVR_OP_RET,   [AVR_OP_RETI]  = &&AVR_OP_RETI,
        [AVR_OP_BRBS]  = &&AVR_OP_BRBS,  [AVR_OP_BRBC]  = &&AVR_OP_BRBC,
        [AVR_OP_SBRC]  = &&AVR_OP_SBRC,  [AVR_OP_SBRS]  = &&AVR_OP_SBRS,
        [AVR_OP_BSET]  = &&AVR_OP_BSET,  [AVR_OP_BCLR]  = &&AVR_OP_BCLR,
        [AVR_OP_BST]   = &&AVR_OP_BST,   [AVR_OP_BLD]   = &&AVR_OP_BLD,
        [AVR_OP_LDS]   = &&AVR_OP_LDS,
        [AVR_OP_LD_X]  = &&AVR_OP_LD_X,  [AVR_OP_LD_XP] = &&AVR_OP_LD_XP,
        [AVR_OP_LD_MX] = &&AVR_OP_LD_MX,
        [AVR_OP_LD_YP] = &&AVR_OP_LD_YP, [AVR_OP_LD_MY] = &&AVR_OP_LD_MY,
        [AVR_OP_LDD_Y] = &&AVR_OP_LDD_Y,
        [AVR_OP_LD_ZP] = &&AVR_OP_LD_ZP, [AVR_OP_LD_MZ] = &&AVR_OP_LD_MZ,
        [AVR_OP_LDD_Z] = &&AVR_OP_LDD_Z,
        [AVR_OP_STS]   = &&AVR_OP_STS,
        [AVR_OP_ST_X]  = &&AVR_OP_ST_X,  [AVR_OP_ST_XP] = &&AVR_OP_ST_XP,
        [AVR_OP_ST_MX] = &&AVR_OP_ST_MX,
        [AVR_OP_ST_YP] = &&AVR_OP_ST_YP, [AVR_OP_ST_MY] = &&AVR_OP_ST_MY,
        [AVR_OP_STD_Y] = &&AVR_OP_STD_Y,
        [AVR_OP_ST_ZP] = &&AVR_OP_ST_ZP, [AVR_OP_ST_MZ] = &&AVR_OP_ST_MZ,
        [AVR_OP_STD_Z] = &&AVR_OP_STD_Z,
        [AVR_OP_PUSH]  = &&AVR_OP_PUSH,  [AVR_OP_POP]   = &&AVR_OP_POP,
        [AVR_OP_LPM_R0]= &&AVR_OP_LPM_R0,[AVR_OP_LPM_RD]= &&AVR_OP_LPM_RD,
        [AVR_OP_LPM_RDP]=&&AVR_OP_LPM_RDP,
        [AVR_OP_NOP]   = &&AVR_OP_NOP,   [AVR_OP_SLEEP] = &&AVR_OP_SLEEP,
        [AVR_OP_WDR]   = &&AVR_OP_WDR,   [AVR_OP_BREAK] = &&AVR_OP_BREAK,
        [AVR_OP_SPM]   = &&AVR_OP_SPM,
        [AVR_OP_FAULT] = &&AVR_OP_FAULT,  [AVR_OP_DATA]  = &&AVR_OP_NOP,
    };
    /* Initial dispatch */
    if (pc >= flash_words) goto done;
    d = &cache[pc];
    goto *htab[d->op];
#else
    while (steps > 0 && state == AVR_STATE_RUNNING) {
        if (pc >= flash_words) { state = AVR_STATE_HALTED; cpu->state = state; break; }
        d = &cache[pc];
        switch (d->op) {
#endif

    /* ================================================================ */
    /*  Handler bodies — written once, used by both dispatch modes      */
    /* ================================================================ */

    /* ---- Arithmetic (reg-reg) ---- */

    H(AVR_OP_ADD) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t R = Rd + Rr;
        AVR_R(cpu, d->a) = R;
        flags_add(cpu, Rd, Rr, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_ADC) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
        uint8_t R = Rd + Rr + C;
        AVR_R(cpu, d->a) = R;
        flags_add(cpu, Rd, Rr, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_SUB) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t R = Rd - Rr;
        AVR_R(cpu, d->a) = R;
        flags_sub(cpu, Rd, Rr, R, 0);
        pc++; NEXT(1);
    }

    H(AVR_OP_SBC) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
        uint8_t R = Rd - Rr - C;
        AVR_R(cpu, d->a) = R;
        flags_sub(cpu, Rd, Rr, R, 1);
        pc++; NEXT(1);
    }

    H(AVR_OP_CP) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t R = Rd - Rr;
        flags_sub(cpu, Rd, Rr, R, 0);
        pc++; NEXT(1);
    }

    H(AVR_OP_CPC) {
        uint8_t Rd = AVR_R(cpu, d->a), Rr = AVR_R(cpu, (uint8_t)d->b);
        uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
        uint8_t R = Rd - Rr - C;
        flags_sub(cpu, Rd, Rr, R, 1);
        pc++; NEXT(1);
    }

    H(AVR_OP_CPSE) {
        if (AVR_R(cpu, d->a) == AVR_R(cpu, (uint8_t)d->b)) {
            uint16_t next_pc = pc + 1;
            int w = (next_pc < flash_words && avr_is_32bit(cache[next_pc].op)) ? 2 : 1;
            pc = next_pc + w;
            NEXT(1 + w);
        }
        pc++; NEXT(1);
    }

    /* ---- Arithmetic (reg-imm) ---- */

    H(AVR_OP_SUBI) {
        uint8_t Rd = AVR_R(cpu, d->a), K = (uint8_t)d->b;
        uint8_t R = Rd - K;
        AVR_R(cpu, d->a) = R;
        flags_sub(cpu, Rd, K, R, 0);
        pc++; NEXT(1);
    }

    H(AVR_OP_SBCI) {
        uint8_t Rd = AVR_R(cpu, d->a), K = (uint8_t)d->b;
        uint8_t C = (cpu->sreg & SREG_C) ? 1 : 0;
        uint8_t R = Rd - K - C;
        AVR_R(cpu, d->a) = R;
        flags_sub(cpu, Rd, K, R, 1);
        pc++; NEXT(1);
    }

    H(AVR_OP_CPI) {
        uint8_t Rd = AVR_R(cpu, d->a), K = (uint8_t)d->b;
        uint8_t R = Rd - K;
        flags_sub(cpu, Rd, K, R, 0);
        pc++; NEXT(1);
    }

    H(AVR_OP_LDI) {
        AVR_R(cpu, d->a) = (uint8_t)d->b;
        pc++; NEXT(1);
    }

    /* ---- Logic ---- */

    H(AVR_OP_AND) {
        uint8_t R = AVR_R(cpu, d->a) & AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_OR) {
        uint8_t R = AVR_R(cpu, d->a) | AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_EOR) {
        uint8_t R = AVR_R(cpu, d->a) ^ AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_ANDI) {
        uint8_t R = AVR_R(cpu, d->a) & (uint8_t)d->b;
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_ORI) {
        uint8_t R = AVR_R(cpu, d->a) | (uint8_t)d->b;
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        pc++; NEXT(1);
    }

    /* ---- Single-register ---- */

    H(AVR_OP_COM) {
        uint8_t R = 0xFF - AVR_R(cpu, d->a);
        AVR_R(cpu, d->a) = R;
        flags_logic(cpu, R);
        cpu->sreg |= SREG_C;
        pc++; NEXT(1);
    }

    H(AVR_OP_NEG) {
        uint8_t Rd = AVR_R(cpu, d->a);
        uint8_t R = 0 - Rd;
        AVR_R(cpu, d->a) = R;
        flags_sub(cpu, 0, Rd, R, 0);
        set_flag(cpu, SREG_C, R != 0);
        pc++; NEXT(1);
    }

    H(AVR_OP_SWAP) {
        uint8_t v = AVR_R(cpu, d->a);
        AVR_R(cpu, d->a) = ((v >> 4) & 0x0F) | ((v << 4) & 0xF0);
        pc++; NEXT(1);
    }

    H(AVR_OP_INC) {
        uint8_t R = AVR_R(cpu, d->a) + 1;
        AVR_R(cpu, d->a) = R;
        flags_inc(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_DEC) {
        uint8_t R = AVR_R(cpu, d->a) - 1;
        AVR_R(cpu, d->a) = R;
        flags_dec(cpu, R);
        pc++; NEXT(1);
    }

    H(AVR_OP_ASR) {
        uint8_t Rd = AVR_R(cpu, d->a);
        uint8_t R = (Rd >> 1) | (Rd & 0x80);
        AVR_R(cpu, d->a) = R;
        set_flag(cpu, SREG_C, Rd & 1);
        set_flag(cpu, SREG_N, (R >> 7) & 1);
        set_flag(cpu, SREG_Z, R == 0);
        set_flag(cpu, SREG_V, (Rd & 1) ^ ((R >> 7) & 1));
        set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
        pc++; NEXT(1);
    }

    H(AVR_OP_LSR) {
        uint8_t Rd = AVR_R(cpu, d->a);
        uint8_t R = Rd >> 1;
        AVR_R(cpu, d->a) = R;
        set_flag(cpu, SREG_C, Rd & 1);
        set_flag(cpu, SREG_N, 0);
        set_flag(cpu, SREG_Z, R == 0);
        set_flag(cpu, SREG_V, Rd & 1);
        set_flag(cpu, SREG_S, Rd & 1);
        pc++; NEXT(1);
    }

    H(AVR_OP_ROR) {
        uint8_t Rd = AVR_R(cpu, d->a);
        uint8_t C = (cpu->sreg & SREG_C) ? 0x80 : 0;
        uint8_t R = (Rd >> 1) | C;
        AVR_R(cpu, d->a) = R;
        set_flag(cpu, SREG_C, Rd & 1);
        set_flag(cpu, SREG_N, (R >> 7) & 1);
        set_flag(cpu, SREG_Z, R == 0);
        set_flag(cpu, SREG_V, ((R >> 7) & 1) ^ (Rd & 1));
        set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
        pc++; NEXT(1);
    }

    /* ---- Data transfer ---- */

    H(AVR_OP_MOV) {
        AVR_R(cpu, d->a) = AVR_R(cpu, (uint8_t)d->b);
        pc++; NEXT(1);
    }

    H(AVR_OP_MOVW) {
        uint8_t dr = d->a, sr = (uint8_t)d->b;
        AVR_R(cpu, dr)   = AVR_R(cpu, sr);
        AVR_R(cpu, dr+1) = AVR_R(cpu, sr+1);
        pc++; NEXT(1);
    }

    /* ---- Word arithmetic ---- */

    H(AVR_OP_ADIW) {
        uint8_t dr = d->a;
        uint16_t Rdw = AVR_REGW(cpu, dr);
        uint16_t R = Rdw + d->b;
        AVR_SET_REGW(cpu, dr, R);
        set_flag(cpu, SREG_C, (~R & Rdw) >> 15);
        set_flag(cpu, SREG_V, (R & ~Rdw) >> 15);
        set_flag(cpu, SREG_N, (R >> 15) & 1);
        set_flag(cpu, SREG_Z, R == 0);
        set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_SBIW) {
        uint8_t dr = d->a;
        uint16_t Rdw = AVR_REGW(cpu, dr);
        uint16_t R = Rdw - d->b;
        AVR_SET_REGW(cpu, dr, R);
        set_flag(cpu, SREG_C, (~Rdw & R) >> 15);
        set_flag(cpu, SREG_V, (Rdw & ~R) >> 15);
        set_flag(cpu, SREG_N, (R >> 15) & 1);
        set_flag(cpu, SREG_Z, R == 0);
        set_flag(cpu, SREG_S, ((cpu->sreg >> 2) ^ (cpu->sreg >> 3)) & 1);
        pc++; NEXT(2);
    }

    /* ---- Multiply ---- */

    H(AVR_OP_MUL) {
        uint16_t res = (uint16_t)AVR_R(cpu, d->a) * (uint16_t)AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        set_flag(cpu, SREG_Z, res == 0);
        pc++; NEXT(2);
    }

    H(AVR_OP_MULS) {
        int16_t res = (int8_t)AVR_R(cpu, d->a) * (int8_t)AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        set_flag(cpu, SREG_Z, res == 0);
        pc++; NEXT(2);
    }

    H(AVR_OP_MULSU) {
        int16_t res = (int8_t)AVR_R(cpu, d->a) * (uint8_t)AVR_R(cpu, (uint8_t)d->b);
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        set_flag(cpu, SREG_Z, res == 0);
        pc++; NEXT(2);
    }

    H(AVR_OP_FMUL) {
        uint16_t res = (uint16_t)AVR_R(cpu, d->a) * (uint16_t)AVR_R(cpu, (uint8_t)d->b);
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        res <<= 1;
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
        pc++; NEXT(2);
    }

    H(AVR_OP_FMULS) {
        int16_t res = (int8_t)AVR_R(cpu, d->a) * (int8_t)AVR_R(cpu, (uint8_t)d->b);
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        res <<= 1;
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
        pc++; NEXT(2);
    }

    H(AVR_OP_FMULSU) {
        int16_t res = (int8_t)AVR_R(cpu, d->a) * (uint8_t)AVR_R(cpu, (uint8_t)d->b);
        set_flag(cpu, SREG_C, (res >> 15) & 1);
        res <<= 1;
        AVR_R(cpu, 0) = res & 0xFF;
        AVR_R(cpu, 1) = (res >> 8) & 0xFF;
        set_flag(cpu, SREG_Z, (res & 0xFFFF) == 0);
        pc++; NEXT(2);
    }

    /* ---- IO ---- */

    H(AVR_OP_IN) {
        AVR_R(cpu, d->a) = avr_io_read(cpu, (uint8_t)d->b);
        pc++; NEXT(1);
    }

    H(AVR_OP_OUT) {
        avr_io_write(cpu, (uint8_t)d->b, AVR_R(cpu, d->a));
        pc++; NEXT(1);
    }

    H(AVR_OP_CBI) {
        uint8_t val = avr_io_read(cpu, d->a) & ~(uint8_t)d->b;
        avr_io_write(cpu, d->a, val);
        pc++; NEXT(2);
    }

    H(AVR_OP_SBI) {
        uint8_t val = avr_io_read(cpu, d->a) | (uint8_t)d->b;
        avr_io_write(cpu, d->a, val);
        pc++; NEXT(2);
    }

    H(AVR_OP_SBIC) {
        if (!(avr_io_read(cpu, d->a) & (uint8_t)d->b)) {
            uint16_t next_pc = pc + 1;
            int w = (next_pc < flash_words && avr_is_32bit(cache[next_pc].op)) ? 2 : 1;
            pc = next_pc + w;
            NEXT(1 + w);
        }
        pc++; NEXT(1);
    }

    H(AVR_OP_SBIS) {
        if (avr_io_read(cpu, d->a) & (uint8_t)d->b) {
            uint16_t next_pc = pc + 1;
            int w = (next_pc < flash_words && avr_is_32bit(cache[next_pc].op)) ? 2 : 1;
            pc = next_pc + w;
            NEXT(1 + w);
        }
        pc++; NEXT(1);
    }

    /* ---- Branch ---- */

    H(AVR_OP_RJMP) {
        pc = d->b;
        NEXT(2);
    }

    H(AVR_OP_RCALL) {
        uint16_t ret = pc + 1;
        avr_push(cpu, ret & 0xFF);
        avr_push(cpu, (ret >> 8) & 0xFF);
        pc = d->b;
        NEXT(3);
    }

    H(AVR_OP_JMP) {
        pc = d->b;
        NEXT(3);
    }

    H(AVR_OP_CALL) {
        uint16_t ret = pc + 2;  /* skip 2-word instruction */
        avr_push(cpu, ret & 0xFF);
        avr_push(cpu, (ret >> 8) & 0xFF);
        pc = d->b;
        NEXT(4);
    }

    H(AVR_OP_IJMP) {
        pc = AVR_Z(cpu);
        NEXT(2);
    }

    H(AVR_OP_ICALL) {
        uint16_t ret = pc + 1;
        avr_push(cpu, ret & 0xFF);
        avr_push(cpu, (ret >> 8) & 0xFF);
        pc = AVR_Z(cpu);
        NEXT(3);
    }

    H(AVR_OP_RET) {
        uint8_t pch = avr_pop(cpu);
        uint8_t pcl = avr_pop(cpu);
        pc = ((uint16_t)pch << 8) | pcl;
        NEXT(4);
    }

    H(AVR_OP_RETI) {
        uint8_t pch = avr_pop(cpu);
        uint8_t pcl = avr_pop(cpu);
        pc = ((uint16_t)pch << 8) | pcl;
        cpu->sreg |= SREG_I;
        NEXT(4);
    }

    H(AVR_OP_BRBS) {
        if (cpu->sreg & d->a) {
            pc = d->b;
            NEXT(2);
        }
        pc++; NEXT(1);
    }

    H(AVR_OP_BRBC) {
        if (!(cpu->sreg & d->a)) {
            pc = d->b;
            NEXT(2);
        }
        pc++; NEXT(1);
    }

    /* ---- Skip ---- */

    H(AVR_OP_SBRC) {
        if (!(AVR_R(cpu, d->a) & (uint8_t)d->b)) {
            uint16_t next_pc = pc + 1;
            int w = (next_pc < flash_words && avr_is_32bit(cache[next_pc].op)) ? 2 : 1;
            pc = next_pc + w;
            NEXT(1 + w);
        }
        pc++; NEXT(1);
    }

    H(AVR_OP_SBRS) {
        if (AVR_R(cpu, d->a) & (uint8_t)d->b) {
            uint16_t next_pc = pc + 1;
            int w = (next_pc < flash_words && avr_is_32bit(cache[next_pc].op)) ? 2 : 1;
            pc = next_pc + w;
            NEXT(1 + w);
        }
        pc++; NEXT(1);
    }

    /* ---- Bit operations ---- */

    H(AVR_OP_BSET) {
        cpu->sreg |= d->a;
        pc++; NEXT(1);
    }

    H(AVR_OP_BCLR) {
        cpu->sreg &= ~d->a;
        pc++; NEXT(1);
    }

    H(AVR_OP_BST) {
        set_flag(cpu, SREG_T, AVR_R(cpu, d->a) & (uint8_t)d->b);
        pc++; NEXT(1);
    }

    H(AVR_OP_BLD) {
        if (cpu->sreg & SREG_T)
            AVR_R(cpu, d->a) |= (uint8_t)d->b;
        else
            AVR_R(cpu, d->a) &= ~(uint8_t)d->b;
        pc++; NEXT(1);
    }

    /* ---- Load ---- */

    H(AVR_OP_LDS) {
        AVR_R(cpu, d->a) = avr_data_read(cpu, d->b);
        pc += 2;
        NEXT(2);
    }

    H(AVR_OP_LD_X) {
        AVR_R(cpu, d->a) = avr_data_read(cpu, AVR_X(cpu));
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_XP) {
        uint16_t x = AVR_X(cpu);
        AVR_R(cpu, d->a) = avr_data_read(cpu, x);
        AVR_SET_X(cpu, x + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_MX) {
        uint16_t x = AVR_X(cpu) - 1;
        AVR_SET_X(cpu, x);
        AVR_R(cpu, d->a) = avr_data_read(cpu, x);
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_YP) {
        uint16_t y = AVR_Y(cpu);
        AVR_R(cpu, d->a) = avr_data_read(cpu, y);
        AVR_SET_Y(cpu, y + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_MY) {
        uint16_t y = AVR_Y(cpu) - 1;
        AVR_SET_Y(cpu, y);
        AVR_R(cpu, d->a) = avr_data_read(cpu, y);
        pc++; NEXT(2);
    }

    H(AVR_OP_LDD_Y) {
        AVR_R(cpu, d->a) = avr_data_read(cpu, AVR_Y(cpu) + d->b);
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_ZP) {
        uint16_t z = AVR_Z(cpu);
        AVR_R(cpu, d->a) = avr_data_read(cpu, z);
        AVR_SET_Z(cpu, z + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_LD_MZ) {
        uint16_t z = AVR_Z(cpu) - 1;
        AVR_SET_Z(cpu, z);
        AVR_R(cpu, d->a) = avr_data_read(cpu, z);
        pc++; NEXT(2);
    }

    H(AVR_OP_LDD_Z) {
        AVR_R(cpu, d->a) = avr_data_read(cpu, AVR_Z(cpu) + d->b);
        pc++; NEXT(2);
    }

    /* ---- Store ---- */

    H(AVR_OP_STS) {
        avr_data_write(cpu, d->b, AVR_R(cpu, d->a));
        pc += 2;
        NEXT(2);
    }

    H(AVR_OP_ST_X) {
        avr_data_write(cpu, AVR_X(cpu), AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_XP) {
        uint16_t x = AVR_X(cpu);
        avr_data_write(cpu, x, AVR_R(cpu, d->a));
        AVR_SET_X(cpu, x + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_MX) {
        uint16_t x = AVR_X(cpu) - 1;
        AVR_SET_X(cpu, x);
        avr_data_write(cpu, x, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_YP) {
        uint16_t y = AVR_Y(cpu);
        avr_data_write(cpu, y, AVR_R(cpu, d->a));
        AVR_SET_Y(cpu, y + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_MY) {
        uint16_t y = AVR_Y(cpu) - 1;
        AVR_SET_Y(cpu, y);
        avr_data_write(cpu, y, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_STD_Y) {
        avr_data_write(cpu, AVR_Y(cpu) + d->b, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_ZP) {
        uint16_t z = AVR_Z(cpu);
        avr_data_write(cpu, z, AVR_R(cpu, d->a));
        AVR_SET_Z(cpu, z + 1);
        pc++; NEXT(2);
    }

    H(AVR_OP_ST_MZ) {
        uint16_t z = AVR_Z(cpu) - 1;
        AVR_SET_Z(cpu, z);
        avr_data_write(cpu, z, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_STD_Z) {
        avr_data_write(cpu, AVR_Z(cpu) + d->b, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    /* ---- Stack ---- */

    H(AVR_OP_PUSH) {
        avr_push(cpu, AVR_R(cpu, d->a));
        pc++; NEXT(2);
    }

    H(AVR_OP_POP) {
        AVR_R(cpu, d->a) = avr_pop(cpu);
        pc++; NEXT(2);
    }

    /* ---- Program memory ---- */

    H(AVR_OP_LPM_R0) {
        AVR_R(cpu, 0) = avr_flash_read_byte(cpu, AVR_Z(cpu));
        pc++; NEXT(3);
    }

    H(AVR_OP_LPM_RD) {
        AVR_R(cpu, d->a) = avr_flash_read_byte(cpu, AVR_Z(cpu));
        pc++; NEXT(3);
    }

    H(AVR_OP_LPM_RDP) {
        uint16_t z = AVR_Z(cpu);
        AVR_R(cpu, d->a) = avr_flash_read_byte(cpu, z);
        AVR_SET_Z(cpu, z + 1);
        pc++; NEXT(3);
    }

    /* ---- Misc ---- */

    H(AVR_OP_NOP) {
        pc++; NEXT(1);
    }

    H(AVR_OP_SLEEP) {
        state = AVR_STATE_SLEEPING;
        cpu->state = state;
        pc++; NEXT(1);
    }

    H(AVR_OP_WDR) {
        pc++; NEXT(1);
    }

    H(AVR_OP_BREAK) {
        state = AVR_STATE_BREAK;
        cpu->state = state;
        pc++; NEXT(1);
    }

    H(AVR_OP_SPM) {
        pc++; NEXT(1);
    }

    H(AVR_OP_FAULT) {
        state = AVR_STATE_HALTED;
        cpu->state = state;
        goto done;
    }

#if !USE_THREADED
        default:
            state = AVR_STATE_HALTED;
            cpu->state = state;
            goto done;
        } /* switch */
    } /* while */
#endif

done:
    cpu->pc = pc;
    cpu->periph_accum = pa;
    cpu->cycles = cycles;
    return (uint32_t)(cycles - start);
}

#undef H
#undef NEXT
#undef USE_THREADED
