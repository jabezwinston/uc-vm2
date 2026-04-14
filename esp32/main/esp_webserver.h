/*
 * ucvm - ESP32 Web Configuration Server
 *
 * HTTP server for:
 *   - Status monitoring (cycles, state, GPIO)
 *   - I/O bridge configuration
 *   - AVR firmware upload
 *   - CPU control (reset, halt)
 */
#ifndef ESP_WEBSERVER_H
#define ESP_WEBSERVER_H

#include "src/core/avr_cpu.h"
#include "src/io/io_bridge.h"

/* Start the HTTP server.
 * cpu: pointer to AVR CPU state (for status/control)
 * config: pointer to I/O bridge config (for read/update)
 * Returns 0 on success, -1 on error. */
int webserver_start(avr_cpu_t *cpu, io_bridge_config_t *config);

/* Stop the HTTP server */
void webserver_stop(void);

#endif /* ESP_WEBSERVER_H */
