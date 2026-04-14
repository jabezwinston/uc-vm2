/*
 * ucvm - ESP32 Web Configuration Server
 *
 * HTTP server for:
 *   - Status monitoring (cycles, state, GPIO)
 *   - I/O bridge configuration
 *   - Firmware upload
 *   - CPU control (reset, halt)
 */
#ifndef ESP_WEBSERVER_H
#define ESP_WEBSERVER_H

#include "src/io/io_bridge.h"
#include <stdint.h>

/* Start the HTTP server.
 * cpu: pointer to CPU state (avr_cpu_t* or mcs51_cpu_t*)
 * arch: 0 = AVR, 1 = MCS-51
 * config: pointer to I/O bridge config (for read/update)
 * Returns 0 on success, -1 on error. */
int webserver_start(void *cpu, int arch, io_bridge_config_t *config);

/* Stop the HTTP server */
void webserver_stop(void);

#endif /* ESP_WEBSERVER_H */
