/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Maps emulated MCU peripheral resources to ESP32 hardware:
 *   GPIO: MCU port pins → ESP32 GPIO pins (input/output, ISR)
 *   UART: MCU USART → ESP32 HW UART or TCP socket
 *   ADC:  MCU ADC channels → ESP32 ADC oneshot reads
 *
 * Currently AVR-specific for GPIO polling and UART drain.
 * 8051 UART TX goes through bridge callback directly.
 */
#ifndef ESP_IO_BRIDGE_H
#define ESP_IO_BRIDGE_H

#include "src/io/io_bridge.h"

#ifdef CONFIG_UCVM_ENABLE_AVR
#include "src/avr/avr_cpu.h"

void esp_bridge_init(avr_cpu_t *cpu, const io_bridge_config_t *config);
void esp_bridge_deinit(void);
void esp_bridge_poll(avr_cpu_t *cpu);
int  esp_bridge_uart_tx_pop(avr_cpu_t *cpu);
void esp_bridge_uart_rx_push(avr_cpu_t *cpu, uint8_t byte);

#else
/* Stubs when AVR is disabled */
static inline void esp_bridge_deinit(void) {}
#endif

#endif /* ESP_IO_BRIDGE_H */
