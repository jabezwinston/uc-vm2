/*
 * ucvm - Microcontroller Virtual Machine
 * AVR CPU core: types, macros, and API declarations
 */
#ifndef AVR_CPU_H
#define AVR_CPU_H

#include <stdint.h>
#include <stddef.h>
#include "../io/io_bridge.h"

/* Place hot emulation code in IRAM on ESP32 to avoid flash cache misses */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define AVR_HOT  IRAM_ATTR
#else
#define AVR_HOT
#endif

/* Forward declaration */
typedef struct avr_cpu avr_cpu_t;

/* ---------- Variant configuration ---------- */

#define AVR_FLAG_HAS_MUL       0x01  /* MUL/MULS/MULSU/FMUL* */
#define AVR_FLAG_HAS_JMP_CALL  0x02  /* 22-bit JMP/CALL */
#define AVR_FLAG_HAS_MOVW      0x04  /* MOVW instruction */
#define AVR_FLAG_HAS_LPM_RD    0x08  /* LPM Rd,Z / LPM Rd,Z+ */
#define AVR_FLAG_HAS_ADIW      0x10  /* ADIW/SBIW */
#define AVR_FLAG_HAS_BREAK     0x20  /* BREAK (debug) */

typedef struct {
    const char *name;
    uint32_t flash_size;     /* bytes (e.g. 32768 for 328P, 8192 for tiny85) */
    uint16_t data_size;      /* total data address space in bytes */
    uint16_t sram_start;     /* first SRAM byte in data space */
    uint16_t eeprom_size;    /* bytes */
    uint8_t  num_vectors;    /* number of interrupt vectors (incl. reset) */
    uint8_t  vector_size;    /* words per vector (2 for JMP, 1 for RJMP) */
    uint8_t  flags;          /* AVR_FLAG_* bitmask */
    /* Variant-specific peripheral init */
    void (*periph_init)(avr_cpu_t *cpu);
} avr_variant_t;

/* ---------- CPU state flags ---------- */

enum {
    AVR_STATE_RUNNING  = 0,
    AVR_STATE_SLEEPING = 1,
    AVR_STATE_HALTED   = 2,  /* illegal instruction or error */
    AVR_STATE_BREAK    = 3,  /* BREAK instruction (GDB) */
};

/* ---------- SREG bit positions ---------- */

#define SREG_C  0x01  /* bit 0: Carry */
#define SREG_Z  0x02  /* bit 1: Zero */
#define SREG_N  0x04  /* bit 2: Negative */
#define SREG_V  0x08  /* bit 3: Two's complement overflow */
#define SREG_S  0x10  /* bit 4: Sign (N xor V) */
#define SREG_H  0x20  /* bit 5: Half carry */
#define SREG_T  0x40  /* bit 6: Bit copy storage */
#define SREG_I  0x80  /* bit 7: Global interrupt enable */

/* ---------- I/O handler callbacks ---------- */

typedef uint8_t (*io_read_fn)(avr_cpu_t *cpu, uint8_t io_addr, void *ctx);
typedef void (*io_write_fn)(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val, void *ctx);

/* Max I/O address space (64 standard + 160 extended for 328P) */
#define AVR_IO_MAX 224

/* ---------- AVR CPU state ---------- */

struct avr_cpu {
    /* Core registers */
    uint16_t pc;             /* Program counter (word address) */
    uint8_t  sreg;           /* Status register (kept separate for speed) */
    uint16_t sp;             /* Stack pointer */
    uint64_t cycles;         /* Total cycles executed */
    volatile uint8_t state;  /* AVR_STATE_* — volatile for cross-core access */
    uint8_t  skip_next;      /* 1 = skip next instruction (CPSE/SBRC/SBRS/SBIC/SBIS) */
    uint16_t periph_accum;   /* cycles since last peripheral tick (for batching) */

    /* Program memory (word-addressed, not copied to RAM on ESP32) */
    const uint16_t *flash;
    uint32_t flash_size;     /* in bytes */

    /* Variant config */
    const avr_variant_t *variant;

    /* I/O register dispatch tables (indexed by I/O address 0x00..0xDF) */
    io_read_fn  io_read[AVR_IO_MAX];
    io_write_fn io_write[AVR_IO_MAX];
    void       *io_ctx[AVR_IO_MAX];

    /* Interrupt pending flags (one bit per vector, bit 0 unused/reset) */
    uint32_t irq_pending;

