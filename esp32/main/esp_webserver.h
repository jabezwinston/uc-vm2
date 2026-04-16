/*
 * ucvm - ESP32 Web Configuration Server
 *
 * HTTP server for CPU status, bridge config, firmware upload.
 * Fully arch-neutral — all CPU access via bridge->mcu_ops.
 */
#ifndef ESP_WEBSERVER_H
#define ESP_WEBSERVER_H

#include "src/io/io_bridge.h"

int  webserver_start(io_bridge_t *br);
void webserver_stop(void);

#endif /* ESP_WEBSERVER_H */
