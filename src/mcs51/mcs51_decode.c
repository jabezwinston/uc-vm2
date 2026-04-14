/*
 * ucvm - MCS-51 (8051) instruction decoder and executor
 *
 * Complete implementation of all 256 opcodes using a function-pointer
 * dispatch table.  Each handler fetches its own operands from code memory,
 * executes the operation, updates flags as required, advances PC, and
 * returns the cycle count.
 */
#include "mcs51_cpu.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Handler function type                                              */
/* ------------------------------------------------------------------ */
typedef uint8_t (*mcs51_op_fn)(mcs51_cpu_t *cpu);

/* ------------------------------------------------------------------ */
/*  Stack helpers (defined in mcs51_cpu.c)                             */
/* ------------------------------------------------------------------ */
extern void    mcs51_push(mcs51_cpu_t *cpu, uint8_t val);
extern uint8_t mcs51_pop(mcs51_cpu_t *cpu);

/* ------------------------------------------------------------------ */
/*  Quick code-fetch helpers                                           */
/* ------------------------------------------------------------------ */
static inline uint8_t fetch1(mcs51_cpu_t *cpu)
{
    return mcs51_code_read(cpu, cpu->pc + 1);
}

static inline uint8_t fetch2(mcs51_cpu_t *cpu)
{
    return mcs51_code_read(cpu, cpu->pc + 2);
}

/* ------------------------------------------------------------------ */
/*  PSW helpers                                                        */
/* ------------------------------------------------------------------ */
static inline void set_cy(mcs51_cpu_t *cpu, uint8_t v)
{
    if (v) PSW(cpu) |= PSW_CY; else PSW(cpu) &= ~PSW_CY;
}

static inline void set_ac(mcs51_cpu_t *cpu, uint8_t v)
{
    if (v) PSW(cpu) |= PSW_AC; else PSW(cpu) &= ~PSW_AC;
}

static inline void set_ov(mcs51_cpu_t *cpu, uint8_t v)
{
    if (v) PSW(cpu) |= PSW_OV; else PSW(cpu) &= ~PSW_OV;
}

static inline uint8_t get_cy(mcs51_cpu_t *cpu)
{
    return (PSW(cpu) & PSW_CY) ? 1 : 0;
}

/* ADD with full flag computation.  Returns result. */
static inline uint8_t do_add(mcs51_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t carry_in)
{
    uint16_t sum  = (uint16_t)a + b + carry_in;
    uint8_t  low  = (a & 0x0F) + (b & 0x0F) + carry_in;
    uint8_t  c6   = ((a & 0x7F) + (b & 0x7F) + carry_in) >> 7; /* carry out of bit 6 */
    uint8_t  c7   = (sum >> 8) & 1;                              /* carry out of bit 7 */

    set_cy(cpu, c7);
    set_ac(cpu, (low > 0x0F) ? 1 : 0);
    set_ov(cpu, c6 ^ c7);

    return (uint8_t)sum;
}

/* SUBB with full flag computation.  Returns result. */
static inline uint8_t do_subb(mcs51_cpu_t *cpu, uint8_t a, uint8_t b, uint8_t borrow_in)
{
    uint16_t diff = (uint16_t)a - b - borrow_in;
    uint8_t  c7   = (diff >> 8) & 1;                             /* borrow from bit 8 */
    int      low  = (int)(a & 0x0F) - (b & 0x0F) - borrow_in;
    uint8_t  ac   = (low < 0) ? 1 : 0;
    /* OV: borrow into bit 7 XOR borrow from bit 7 */
    uint8_t  c6   = ((int)(a & 0x7F) - (b & 0x7F) - borrow_in) < 0 ? 1 : 0;

    set_cy(cpu, c7);
    set_ac(cpu, ac);
    set_ov(cpu, c6 ^ c7);

    return (uint8_t)diff;
}

