/*
 * ucvm - Microcontroller Virtual Machine
 * MCS-51 (8051) CPU core: types, SFR definitions, memory access API
 */
#ifndef MCS51_CPU_H
#define MCS51_CPU_H

#include <stdint.h>
#include <stddef.h>
#include "../io/io_bridge.h"

typedef struct mcs51_cpu mcs51_cpu_t;

/* ---------- CPU states (shared with AVR) ---------- */
enum {
    MCS51_STATE_RUNNING  = 0,
    MCS51_STATE_SLEEPING = 1,
    MCS51_STATE_HALTED   = 2,
    MCS51_STATE_BREAK    = 3,
};

/* ---------- SFR addresses ---------- */
#define SFR_P0      0x80
#define SFR_SP      0x81
#define SFR_DPL     0x82
#define SFR_DPH     0x83
#define SFR_PCON    0x87
#define SFR_TCON    0x88
#define SFR_TMOD    0x89
#define SFR_TL0     0x8A
#define SFR_TL1     0x8B
#define SFR_TH0     0x8C
#define SFR_TH1     0x8D
#define SFR_P1      0x90
#define SFR_SCON    0x98
#define SFR_SBUF    0x99
#define SFR_P2      0xA0
#define SFR_IE      0xA8
#define SFR_P3      0xB0
#define SFR_IP      0xB8
#define SFR_T2CON   0xC8
#define SFR_T2MOD   0xC9
#define SFR_RCAP2L  0xCA
#define SFR_RCAP2H  0xCB
#define SFR_TL2     0xCC
#define SFR_TH2     0xCD
#define SFR_PSW     0xD0
#define SFR_ACC     0xE0
#define SFR_B       0xF0

/* SFR index (for sfr[] array) */
#define SFI(addr) ((addr) - 0x80)

/* ---------- PSW bits ---------- */
#define PSW_P   0x01
#define PSW_F1  0x02
#define PSW_OV  0x04
#define PSW_RS0 0x08
#define PSW_RS1 0x10
#define PSW_F0  0x20
#define PSW_AC  0x40
#define PSW_CY  0x80

/* ---------- TCON bits ---------- */
#define TCON_IT0 0x01
#define TCON_IE0 0x02
#define TCON_IT1 0x04
#define TCON_IE1 0x08
#define TCON_TR0 0x10
#define TCON_TF0 0x20
#define TCON_TR1 0x40
#define TCON_TF1 0x80

/* ---------- IE bits ---------- */
#define IE_EX0  0x01
#define IE_ET0  0x02
#define IE_EX1  0x04
#define IE_ET1  0x08
#define IE_ES   0x10
#define IE_ET2  0x20
#define IE_EA   0x80

/* ---------- IP bits ---------- */
#define IP_PX0  0x01
#define IP_PT0  0x02
#define IP_PX1  0x04
#define IP_PT1  0x08
#define IP_PS   0x10
#define IP_PT2  0x20

/* ---------- SCON bits ---------- */
#define SCON_RI   0x01
#define SCON_TI   0x02
#define SCON_RB8  0x04
#define SCON_TB8  0x08
#define SCON_REN  0x10
#define SCON_SM2  0x20
#define SCON_SM1  0x40
#define SCON_SM0  0x80

/* ---------- Interrupt vectors ---------- */
#define INT_VEC_EX0    0x0003  /* External INT0 */
#define INT_VEC_T0     0x000B  /* Timer 0 */
#define INT_VEC_EX1    0x0013  /* External INT1 */
#define INT_VEC_T1     0x001B  /* Timer 1 */
#define INT_VEC_SERIAL 0x0023  /* Serial port */
#define INT_VEC_T2     0x002B  /* Timer 2 */

/* Number of interrupt sources */
#define MCS51_NUM_INTERRUPTS 6

/* SFR hook callbacks */
typedef void    (*sfr_write_fn)(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val, void *ctx);
typedef uint8_t (*sfr_read_fn)(mcs51_cpu_t *cpu, uint8_t addr, void *ctx);

/* ---------- Variant configuration ---------- */
typedef struct {
    const char *name;
    uint32_t code_size;     /* bytes (8192 for AT89S52) */
    uint16_t iram_size;     /* 256 for AT89S52 (128 lower + 128 upper) */
    uint16_t xram_size;     /* 256 for AT89S52 on-chip */
    uint8_t  has_timer2;    /* 1 for 8052/AT89S52 */
    uint8_t  num_interrupts;
    void (*periph_init)(mcs51_cpu_t *cpu);
} mcs51_variant_t;

