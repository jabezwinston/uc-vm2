/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Opens ESP32 hardware (GPIO, UART, ADC, I2C) for each bridge entry
 * and routes data between the emulated MCU and host peripherals.
 * Architecture-neutral — all MCU access via bridge->mcu_ops.
 */
#ifndef ESP_IO_BRIDGE_H
#define ESP_IO_BRIDGE_H

#include "src/io/io_bridge.h"

/* Open host resources for all entries and install bridge callback. */
void esp_bridge_init(io_bridge_t *br);

/* Close all host resources. */
void esp_bridge_deinit(io_bridge_t *br);

/* Poll host→MCU data transfer (UART RX, GPIO input, etc.).
 * Called periodically from core 0. */
void esp_bridge_poll(io_bridge_t *br);

/* Close the host resource handle for entry at index.
 * Call before io_bridge_remove() so the handle is cleaned up. */
void esp_bridge_close_entry(int index);

#endif /* ESP_IO_BRIDGE_H */