/* ------------------------------------------------------------------ */
/*  0x00  NOP                                                          */
/* ------------------------------------------------------------------ */
static uint8_t op_nop(mcs51_cpu_t *cpu)
{
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  AJMP  (0x01,0x21,0x41,0x61,0x81,0xA1,0xC1,0xE1)                  */
/* ------------------------------------------------------------------ */
static uint8_t op_ajmp(mcs51_cpu_t *cpu)
{
    uint8_t opcode = mcs51_code_read(cpu, cpu->pc);
    uint8_t arg    = fetch1(cpu);
    uint16_t base  = cpu->pc + 2;
    cpu->pc = (base & 0xF800) | ((uint16_t)(opcode & 0xE0) << 3) | arg;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x02  LJMP addr16                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_ljmp(mcs51_cpu_t *cpu)
{
    uint16_t hi = fetch1(cpu);
    uint16_t lo = fetch2(cpu);
    cpu->pc = (hi << 8) | lo;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x03  RR A                                                         */
/* ------------------------------------------------------------------ */
static uint8_t op_rr_a(mcs51_cpu_t *cpu)
{
    uint8_t a = ACC(cpu);
    ACC(cpu) = (a >> 1) | (a << 7);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x04  INC A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_inc_a(mcs51_cpu_t *cpu)
{
    ACC(cpu)++;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x05  INC direct                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_inc_direct(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t val  = mcs51_direct_read(cpu, addr) + 1;
    mcs51_direct_write(cpu, addr, val);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x06-0x07  INC @Ri                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_inc_ind_r0(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 0);
    mcs51_indirect_write(cpu, addr, mcs51_indirect_read(cpu, addr) + 1);
    cpu->pc += 1;
    return 1;
}

static uint8_t op_inc_ind_r1(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 1);
    mcs51_indirect_write(cpu, addr, mcs51_indirect_read(cpu, addr) + 1);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x08-0x0F  INC Rn                                                  */
/* ------------------------------------------------------------------ */
#define DEF_INC_RN(n) \
static uint8_t op_inc_r##n(mcs51_cpu_t *cpu) { \
    REG(cpu, n)++; \
    cpu->pc += 1; \
    return 1; \
}
DEF_INC_RN(0) DEF_INC_RN(1) DEF_INC_RN(2) DEF_INC_RN(3)
DEF_INC_RN(4) DEF_INC_RN(5) DEF_INC_RN(6) DEF_INC_RN(7)

/* ------------------------------------------------------------------ */
/*  0x10  JBC bit,rel                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_jbc(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    cpu->pc += 3;
    if (mcs51_bit_read(cpu, bit)) {
        mcs51_bit_write(cpu, bit, 0);
        cpu->pc = cpu->pc + rel;
    }
    return 2;
}

/* ------------------------------------------------------------------ */
/*  ACALL  (0x11,0x31,0x51,0x71,0x91,0xB1,0xD1,0xF1)                 */
/* ------------------------------------------------------------------ */
static uint8_t op_acall(mcs51_cpu_t *cpu)
{
    uint8_t  opcode = mcs51_code_read(cpu, cpu->pc);
    uint8_t  arg    = fetch1(cpu);
    uint16_t ret    = cpu->pc + 2;

    mcs51_push(cpu, ret & 0xFF);
    mcs51_push(cpu, (ret >> 8) & 0xFF);

    cpu->pc = (ret & 0xF800) | ((uint16_t)(opcode & 0xE0) << 3) | arg;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x12  LCALL addr16                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_lcall(mcs51_cpu_t *cpu)
{
    uint16_t hi  = fetch1(cpu);
    uint16_t lo  = fetch2(cpu);
    uint16_t ret = cpu->pc + 3;

    mcs51_push(cpu, ret & 0xFF);
    mcs51_push(cpu, (ret >> 8) & 0xFF);

    cpu->pc = (hi << 8) | lo;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x13  RRC A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_rrc_a(mcs51_cpu_t *cpu)
{
    uint8_t a  = ACC(cpu);
    uint8_t cy = get_cy(cpu);
    set_cy(cpu, a & 1);
    ACC(cpu) = (a >> 1) | (cy << 7);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x14  DEC A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_dec_a(mcs51_cpu_t *cpu)
{
    ACC(cpu)--;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x15  DEC direct                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_dec_direct(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t val  = mcs51_direct_read(cpu, addr) - 1;
    mcs51_direct_write(cpu, addr, val);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x16-0x17  DEC @Ri                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_dec_ind_r0(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 0);
    mcs51_indirect_write(cpu, addr, mcs51_indirect_read(cpu, addr) - 1);
    cpu->pc += 1;
    return 1;
}

static uint8_t op_dec_ind_r1(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 1);
    mcs51_indirect_write(cpu, addr, mcs51_indirect_read(cpu, addr) - 1);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x18-0x1F  DEC Rn                                                  */
/* ------------------------------------------------------------------ */
#define DEF_DEC_RN(n) \
static uint8_t op_dec_r##n(mcs51_cpu_t *cpu) { \
    REG(cpu, n)--; \
    cpu->pc += 1; \
    return 1; \
}
DEF_DEC_RN(0) DEF_DEC_RN(1) DEF_DEC_RN(2) DEF_DEC_RN(3)
DEF_DEC_RN(4) DEF_DEC_RN(5) DEF_DEC_RN(6) DEF_DEC_RN(7)

/* ------------------------------------------------------------------ */
/*  0x20  JB bit,rel                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_jb(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    cpu->pc += 3;
    if (mcs51_bit_read(cpu, bit))
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x22  RET                                                          */
/* ------------------------------------------------------------------ */
static uint8_t op_ret(mcs51_cpu_t *cpu)
{
    uint8_t hi = mcs51_pop(cpu);
    uint8_t lo = mcs51_pop(cpu);
    cpu->pc = ((uint16_t)hi << 8) | lo;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x23  RL A                                                         */
/* ------------------------------------------------------------------ */
static uint8_t op_rl_a(mcs51_cpu_t *cpu)
{
    uint8_t a = ACC(cpu);
    ACC(cpu) = (a << 1) | (a >> 7);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x24  ADD A,#imm                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_add_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), fetch1(cpu), 0);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x25  ADD A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_add_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_direct_read(cpu, fetch1(cpu)), 0);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x26-0x27  ADD A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_add_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 0)), 0);
    cpu->pc += 1;
    return 1;
}

static uint8_t op_add_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 1)), 0);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x28-0x2F  ADD A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_ADD_RN(n) \
static uint8_t op_add_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) = do_add(cpu, ACC(cpu), REG(cpu, n), 0); \
    cpu->pc += 1; \
    return 1; \
}
DEF_ADD_RN(0) DEF_ADD_RN(1) DEF_ADD_RN(2) DEF_ADD_RN(3)
DEF_ADD_RN(4) DEF_ADD_RN(5) DEF_ADD_RN(6) DEF_ADD_RN(7)

/* ------------------------------------------------------------------ */
/*  0x30  JNB bit,rel                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_jnb(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    cpu->pc += 3;
    if (!mcs51_bit_read(cpu, bit))
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x32  RETI                                                         */
/* ------------------------------------------------------------------ */
static uint8_t op_reti(mcs51_cpu_t *cpu)
{
    uint8_t hi = mcs51_pop(cpu);
    uint8_t lo = mcs51_pop(cpu);
    cpu->pc = ((uint16_t)hi << 8) | lo;

    /* Clear the current active interrupt level */
    if (cpu->int_active[1])
        cpu->int_active[1] = 0;
    else
        cpu->int_active[0] = 0;

    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x33  RLC A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_rlc_a(mcs51_cpu_t *cpu)
{
    uint8_t a  = ACC(cpu);
    uint8_t cy = get_cy(cpu);
    set_cy(cpu, (a >> 7) & 1);
    ACC(cpu) = (a << 1) | cy;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x34  ADDC A,#imm                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_addc_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), fetch1(cpu), get_cy(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x35  ADDC A,direct                                                */
/* ------------------------------------------------------------------ */
static uint8_t op_addc_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_direct_read(cpu, fetch1(cpu)), get_cy(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x36-0x37  ADDC A,@Ri                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_addc_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 0)), get_cy(cpu));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_addc_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_add(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 1)), get_cy(cpu));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x38-0x3F  ADDC A,Rn                                               */
/* ------------------------------------------------------------------ */
#define DEF_ADDC_RN(n) \
static uint8_t op_addc_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) = do_add(cpu, ACC(cpu), REG(cpu, n), get_cy(cpu)); \
    cpu->pc += 1; \
    return 1; \
}
DEF_ADDC_RN(0) DEF_ADDC_RN(1) DEF_ADDC_RN(2) DEF_ADDC_RN(3)
DEF_ADDC_RN(4) DEF_ADDC_RN(5) DEF_ADDC_RN(6) DEF_ADDC_RN(7)

