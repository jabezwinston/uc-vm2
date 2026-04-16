/*
 * ucvm - GDB Remote Serial Protocol stub
 *
 * Architecture-neutral: uses gdb_target_ops_t for register/memory access.
 * Runs over TCP on PC, LWIP sockets on ESP32.
 */
#ifndef GDB_STUB_H
#define GDB_STUB_H

#include <stdint.h>
#include "gdb_target.h"

typedef struct gdb_state gdb_state_t;

#define GDB_MAX_BREAKPOINTS 8

/* Initialize GDB stub with target ops.
 * cpu: opaque pointer to CPU state (avr_cpu_t* or mcs51_cpu_t*)
 * ops: architecture-specific callbacks
 * port: TCP listen port
 * Returns NULL on failure. */
gdb_state_t *gdb_init(void *cpu, const gdb_target_ops_t *ops, int port);

/* Legacy init for AVR (backward compat — wraps gdb_init with avr ops) */
gdb_state_t *gdb_init_avr(void *cpu, const uint16_t *flash,
                            uint32_t flash_words, int port);

void gdb_poll(gdb_state_t *gdb);
int gdb_has_client(gdb_state_t *gdb);
int gdb_is_running(gdb_state_t *gdb);
int gdb_is_single_stepping(gdb_state_t *gdb);
int gdb_check_breakpoint(gdb_state_t *gdb, uint16_t pc);
void gdb_notify_stop(gdb_state_t *gdb, int reason);
int gdb_should_stop(gdb_state_t *gdb);
void gdb_free(gdb_state_t *gdb);

#endif /* GDB_STUB_H */
