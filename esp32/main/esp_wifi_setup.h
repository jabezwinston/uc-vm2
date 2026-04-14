/*
 * ucvm - ESP32 WiFi Station Setup
 */
#ifndef ESP_WIFI_SETUP_H
#define ESP_WIFI_SETUP_H

#include "esp_err.h"

/* Initialize WiFi in station mode.
 * SSID and password are read from Kconfig (menuconfig).
 * Blocks until connected or max retries exceeded.
 * Returns ESP_OK on success. */
esp_err_t wifi_init_sta(void);

/* Get the current IP address as a string.
 * Returns pointer to static buffer, or "0.0.0.0" if not connected. */
const char *wifi_get_ip(void);

/* Check if WiFi is connected */
int wifi_is_connected(void);

#endif /* ESP_WIFI_SETUP_H */