/* ------------------------------------------------------------------ */
/*  0x40  JC rel                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_jc(mcs51_cpu_t *cpu)
{
    int8_t rel = (int8_t)fetch1(cpu);
    cpu->pc += 2;
    if (get_cy(cpu))
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x42  ORL direct,A                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_direct_a(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) | ACC(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x43  ORL direct,#imm                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_direct_imm(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t imm  = fetch2(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) | imm);
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x44  ORL A,#imm                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) |= fetch1(cpu);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x45  ORL A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) |= mcs51_direct_read(cpu, fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x46-0x47  ORL A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) |= mcs51_indirect_read(cpu, REG(cpu, 0));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_orl_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) |= mcs51_indirect_read(cpu, REG(cpu, 1));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x48-0x4F  ORL A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_ORL_RN(n) \
static uint8_t op_orl_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) |= REG(cpu, n); \
    cpu->pc += 1; \
    return 1; \
}
DEF_ORL_RN(0) DEF_ORL_RN(1) DEF_ORL_RN(2) DEF_ORL_RN(3)
DEF_ORL_RN(4) DEF_ORL_RN(5) DEF_ORL_RN(6) DEF_ORL_RN(7)

/* ------------------------------------------------------------------ */
/*  0x50  JNC rel                                                      */
/* ------------------------------------------------------------------ */
static uint8_t op_jnc(mcs51_cpu_t *cpu)
{
    int8_t rel = (int8_t)fetch1(cpu);
    cpu->pc += 2;
    if (!get_cy(cpu))
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x52  ANL direct,A                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_direct_a(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) & ACC(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x53  ANL direct,#imm                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_direct_imm(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t imm  = fetch2(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) & imm);
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x54  ANL A,#imm                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) &= fetch1(cpu);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x55  ANL A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) &= mcs51_direct_read(cpu, fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x56-0x57  ANL A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) &= mcs51_indirect_read(cpu, REG(cpu, 0));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_anl_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) &= mcs51_indirect_read(cpu, REG(cpu, 1));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x58-0x5F  ANL A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_ANL_RN(n) \
static uint8_t op_anl_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) &= REG(cpu, n); \
    cpu->pc += 1; \
    return 1; \
}
DEF_ANL_RN(0) DEF_ANL_RN(1) DEF_ANL_RN(2) DEF_ANL_RN(3)
DEF_ANL_RN(4) DEF_ANL_RN(5) DEF_ANL_RN(6) DEF_ANL_RN(7)