    /* Peripheral state pointers (set by variant periph_init) */
    void *periph_timer;
    void *periph_gpio;
    void *periph_uart;
    void *periph_twi;

    /* I/O bridge callback */
    io_bridge_cb_t bridge_cb;
    void *bridge_ctx;

    /* Data memory: flexible array member
     * Layout: [0x00..0x1F] = R0-R31
     *         [0x20..0x5F] = I/O registers
     *         [0x60..0xFF] = Extended I/O (328P) or SRAM start (tiny85)
     *         [sram_start..data_size-1] = SRAM
     */
    uint8_t data[];
};

/* ---------- Register access macros ---------- */

#define AVR_R(cpu, n)      ((cpu)->data[(n)])

/* 16-bit register pairs (little-endian: low byte at even address) */
#define AVR_X(cpu)         ((uint16_t)(AVR_R(cpu, 26) | (AVR_R(cpu, 27) << 8)))
#define AVR_Y(cpu)         ((uint16_t)(AVR_R(cpu, 28) | (AVR_R(cpu, 29) << 8)))
#define AVR_Z(cpu)         ((uint16_t)(AVR_R(cpu, 30) | (AVR_R(cpu, 31) << 8)))

#define AVR_SET_X(cpu, v)  do { AVR_R(cpu, 26) = (v) & 0xFF; AVR_R(cpu, 27) = (v) >> 8; } while(0)
#define AVR_SET_Y(cpu, v)  do { AVR_R(cpu, 28) = (v) & 0xFF; AVR_R(cpu, 29) = (v) >> 8; } while(0)
#define AVR_SET_Z(cpu, v)  do { AVR_R(cpu, 30) = (v) & 0xFF; AVR_R(cpu, 31) = (v) >> 8; } while(0)

/* Word register pairs for ADIW/SBIW (R24:25, R26:27, R28:29, R30:31) */
#define AVR_REGW(cpu, n)   ((uint16_t)(AVR_R(cpu, (n)) | (AVR_R(cpu, (n)+1) << 8)))
#define AVR_SET_REGW(cpu, n, v) do { \
    AVR_R(cpu, (n)) = (v) & 0xFF;   \
    AVR_R(cpu, (n)+1) = (v) >> 8;   \
} while(0)

/* ---------- API ---------- */

/* Allocate and initialize a CPU for the given variant.
 * flash: pointer to program memory (word array, flash_size/2 words)
 * flash_size: flash size in bytes
 * Returns NULL on allocation failure. */
avr_cpu_t *avr_cpu_init(const avr_variant_t *variant,
                        const uint16_t *flash, uint32_t flash_size);

/* Free CPU state */
void avr_cpu_free(avr_cpu_t *cpu);

/* Reset CPU to initial state (PC=0, SP=top of SRAM, etc.) */
void avr_cpu_reset(avr_cpu_t *cpu);

/* Execute one instruction. Returns number of cycles consumed. */
uint8_t avr_cpu_step(avr_cpu_t *cpu);

/* Check and dispatch pending interrupts. Called after each step. */
void avr_cpu_check_irq(avr_cpu_t *cpu);

/* Register an I/O handler for a specific I/O address (0x00-0xDF) */
void avr_io_register(avr_cpu_t *cpu, uint8_t io_addr,
                     io_read_fn read, io_write_fn write, void *ctx);

/* Data space read/write (handles register file, I/O dispatch, SRAM) */
uint8_t avr_data_read(avr_cpu_t *cpu, uint16_t addr);
void avr_data_write(avr_cpu_t *cpu, uint16_t addr, uint8_t val);

/* I/O space read/write (for IN/OUT instructions, io_addr 0x00-0x3F) */
uint8_t avr_io_read(avr_cpu_t *cpu, uint8_t io_addr);
void avr_io_write(avr_cpu_t *cpu, uint8_t io_addr, uint8_t val);

/* Flash read (byte-addressed, for LPM instruction) */
uint8_t avr_flash_read_byte(avr_cpu_t *cpu, uint16_t byte_addr);

/* Instruction decode+execute (called by avr_cpu_step) */
uint8_t avr_decode_execute(avr_cpu_t *cpu);

/* ---------- Variant descriptors (defined in variants/) ---------- */

extern const avr_variant_t avr_atmega328p;
extern const avr_variant_t avr_attiny85;

#endif /* AVR_CPU_H */
