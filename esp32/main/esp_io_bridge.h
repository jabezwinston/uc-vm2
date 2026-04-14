/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Maps AVR peripheral resources to ESP32 hardware:
 *   GPIO: AVR port pins → ESP32 GPIO pins (input/output, ISR for interrupts)
 *   UART: AVR USART → ESP32 HW UART or TCP socket
 *   ADC:  AVR ADC channels → ESP32 ADC oneshot reads
 */
#ifndef ESP_IO_BRIDGE_H
#define ESP_IO_BRIDGE_H

#include "src/core/avr_cpu.h"
#include "src/io/io_bridge.h"

/* Initialize all ESP32 I/O bridge backends from a bridge config.
 * Configures GPIO pins, UART drivers, ADC channels as specified.
 * Installs the bridge callback on the CPU. */
void esp_bridge_init(avr_cpu_t *cpu, const io_bridge_config_t *config);

/* Tear down all bridge backends (free drivers, remove ISRs) */
void esp_bridge_deinit(void);

/* Poll bridge inputs: read GPIO input states and ADC values,
 * push into AVR peripheral state. Called periodically from core 0. */
void esp_bridge_poll(avr_cpu_t *cpu);

/* Get UART TX byte from AVR (for forwarding to ESP32 UART).
 * Called from emu_task or bridge poll. Returns -1 if none. */
int esp_bridge_uart_tx_pop(avr_cpu_t *cpu);

/* Push a byte into AVR UART RX (received from ESP32 UART).
 * Called from bridge poll. */
void esp_bridge_uart_rx_push(avr_cpu_t *cpu, uint8_t byte);

#endif /* ESP_IO_BRIDGE_H */