/* ------------------------------------------------------------------ */
/*  0x60  JZ rel                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_jz(mcs51_cpu_t *cpu)
{
    int8_t rel = (int8_t)fetch1(cpu);
    cpu->pc += 2;
    if (ACC(cpu) == 0)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x62  XRL direct,A                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_xrl_direct_a(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) ^ ACC(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x63  XRL direct,#imm                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_xrl_direct_imm(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t imm  = fetch2(cpu);
    mcs51_direct_write(cpu, addr, mcs51_direct_read(cpu, addr) ^ imm);
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x64  XRL A,#imm                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_xrl_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) ^= fetch1(cpu);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x65  XRL A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_xrl_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) ^= mcs51_direct_read(cpu, fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x66-0x67  XRL A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_xrl_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) ^= mcs51_indirect_read(cpu, REG(cpu, 0));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_xrl_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) ^= mcs51_indirect_read(cpu, REG(cpu, 1));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x68-0x6F  XRL A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_XRL_RN(n) \
static uint8_t op_xrl_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) ^= REG(cpu, n); \
    cpu->pc += 1; \
    return 1; \
}
DEF_XRL_RN(0) DEF_XRL_RN(1) DEF_XRL_RN(2) DEF_XRL_RN(3)
DEF_XRL_RN(4) DEF_XRL_RN(5) DEF_XRL_RN(6) DEF_XRL_RN(7)

/* ------------------------------------------------------------------ */
/*  0x70  JNZ rel                                                      */
/* ------------------------------------------------------------------ */
static uint8_t op_jnz(mcs51_cpu_t *cpu)
{
    int8_t rel = (int8_t)fetch1(cpu);
    cpu->pc += 2;
    if (ACC(cpu) != 0)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x72  ORL C,bit                                                    */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_c_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    if (mcs51_bit_read(cpu, bit))
        PSW(cpu) |= PSW_CY;
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x73  JMP @A+DPTR                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_jmp_a_dptr(mcs51_cpu_t *cpu)
{
    cpu->pc = (uint16_t)(ACC(cpu) + DPTR(cpu));
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x74  MOV A,#imm                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) = fetch1(cpu);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x75  MOV direct,#imm                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_direct_imm(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t imm  = fetch2(cpu);
    mcs51_direct_write(cpu, addr, imm);
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x76-0x77  MOV @Ri,#imm                                            */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_ind_r0_imm(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 0), fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

static uint8_t op_mov_ind_r1_imm(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 1), fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x78-0x7F  MOV Rn,#imm                                             */
/* ------------------------------------------------------------------ */
#define DEF_MOV_RN_IMM(n) \
static uint8_t op_mov_r##n##_imm(mcs51_cpu_t *cpu) { \
    REG(cpu, n) = fetch1(cpu); \
    cpu->pc += 2; \
    return 1; \
}
DEF_MOV_RN_IMM(0) DEF_MOV_RN_IMM(1) DEF_MOV_RN_IMM(2) DEF_MOV_RN_IMM(3)
DEF_MOV_RN_IMM(4) DEF_MOV_RN_IMM(5) DEF_MOV_RN_IMM(6) DEF_MOV_RN_IMM(7)

/* ------------------------------------------------------------------ */
/*  0x80  SJMP rel                                                     */
/* ------------------------------------------------------------------ */
static uint8_t op_sjmp(mcs51_cpu_t *cpu)
{
    int8_t rel = (int8_t)fetch1(cpu);
    cpu->pc += 2;
    cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x82  ANL C,bit                                                    */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_c_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    if (!mcs51_bit_read(cpu, bit))
        PSW(cpu) &= ~PSW_CY;
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x83  MOVC A,@A+PC                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_movc_a_pc(mcs51_cpu_t *cpu)
{
    cpu->pc += 1;
    ACC(cpu) = mcs51_code_read(cpu, (uint16_t)(ACC(cpu) + cpu->pc));
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x84  DIV AB                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_div_ab(mcs51_cpu_t *cpu)
{
    uint8_t a = ACC(cpu);
    uint8_t b = B(cpu);

    set_cy(cpu, 0);

    if (b == 0) {
        set_ov(cpu, 1);
        /* A and B are undefined on divide-by-zero; leave unchanged */
    } else {
        set_ov(cpu, 0);
        ACC(cpu) = a / b;
        B(cpu)   = a % b;
    }
    cpu->pc += 1;
    return 4;
}

/* ------------------------------------------------------------------ */
/*  0x85  MOV direct,direct                                            */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_direct_direct(mcs51_cpu_t *cpu)
{
    uint8_t src = fetch1(cpu);  /* source address */
    uint8_t dst = fetch2(cpu);  /* destination address */
    mcs51_direct_write(cpu, dst, mcs51_direct_read(cpu, src));
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x86-0x87  MOV direct,@Ri                                          */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_direct_ind_r0(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_indirect_read(cpu, REG(cpu, 0)));
    cpu->pc += 2;
    return 2;
}

static uint8_t op_mov_direct_ind_r1(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_indirect_read(cpu, REG(cpu, 1)));
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x88-0x8F  MOV direct,Rn                                           */
/* ------------------------------------------------------------------ */
#define DEF_MOV_DIRECT_RN(n) \
static uint8_t op_mov_direct_r##n(mcs51_cpu_t *cpu) { \
    mcs51_direct_write(cpu, fetch1(cpu), REG(cpu, n)); \
    cpu->pc += 2; \
    return 2; \
}
DEF_MOV_DIRECT_RN(0) DEF_MOV_DIRECT_RN(1) DEF_MOV_DIRECT_RN(2) DEF_MOV_DIRECT_RN(3)
DEF_MOV_DIRECT_RN(4) DEF_MOV_DIRECT_RN(5) DEF_MOV_DIRECT_RN(6) DEF_MOV_DIRECT_RN(7)

/* ------------------------------------------------------------------ */
/*  0x90  MOV DPTR,#imm16                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_dptr_imm(mcs51_cpu_t *cpu)
{
    DPH(cpu) = fetch1(cpu);
    DPL(cpu) = fetch2(cpu);
    cpu->pc += 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x92  MOV bit,C                                                    */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_bit_c(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    mcs51_bit_write(cpu, bit, get_cy(cpu));
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x93  MOVC A,@A+DPTR                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_movc_a_dptr(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_code_read(cpu, (uint16_t)(ACC(cpu) + DPTR(cpu)));
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0x94  SUBB A,#imm                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_subb_a_imm(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_subb(cpu, ACC(cpu), fetch1(cpu), get_cy(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x95  SUBB A,direct                                                */
/* ------------------------------------------------------------------ */
static uint8_t op_subb_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_subb(cpu, ACC(cpu), mcs51_direct_read(cpu, fetch1(cpu)), get_cy(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x96-0x97  SUBB A,@Ri                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_subb_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_subb(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 0)), get_cy(cpu));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_subb_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) = do_subb(cpu, ACC(cpu), mcs51_indirect_read(cpu, REG(cpu, 1)), get_cy(cpu));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0x98-0x9F  SUBB A,Rn                                               */
/* ------------------------------------------------------------------ */
#define DEF_SUBB_RN(n) \
static uint8_t op_subb_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) = do_subb(cpu, ACC(cpu), REG(cpu, n), get_cy(cpu)); \
    cpu->pc += 1; \
    return 1; \
}
DEF_SUBB_RN(0) DEF_SUBB_RN(1) DEF_SUBB_RN(2) DEF_SUBB_RN(3)
DEF_SUBB_RN(4) DEF_SUBB_RN(5) DEF_SUBB_RN(6) DEF_SUBB_RN(7)

/* ------------------------------------------------------------------ */
/*  0xA0  ORL C,/bit                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_orl_c_nbit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    if (!mcs51_bit_read(cpu, bit))
        PSW(cpu) |= PSW_CY;
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xA2  MOV C,bit                                                    */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_c_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    set_cy(cpu, mcs51_bit_read(cpu, bit));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xA3  INC DPTR                                                     */
/* ------------------------------------------------------------------ */
static uint8_t op_inc_dptr(mcs51_cpu_t *cpu)
{
    uint16_t dp = DPTR(cpu) + 1;
    DPL(cpu) = dp & 0xFF;
    DPH(cpu) = (dp >> 8) & 0xFF;
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xA4  MUL AB                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_mul_ab(mcs51_cpu_t *cpu)
{
    uint16_t result = (uint16_t)ACC(cpu) * B(cpu);
    ACC(cpu) = result & 0xFF;
    B(cpu)   = (result >> 8) & 0xFF;

    set_cy(cpu, 0);
    set_ov(cpu, B(cpu) != 0);

    cpu->pc += 1;
    return 4;
}

/* ------------------------------------------------------------------ */
/*  0xA5  RESERVED / BREAK                                             */
/* ------------------------------------------------------------------ */
static uint8_t op_break(mcs51_cpu_t *cpu)
{
    cpu->state = MCS51_STATE_BREAK;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xA6-0xA7  MOV @Ri,direct                                          */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_ind_r0_direct(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 0), mcs51_direct_read(cpu, fetch1(cpu)));
    cpu->pc += 2;
    return 2;
}

static uint8_t op_mov_ind_r1_direct(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 1), mcs51_direct_read(cpu, fetch1(cpu)));
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xA8-0xAF  MOV Rn,direct                                           */
/* ------------------------------------------------------------------ */
#define DEF_MOV_RN_DIRECT(n) \
static uint8_t op_mov_r##n##_direct(mcs51_cpu_t *cpu) { \
    REG(cpu, n) = mcs51_direct_read(cpu, fetch1(cpu)); \
    cpu->pc += 2; \
    return 2; \
}
DEF_MOV_RN_DIRECT(0) DEF_MOV_RN_DIRECT(1) DEF_MOV_RN_DIRECT(2) DEF_MOV_RN_DIRECT(3)
DEF_MOV_RN_DIRECT(4) DEF_MOV_RN_DIRECT(5) DEF_MOV_RN_DIRECT(6) DEF_MOV_RN_DIRECT(7)

