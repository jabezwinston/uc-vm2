/*
 * ucvm - GDB Remote Serial Protocol stub
 *
 * Implements minimal RSP subset for avr-gdb debugging.
 * Runs over TCP on PC, could be adapted for UART on ESP32.
 */
#ifndef GDB_STUB_H
#define GDB_STUB_H

#include <stdint.h>

/* Forward declarations */
typedef struct avr_cpu avr_cpu_t;
typedef struct gdb_state gdb_state_t;

/* Max breakpoints */
#define GDB_MAX_BREAKPOINTS 8

/* Initialize GDB stub. Starts TCP listener on given port.
 * Returns NULL on failure. */
gdb_state_t *gdb_init(avr_cpu_t *cpu, const uint16_t *flash,
                       uint32_t flash_words, int port);

/* Wait for a GDB client to connect (blocking). */
void gdb_wait_connect(gdb_state_t *gdb);

/* Poll for and handle GDB packets. Non-blocking. */
void gdb_poll(gdb_state_t *gdb);

/* Check if emulation should continue running */
int gdb_is_running(gdb_state_t *gdb);

/* Check if single-stepping */
int gdb_is_single_stepping(gdb_state_t *gdb);

/* Check if PC matches a breakpoint */
int gdb_check_breakpoint(gdb_state_t *gdb, uint16_t pc);

/* Notify GDB that execution stopped */
void gdb_notify_stop(gdb_state_t *gdb, int reason);

/* Check if GDB requested disconnection */
int gdb_should_stop(gdb_state_t *gdb);

/* Cleanup */
void gdb_free(gdb_state_t *gdb);

#endif /* GDB_STUB_H */
