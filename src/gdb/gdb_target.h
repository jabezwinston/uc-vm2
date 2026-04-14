/*
 * ucvm - Architecture-neutral GDB target interface
 *
 * Both AVR and 8051 implement this interface. The GDB RSP stub
 * dispatches through these function pointers.
 */
#ifndef GDB_TARGET_H
#define GDB_TARGET_H

#include <stdint.h>

typedef struct {
    /* Read all registers into hex-encoded buf. Returns string length. */
    int (*read_regs)(void *cpu, char *buf, int buf_size);
    /* Write all registers from hex-encoded buf. */
    void (*write_regs)(void *cpu, const char *buf);
    /* Read memory byte. addr may include address-space prefix (0x800000=data for AVR). */
    uint8_t (*read_mem)(void *cpu, uint32_t addr);
    /* Write memory byte. */
    void (*write_mem)(void *cpu, uint32_t addr, uint8_t val);
    /* Get/set program counter (byte address). */
    uint32_t (*get_pc)(void *cpu);
    void (*set_pc)(void *cpu, uint32_t addr);
    /* Get/set CPU state (RUNNING/HALTED/BREAK/SLEEPING). */
    uint8_t (*get_state)(void *cpu);
    void (*set_state)(void *cpu, uint8_t state);
    /* Execute one instruction. Returns cycles. */
    uint8_t (*step)(void *cpu);
    /* Reset CPU to initial state. */
    void (*reset)(void *cpu);
} gdb_target_ops_t;

/* Pre-built target ops for each architecture */
extern const gdb_target_ops_t gdb_target_avr;
extern const gdb_target_ops_t gdb_target_mcs51;

#endif /* GDB_TARGET_H */