/* ---------- CPU state ---------- */
struct mcs51_cpu {
    uint16_t pc;
    uint64_t cycles;
    volatile uint8_t state;

    /* Memory */
    uint8_t  iram[256];     /* Internal RAM: 0x00-0x7F (direct+indirect), 0x80-0xFF (indirect only) */
    uint8_t  sfr[128];      /* SFR: indexed by addr-0x80, direct addressing only */
    uint8_t  *code;         /* Code memory (allocated, up to 64KB) */
    uint32_t code_size;
    uint8_t  *xram;         /* External RAM (allocated) */
    uint16_t xram_size;

    /* SFR dispatch hooks (same pattern as AVR I/O handlers) */
    sfr_write_fn sfr_write[128];
    sfr_read_fn  sfr_read[128];
    void        *sfr_ctx[128];

    /* Interrupt state */
    uint8_t  int_active[2]; /* Currently active ISR at each priority level */
    uint8_t  int_priority;  /* Current highest active priority (-1 = none) */

    /* Peripheral pointers */
    void *periph_timer;
    void *periph_uart;
    void *periph_gpio;
    void *periph_intc;

    /* I/O bridge */
    io_bridge_cb_t bridge_cb;
    void *bridge_ctx;

    const mcs51_variant_t *variant;
};

/* ---------- Convenience macros ---------- */
#define ACC(cpu)  ((cpu)->sfr[SFI(SFR_ACC)])
#define B(cpu)    ((cpu)->sfr[SFI(SFR_B)])
#define PSW(cpu)  ((cpu)->sfr[SFI(SFR_PSW)])
#define SP(cpu)   ((cpu)->sfr[SFI(SFR_SP)])
#define DPL(cpu)  ((cpu)->sfr[SFI(SFR_DPL)])
#define DPH(cpu)  ((cpu)->sfr[SFI(SFR_DPH)])
#define DPTR(cpu) ((uint16_t)(DPL(cpu) | (DPH(cpu) << 8)))

/* Register bank base address: R0-R7 in iram at bank*8 */
#define REG_BANK(cpu)  (((PSW(cpu) >> 3) & 0x03) * 8)
#define REG(cpu, n)    ((cpu)->iram[REG_BANK(cpu) + (n)])

/* SFR access macros */
#define SFR_GET(cpu, addr) ((cpu)->sfr[SFI(addr)])
#define SFR_SET(cpu, addr, val) do { \
    uint8_t _si = SFI(addr); \
    (cpu)->sfr[_si] = (val); \
    if ((cpu)->sfr_write[_si]) \
        (cpu)->sfr_write[_si]((cpu), (addr), (val), (cpu)->sfr_ctx[_si]); \
} while(0)

/* ---------- API ---------- */

mcs51_cpu_t *mcs51_cpu_init(const mcs51_variant_t *variant);
void mcs51_cpu_free(mcs51_cpu_t *cpu);
void mcs51_cpu_reset(mcs51_cpu_t *cpu);
uint8_t mcs51_cpu_step(mcs51_cpu_t *cpu);
void mcs51_cpu_check_irq(mcs51_cpu_t *cpu);

/* Memory access */
uint8_t mcs51_direct_read(mcs51_cpu_t *cpu, uint8_t addr);
void    mcs51_direct_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val);
uint8_t mcs51_indirect_read(mcs51_cpu_t *cpu, uint8_t addr);
void    mcs51_indirect_write(mcs51_cpu_t *cpu, uint8_t addr, uint8_t val);
uint8_t mcs51_bit_read(mcs51_cpu_t *cpu, uint8_t bitaddr);
void    mcs51_bit_write(mcs51_cpu_t *cpu, uint8_t bitaddr, uint8_t val);
uint8_t mcs51_xdata_read(mcs51_cpu_t *cpu, uint16_t addr);
void    mcs51_xdata_write(mcs51_cpu_t *cpu, uint16_t addr, uint8_t val);
uint8_t mcs51_code_read(mcs51_cpu_t *cpu, uint16_t addr);

/* SFR handler registration */
void mcs51_sfr_register(mcs51_cpu_t *cpu, uint8_t addr,
                         sfr_read_fn read, sfr_write_fn write, void *ctx);

/* Instruction decoder (called by mcs51_cpu_step) */
uint8_t mcs51_decode_execute(mcs51_cpu_t *cpu);

/* Parity helper */
static inline uint8_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
}

/* ---------- Variant descriptors ---------- */
extern const mcs51_variant_t mcs51_at89s52;

#endif /* MCS51_CPU_H */
