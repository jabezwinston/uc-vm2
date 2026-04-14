/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Maps emulated MCU peripheral resources to ESP32 hardware.
 * Architecture-neutral: works with both AVR and 8051.
 *
 *   GPIO: MCU port pins → ESP32 GPIO pins (input/output, ISR)
 *   UART: MCU USART → ESP32 HW UART
 *   ADC:  MCU ADC channels → ESP32 ADC oneshot reads
 */
#ifndef ESP_IO_BRIDGE_H
#define ESP_IO_BRIDGE_H

#include "src/io/io_bridge.h"
#include <stdint.h>

/* Architecture IDs (match main.c ARCH_AVR/ARCH_MCS51) */
#define ESP_BRIDGE_ARCH_AVR   0
#define ESP_BRIDGE_ARCH_MCS51 1

/* Initialize all ESP32 I/O bridge backends from a bridge config.
 * cpu: opaque pointer (avr_cpu_t* or mcs51_cpu_t*)
 * arch: ESP_BRIDGE_ARCH_AVR or ESP_BRIDGE_ARCH_MCS51
 * Configures GPIO, UART, ADC and installs the bridge callback on the CPU. */
void esp_bridge_init(void *cpu, int arch, const io_bridge_config_t *config);

/* Tear down all bridge backends */
void esp_bridge_deinit(void);

/* Poll bridge inputs and drain UART TX.
 * Called periodically from core 0. */
void esp_bridge_poll(void);

#endif /* ESP_IO_BRIDGE_H */
