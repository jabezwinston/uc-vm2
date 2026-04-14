/*
 * ucvm - I/O Bridge Framework
 *
 * Maps AVR peripheral resources to host platform resources.
 * Configuration stored in compact binary format (flash-friendly).
 */
#ifndef IO_BRIDGE_H
#define IO_BRIDGE_H

#include <stdint.h>

/* Bridge entry types */
#define IO_BRIDGE_GPIO  1
#define IO_BRIDGE_UART  2
#define IO_BRIDGE_ADC   3
#define IO_BRIDGE_SPI   4
#define IO_BRIDGE_I2C   5
#define IO_BRIDGE_PWM   6

/* Bridge entry flags */
#define IO_BRIDGE_FLAG_INVERT   0x01
#define IO_BRIDGE_FLAG_PULLUP   0x02
#define IO_BRIDGE_FLAG_IRQ_RISE 0x04
#define IO_BRIDGE_FLAG_IRQ_FALL 0x08

/* UART host resource sub-types */
#define IO_BRIDGE_UART_HW      0  /* Hardware UART */
#define IO_BRIDGE_UART_BITBANG 1  /* Bit-banged UART */
#define IO_BRIDGE_UART_TCP     2  /* TCP socket */
#define IO_BRIDGE_UART_UDP     3  /* UDP socket */
#define IO_BRIDGE_UART_CONSOLE 4  /* Console (stdout/stdin) */

/* Binary config format — all packed, stored in flash */
#define IO_BRIDGE_MAGIC "UCIO"
#define IO_BRIDGE_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t magic[4];       /* "UCIO" */
    uint8_t version;        /* IO_BRIDGE_VERSION */
    uint8_t num_entries;    /* number of bridge entries */
} io_bridge_header_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           /* IO_BRIDGE_GPIO, _UART, _ADC, etc. */
    uint8_t avr_resource;   /* GPIO: (port_id<<4)|pin. UART: channel. ADC: channel. */
    uint8_t host_resource;  /* GPIO: host pin#. UART: sub-type. ADC: host channel. */
    uint8_t flags;          /* IO_BRIDGE_FLAG_* */
} io_bridge_entry_t;

/* Runtime bridge configuration */
#define IO_BRIDGE_MAX_ENTRIES 32

typedef struct {
    io_bridge_entry_t entries[IO_BRIDGE_MAX_ENTRIES];
    uint8_t num_entries;
} io_bridge_config_t;

/* Parse binary config from buffer. Returns 0 on success, -1 on error. */
int io_bridge_parse(const uint8_t *data, uint32_t len, io_bridge_config_t *out);

/* Serialize config to binary format. Returns bytes written, or -1 on error.
 * buf must be at least 6 + 4*num_entries bytes. */
int io_bridge_serialize(const io_bridge_config_t *config, uint8_t *buf, uint32_t buf_len);

/* Find bridge entry for a given AVR resource.
 * Returns pointer to entry, or NULL if not found. */
const io_bridge_entry_t *io_bridge_find(const io_bridge_config_t *config,
                                         uint8_t type, uint8_t avr_resource);

#endif /* IO_BRIDGE_H */