/* ------------------------------------------------------------------ */
/*  0xB0  ANL C,/bit                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_anl_c_nbit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    if (mcs51_bit_read(cpu, bit))
        PSW(cpu) &= ~PSW_CY;
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xB2  CPL bit                                                      */
/* ------------------------------------------------------------------ */
static uint8_t op_cpl_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    mcs51_bit_write(cpu, bit, !mcs51_bit_read(cpu, bit));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xB3  CPL C                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_cpl_c(mcs51_cpu_t *cpu)
{
    PSW(cpu) ^= PSW_CY;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xB4  CJNE A,#imm,rel                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_cjne_a_imm(mcs51_cpu_t *cpu)
{
    uint8_t imm = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    set_cy(cpu, ACC(cpu) < imm);
    cpu->pc += 3;
    if (ACC(cpu) != imm)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xB5  CJNE A,direct,rel                                            */
/* ------------------------------------------------------------------ */
static uint8_t op_cjne_a_direct(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    int8_t  rel  = (int8_t)fetch2(cpu);
    uint8_t val  = mcs51_direct_read(cpu, addr);
    set_cy(cpu, ACC(cpu) < val);
    cpu->pc += 3;
    if (ACC(cpu) != val)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xB6-0xB7  CJNE @Ri,#imm,rel                                      */
/* ------------------------------------------------------------------ */
static uint8_t op_cjne_ind_r0_imm(mcs51_cpu_t *cpu)
{
    uint8_t imm = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    uint8_t val = mcs51_indirect_read(cpu, REG(cpu, 0));
    set_cy(cpu, val < imm);
    cpu->pc += 3;
    if (val != imm)
        cpu->pc = cpu->pc + rel;
    return 2;
}

static uint8_t op_cjne_ind_r1_imm(mcs51_cpu_t *cpu)
{
    uint8_t imm = fetch1(cpu);
    int8_t  rel = (int8_t)fetch2(cpu);
    uint8_t val = mcs51_indirect_read(cpu, REG(cpu, 1));
    set_cy(cpu, val < imm);
    cpu->pc += 3;
    if (val != imm)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xB8-0xBF  CJNE Rn,#imm,rel                                       */
/* ------------------------------------------------------------------ */
#define DEF_CJNE_RN(n) \
static uint8_t op_cjne_r##n##_imm(mcs51_cpu_t *cpu) { \
    uint8_t imm = fetch1(cpu); \
    int8_t  rel = (int8_t)fetch2(cpu); \
    set_cy(cpu, REG(cpu, n) < imm); \
    cpu->pc += 3; \
    if (REG(cpu, n) != imm) \
        cpu->pc = cpu->pc + rel; \
    return 2; \
}
DEF_CJNE_RN(0) DEF_CJNE_RN(1) DEF_CJNE_RN(2) DEF_CJNE_RN(3)
DEF_CJNE_RN(4) DEF_CJNE_RN(5) DEF_CJNE_RN(6) DEF_CJNE_RN(7)

/* ------------------------------------------------------------------ */
/*  0xC0  PUSH direct                                                  */
/* ------------------------------------------------------------------ */
static uint8_t op_push(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_push(cpu, mcs51_direct_read(cpu, addr));
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xC2  CLR bit                                                      */
/* ------------------------------------------------------------------ */
static uint8_t op_clr_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    mcs51_bit_write(cpu, bit, 0);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xC3  CLR C                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_clr_c(mcs51_cpu_t *cpu)
{
    PSW(cpu) &= ~PSW_CY;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xC4  SWAP A                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_swap_a(mcs51_cpu_t *cpu)
{
    uint8_t a = ACC(cpu);
    ACC(cpu) = (a << 4) | (a >> 4);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xC5  XCH A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_xch_a_direct(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    uint8_t tmp  = mcs51_direct_read(cpu, addr);
    mcs51_direct_write(cpu, addr, ACC(cpu));
    ACC(cpu) = tmp;
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xC6-0xC7  XCH A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_xch_a_ind_r0(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 0);
    uint8_t tmp  = mcs51_indirect_read(cpu, addr);
    mcs51_indirect_write(cpu, addr, ACC(cpu));
    ACC(cpu) = tmp;
    cpu->pc += 1;
    return 1;
}

static uint8_t op_xch_a_ind_r1(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 1);
    uint8_t tmp  = mcs51_indirect_read(cpu, addr);
    mcs51_indirect_write(cpu, addr, ACC(cpu));
    ACC(cpu) = tmp;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xC8-0xCF  XCH A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_XCH_RN(n) \
static uint8_t op_xch_a_r##n(mcs51_cpu_t *cpu) { \
    uint8_t tmp = REG(cpu, n); \
    REG(cpu, n) = ACC(cpu); \
    ACC(cpu) = tmp; \
    cpu->pc += 1; \
    return 1; \
}
DEF_XCH_RN(0) DEF_XCH_RN(1) DEF_XCH_RN(2) DEF_XCH_RN(3)
DEF_XCH_RN(4) DEF_XCH_RN(5) DEF_XCH_RN(6) DEF_XCH_RN(7)

/* ------------------------------------------------------------------ */
/*  0xD0  POP direct                                                   */
/* ------------------------------------------------------------------ */
static uint8_t op_pop(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    mcs51_direct_write(cpu, addr, mcs51_pop(cpu));
    cpu->pc += 2;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xD2  SETB bit                                                     */
/* ------------------------------------------------------------------ */
static uint8_t op_setb_bit(mcs51_cpu_t *cpu)
{
    uint8_t bit = fetch1(cpu);
    mcs51_bit_write(cpu, bit, 1);
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xD3  SETB C                                                       */
/* ------------------------------------------------------------------ */
static uint8_t op_setb_c(mcs51_cpu_t *cpu)
{
    PSW(cpu) |= PSW_CY;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xD4  DA A                                                         */
/* ------------------------------------------------------------------ */
static uint8_t op_da_a(mcs51_cpu_t *cpu)
{
    uint16_t a = ACC(cpu);

    if ((a & 0x0F) > 9 || (PSW(cpu) & PSW_AC)) {
        a += 0x06;
        if (a > 0xFF)
            PSW(cpu) |= PSW_CY;
    }

    if ((a & 0xF0) > 0x90 || (PSW(cpu) & PSW_CY)) {
        a += 0x60;
        if (a > 0xFF)
            PSW(cpu) |= PSW_CY;
    }

    ACC(cpu) = (uint8_t)a;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xD5  DJNZ direct,rel                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_djnz_direct(mcs51_cpu_t *cpu)
{
    uint8_t addr = fetch1(cpu);
    int8_t  rel  = (int8_t)fetch2(cpu);
    uint8_t val  = mcs51_direct_read(cpu, addr) - 1;
    mcs51_direct_write(cpu, addr, val);
    cpu->pc += 3;
    if (val != 0)
        cpu->pc = cpu->pc + rel;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xD6-0xD7  XCHD A,@Ri                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_xchd_a_ind_r0(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 0);
    uint8_t mem  = mcs51_indirect_read(cpu, addr);
    uint8_t a    = ACC(cpu);
    ACC(cpu) = (a & 0xF0) | (mem & 0x0F);
    mcs51_indirect_write(cpu, addr, (mem & 0xF0) | (a & 0x0F));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_xchd_a_ind_r1(mcs51_cpu_t *cpu)
{
    uint8_t addr = REG(cpu, 1);
    uint8_t mem  = mcs51_indirect_read(cpu, addr);
    uint8_t a    = ACC(cpu);
    ACC(cpu) = (a & 0xF0) | (mem & 0x0F);
    mcs51_indirect_write(cpu, addr, (mem & 0xF0) | (a & 0x0F));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xD8-0xDF  DJNZ Rn,rel                                             */
/* ------------------------------------------------------------------ */
#define DEF_DJNZ_RN(n) \
static uint8_t op_djnz_r##n(mcs51_cpu_t *cpu) { \
    int8_t rel = (int8_t)fetch1(cpu); \
    REG(cpu, n)--; \
    cpu->pc += 2; \
    if (REG(cpu, n) != 0) \
        cpu->pc = cpu->pc + rel; \
    return 2; \
}
DEF_DJNZ_RN(0) DEF_DJNZ_RN(1) DEF_DJNZ_RN(2) DEF_DJNZ_RN(3)
DEF_DJNZ_RN(4) DEF_DJNZ_RN(5) DEF_DJNZ_RN(6) DEF_DJNZ_RN(7)

/* ------------------------------------------------------------------ */
/*  0xE0  MOVX A,@DPTR                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_movx_a_dptr(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_xdata_read(cpu, DPTR(cpu));
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xE2-0xE3  MOVX A,@Ri                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_movx_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_xdata_read(cpu, REG(cpu, 0));
    cpu->pc += 1;
    return 2;
}

static uint8_t op_movx_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_xdata_read(cpu, REG(cpu, 1));
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xE4  CLR A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_clr_a(mcs51_cpu_t *cpu)
{
    ACC(cpu) = 0;
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xE5  MOV A,direct                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_a_direct(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_direct_read(cpu, fetch1(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xE6-0xE7  MOV A,@Ri                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_a_ind_r0(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_indirect_read(cpu, REG(cpu, 0));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_mov_a_ind_r1(mcs51_cpu_t *cpu)
{
    ACC(cpu) = mcs51_indirect_read(cpu, REG(cpu, 1));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xE8-0xEF  MOV A,Rn                                                */
/* ------------------------------------------------------------------ */
#define DEF_MOV_A_RN(n) \
static uint8_t op_mov_a_r##n(mcs51_cpu_t *cpu) { \
    ACC(cpu) = REG(cpu, n); \
    cpu->pc += 1; \
    return 1; \
}
DEF_MOV_A_RN(0) DEF_MOV_A_RN(1) DEF_MOV_A_RN(2) DEF_MOV_A_RN(3)
DEF_MOV_A_RN(4) DEF_MOV_A_RN(5) DEF_MOV_A_RN(6) DEF_MOV_A_RN(7)

/* ------------------------------------------------------------------ */
/*  0xF0  MOVX @DPTR,A                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_movx_dptr_a(mcs51_cpu_t *cpu)
{
    mcs51_xdata_write(cpu, DPTR(cpu), ACC(cpu));
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xF2-0xF3  MOVX @Ri,A                                              */
/* ------------------------------------------------------------------ */
static uint8_t op_movx_ind_r0_a(mcs51_cpu_t *cpu)
{
    mcs51_xdata_write(cpu, REG(cpu, 0), ACC(cpu));
    cpu->pc += 1;
    return 2;
}

static uint8_t op_movx_ind_r1_a(mcs51_cpu_t *cpu)
{
    mcs51_xdata_write(cpu, REG(cpu, 1), ACC(cpu));
    cpu->pc += 1;
    return 2;
}

/* ------------------------------------------------------------------ */
/*  0xF4  CPL A                                                        */
/* ------------------------------------------------------------------ */
static uint8_t op_cpl_a(mcs51_cpu_t *cpu)
{
    ACC(cpu) = ~ACC(cpu);
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xF5  MOV direct,A                                                 */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_direct_a(mcs51_cpu_t *cpu)
{
    mcs51_direct_write(cpu, fetch1(cpu), ACC(cpu));
    cpu->pc += 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xF6-0xF7  MOV @Ri,A                                               */
/* ------------------------------------------------------------------ */
static uint8_t op_mov_ind_r0_a(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 0), ACC(cpu));
    cpu->pc += 1;
    return 1;
}

static uint8_t op_mov_ind_r1_a(mcs51_cpu_t *cpu)
{
    mcs51_indirect_write(cpu, REG(cpu, 1), ACC(cpu));
    cpu->pc += 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  0xF8-0xFF  MOV Rn,A                                                */
/* ------------------------------------------------------------------ */
#define DEF_MOV_RN_A(n) \
static uint8_t op_mov_r##n##_a(mcs51_cpu_t *cpu) { \
    REG(cpu, n) = ACC(cpu); \
    cpu->pc += 1; \
    return 1; \
}
DEF_MOV_RN_A(0) DEF_MOV_RN_A(1) DEF_MOV_RN_A(2) DEF_MOV_RN_A(3)
DEF_MOV_RN_A(4) DEF_MOV_RN_A(5) DEF_MOV_RN_A(6) DEF_MOV_RN_A(7)

/* ================================================================== */
/*  DISPATCH TABLE  (all 256 entries)                                  */
/* ================================================================== */

static const mcs51_op_fn op_table[256] = {
    /* 0x00 */ op_nop,
    /* 0x01 */ op_ajmp,
    /* 0x02 */ op_ljmp,
    /* 0x03 */ op_rr_a,
    /* 0x04 */ op_inc_a,
    /* 0x05 */ op_inc_direct,
    /* 0x06 */ op_inc_ind_r0,
    /* 0x07 */ op_inc_ind_r1,
    /* 0x08 */ op_inc_r0,
    /* 0x09 */ op_inc_r1,
    /* 0x0A */ op_inc_r2,
    /* 0x0B */ op_inc_r3,
    /* 0x0C */ op_inc_r4,
    /* 0x0D */ op_inc_r5,
    /* 0x0E */ op_inc_r6,
    /* 0x0F */ op_inc_r7,

    /* 0x10 */ op_jbc,
    /* 0x11 */ op_acall,
    /* 0x12 */ op_lcall,
    /* 0x13 */ op_rrc_a,
    /* 0x14 */ op_dec_a,
    /* 0x15 */ op_dec_direct,
    /* 0x16 */ op_dec_ind_r0,
    /* 0x17 */ op_dec_ind_r1,
    /* 0x18 */ op_dec_r0,
    /* 0x19 */ op_dec_r1,
    /* 0x1A */ op_dec_r2,
    /* 0x1B */ op_dec_r3,
    /* 0x1C */ op_dec_r4,
    /* 0x1D */ op_dec_r5,
    /* 0x1E */ op_dec_r6,
    /* 0x1F */ op_dec_r7,

    /* 0x20 */ op_jb,
    /* 0x21 */ op_ajmp,
    /* 0x22 */ op_ret,
    /* 0x23 */ op_rl_a,
    /* 0x24 */ op_add_a_imm,
    /* 0x25 */ op_add_a_direct,
    /* 0x26 */ op_add_a_ind_r0,
    /* 0x27 */ op_add_a_ind_r1,
    /* 0x28 */ op_add_a_r0,
    /* 0x29 */ op_add_a_r1,
    /* 0x2A */ op_add_a_r2,
    /* 0x2B */ op_add_a_r3,
    /* 0x2C */ op_add_a_r4,
    /* 0x2D */ op_add_a_r5,
    /* 0x2E */ op_add_a_r6,
    /* 0x2F */ op_add_a_r7,

    /* 0x30 */ op_jnb,
    /* 0x31 */ op_acall,
    /* 0x32 */ op_reti,
    /* 0x33 */ op_rlc_a,
    /* 0x34 */ op_addc_a_imm,
    /* 0x35 */ op_addc_a_direct,
    /* 0x36 */ op_addc_a_ind_r0,
    /* 0x37 */ op_addc_a_ind_r1,
    /* 0x38 */ op_addc_a_r0,
    /* 0x39 */ op_addc_a_r1,
    /* 0x3A */ op_addc_a_r2,
    /* 0x3B */ op_addc_a_r3,
    /* 0x3C */ op_addc_a_r4,
    /* 0x3D */ op_addc_a_r5,
    /* 0x3E */ op_addc_a_r6,
    /* 0x3F */ op_addc_a_r7,

    /* 0x40 */ op_jc,
    /* 0x41 */ op_ajmp,
    /* 0x42 */ op_orl_direct_a,
    /* 0x43 */ op_orl_direct_imm,
    /* 0x44 */ op_orl_a_imm,
    /* 0x45 */ op_orl_a_direct,
    /* 0x46 */ op_orl_a_ind_r0,
    /* 0x47 */ op_orl_a_ind_r1,
    /* 0x48 */ op_orl_a_r0,
    /* 0x49 */ op_orl_a_r1,
    /* 0x4A */ op_orl_a_r2,
    /* 0x4B */ op_orl_a_r3,
    /* 0x4C */ op_orl_a_r4,
    /* 0x4D */ op_orl_a_r5,
    /* 0x4E */ op_orl_a_r6,
    /* 0x4F */ op_orl_a_r7,

    /* 0x50 */ op_jnc,
    /* 0x51 */ op_acall,
    /* 0x52 */ op_anl_direct_a,
    /* 0x53 */ op_anl_direct_imm,
    /* 0x54 */ op_anl_a_imm,
    /* 0x55 */ op_anl_a_direct,
    /* 0x56 */ op_anl_a_ind_r0,
    /* 0x57 */ op_anl_a_ind_r1,
    /* 0x58 */ op_anl_a_r0,
    /* 0x59 */ op_anl_a_r1,
    /* 0x5A */ op_anl_a_r2,
    /* 0x5B */ op_anl_a_r3,
    /* 0x5C */ op_anl_a_r4,
    /* 0x5D */ op_anl_a_r5,
    /* 0x5E */ op_anl_a_r6,
    /* 0x5F */ op_anl_a_r7,

    /* 0x60 */ op_jz,
    /* 0x61 */ op_ajmp,
    /* 0x62 */ op_xrl_direct_a,
    /* 0x63 */ op_xrl_direct_imm,
    /* 0x64 */ op_xrl_a_imm,
    /* 0x65 */ op_xrl_a_direct,
    /* 0x66 */ op_xrl_a_ind_r0,
    /* 0x67 */ op_xrl_a_ind_r1,
    /* 0x68 */ op_xrl_a_r0,
    /* 0x69 */ op_xrl_a_r1,
    /* 0x6A */ op_xrl_a_r2,
    /* 0x6B */ op_xrl_a_r3,
    /* 0x6C */ op_xrl_a_r4,
    /* 0x6D */ op_xrl_a_r5,
    /* 0x6E */ op_xrl_a_r6,
    /* 0x6F */ op_xrl_a_r7,

    /* 0x70 */ op_jnz,
    /* 0x71 */ op_acall,
    /* 0x72 */ op_orl_c_bit,
    /* 0x73 */ op_jmp_a_dptr,
    /* 0x74 */ op_mov_a_imm,
    /* 0x75 */ op_mov_direct_imm,
    /* 0x76 */ op_mov_ind_r0_imm,
    /* 0x77 */ op_mov_ind_r1_imm,
    /* 0x78 */ op_mov_r0_imm,
    /* 0x79 */ op_mov_r1_imm,
    /* 0x7A */ op_mov_r2_imm,
    /* 0x7B */ op_mov_r3_imm,
    /* 0x7C */ op_mov_r4_imm,
    /* 0x7D */ op_mov_r5_imm,
    /* 0x7E */ op_mov_r6_imm,
    /* 0x7F */ op_mov_r7_imm,

    /* 0x80 */ op_sjmp,
    /* 0x81 */ op_ajmp,
    /* 0x82 */ op_anl_c_bit,
    /* 0x83 */ op_movc_a_pc,
    /* 0x84 */ op_div_ab,
    /* 0x85 */ op_mov_direct_direct,
    /* 0x86 */ op_mov_direct_ind_r0,
    /* 0x87 */ op_mov_direct_ind_r1,
    /* 0x88 */ op_mov_direct_r0,
    /* 0x89 */ op_mov_direct_r1,
    /* 0x8A */ op_mov_direct_r2,
    /* 0x8B */ op_mov_direct_r3,
    /* 0x8C */ op_mov_direct_r4,
    /* 0x8D */ op_mov_direct_r5,
    /* 0x8E */ op_mov_direct_r6,
    /* 0x8F */ op_mov_direct_r7,

    /* 0x90 */ op_mov_dptr_imm,
    /* 0x91 */ op_acall,
    /* 0x92 */ op_mov_bit_c,
    /* 0x93 */ op_movc_a_dptr,
    /* 0x94 */ op_subb_a_imm,
    /* 0x95 */ op_subb_a_direct,
    /* 0x96 */ op_subb_a_ind_r0,
    /* 0x97 */ op_subb_a_ind_r1,
    /* 0x98 */ op_subb_a_r0,
    /* 0x99 */ op_subb_a_r1,
    /* 0x9A */ op_subb_a_r2,
    /* 0x9B */ op_subb_a_r3,
    /* 0x9C */ op_subb_a_r4,
    /* 0x9D */ op_subb_a_r5,
    /* 0x9E */ op_subb_a_r6,
    /* 0x9F */ op_subb_a_r7,

    /* 0xA0 */ op_orl_c_nbit,
    /* 0xA1 */ op_ajmp,
    /* 0xA2 */ op_mov_c_bit,
    /* 0xA3 */ op_inc_dptr,
    /* 0xA4 */ op_mul_ab,
    /* 0xA5 */ op_break,
    /* 0xA6 */ op_mov_ind_r0_direct,
    /* 0xA7 */ op_mov_ind_r1_direct,
    /* 0xA8 */ op_mov_r0_direct,
    /* 0xA9 */ op_mov_r1_direct,
    /* 0xAA */ op_mov_r2_direct,
    /* 0xAB */ op_mov_r3_direct,
    /* 0xAC */ op_mov_r4_direct,
    /* 0xAD */ op_mov_r5_direct,
    /* 0xAE */ op_mov_r6_direct,
    /* 0xAF */ op_mov_r7_direct,

    /* 0xB0 */ op_anl_c_nbit,
    /* 0xB1 */ op_acall,
    /* 0xB2 */ op_cpl_bit,
    /* 0xB3 */ op_cpl_c,
    /* 0xB4 */ op_cjne_a_imm,
    /* 0xB5 */ op_cjne_a_direct,
    /* 0xB6 */ op_cjne_ind_r0_imm,
    /* 0xB7 */ op_cjne_ind_r1_imm,
    /* 0xB8 */ op_cjne_r0_imm,
    /* 0xB9 */ op_cjne_r1_imm,
    /* 0xBA */ op_cjne_r2_imm,
    /* 0xBB */ op_cjne_r3_imm,
    /* 0xBC */ op_cjne_r4_imm,
    /* 0xBD */ op_cjne_r5_imm,
    /* 0xBE */ op_cjne_r6_imm,
    /* 0xBF */ op_cjne_r7_imm,

    /* 0xC0 */ op_push,
    /* 0xC1 */ op_ajmp,
    /* 0xC2 */ op_clr_bit,
    /* 0xC3 */ op_clr_c,
    /* 0xC4 */ op_swap_a,
    /* 0xC5 */ op_xch_a_direct,
    /* 0xC6 */ op_xch_a_ind_r0,
    /* 0xC7 */ op_xch_a_ind_r1,
    /* 0xC8 */ op_xch_a_r0,
    /* 0xC9 */ op_xch_a_r1,
    /* 0xCA */ op_xch_a_r2,
    /* 0xCB */ op_xch_a_r3,
    /* 0xCC */ op_xch_a_r4,
    /* 0xCD */ op_xch_a_r5,
    /* 0xCE */ op_xch_a_r6,
    /* 0xCF */ op_xch_a_r7,

    /* 0xD0 */ op_pop,
    /* 0xD1 */ op_acall,
    /* 0xD2 */ op_setb_bit,
    /* 0xD3 */ op_setb_c,
    /* 0xD4 */ op_da_a,
    /* 0xD5 */ op_djnz_direct,
    /* 0xD6 */ op_xchd_a_ind_r0,
    /* 0xD7 */ op_xchd_a_ind_r1,
    /* 0xD8 */ op_djnz_r0,
    /* 0xD9 */ op_djnz_r1,
    /* 0xDA */ op_djnz_r2,
    /* 0xDB */ op_djnz_r3,
    /* 0xDC */ op_djnz_r4,
    /* 0xDD */ op_djnz_r5,
    /* 0xDE */ op_djnz_r6,
    /* 0xDF */ op_djnz_r7,

    /* 0xE0 */ op_movx_a_dptr,
    /* 0xE1 */ op_ajmp,
    /* 0xE2 */ op_movx_a_ind_r0,
    /* 0xE3 */ op_movx_a_ind_r1,
    /* 0xE4 */ op_clr_a,
    /* 0xE5 */ op_mov_a_direct,
    /* 0xE6 */ op_mov_a_ind_r0,
    /* 0xE7 */ op_mov_a_ind_r1,
    /* 0xE8 */ op_mov_a_r0,
    /* 0xE9 */ op_mov_a_r1,
    /* 0xEA */ op_mov_a_r2,
    /* 0xEB */ op_mov_a_r3,
    /* 0xEC */ op_mov_a_r4,
    /* 0xED */ op_mov_a_r5,
    /* 0xEE */ op_mov_a_r6,
    /* 0xEF */ op_mov_a_r7,

    /* 0xF0 */ op_movx_dptr_a,
    /* 0xF1 */ op_acall,
    /* 0xF2 */ op_movx_ind_r0_a,
    /* 0xF3 */ op_movx_ind_r1_a,
    /* 0xF4 */ op_cpl_a,
    /* 0xF5 */ op_mov_direct_a,
    /* 0xF6 */ op_mov_ind_r0_a,
    /* 0xF7 */ op_mov_ind_r1_a,
    /* 0xF8 */ op_mov_r0_a,
    /* 0xF9 */ op_mov_r1_a,
    /* 0xFA */ op_mov_r2_a,
    /* 0xFB */ op_mov_r3_a,
    /* 0xFC */ op_mov_r4_a,
    /* 0xFD */ op_mov_r5_a,
    /* 0xFE */ op_mov_r6_a,
    /* 0xFF */ op_mov_r7_a,
};

/* ================================================================== */
/*  PUBLIC ENTRY POINT                                                 */
/* ================================================================== */

uint8_t mcs51_decode_execute(mcs51_cpu_t *cpu)
{
    uint8_t opcode = mcs51_code_read(cpu, cpu->pc);
    return op_table[opcode](cpu);
}
