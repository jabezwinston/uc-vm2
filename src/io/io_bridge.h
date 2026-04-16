/*
 * ucvm - I/O Bridge
 *
 * Maps emulated MCU peripherals to host hardware resources.
 * Each entry connects one MCU peripheral (GPIO pin, UART channel, etc.)
 * to one host resource (GPIO pin, UART port, I2C bus, ADC channel).
 *
 *   MCU UART ch0  <-->  Host UART port 1  @ 115200
 *   MCU GPIO B.5  <-->  Host GPIO pin 21
 *   MCU I2C  #0   <-->  Host I2C  port 0  @ 100 kHz
 */
#ifndef IO_BRIDGE_H
#define IO_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

/* ---- Bridge callback ----
 * Fired by emulated peripherals when output changes.
 *   type:     io_periph_type_t
 *   resource: port / channel / instance index
 *   value:    output value (port byte for GPIO, TX byte for UART) */
typedef void (*io_bridge_cb_t)(void *ctx, uint8_t type,
                                uint8_t resource, uint8_t value);

/* ---- MCU peripheral types ---- */
typedef enum {
    IO_PERIPH_GPIO  = 0,
    IO_PERIPH_UART  = 1,
    IO_PERIPH_SPI   = 2,
    IO_PERIPH_I2C   = 3,
    IO_PERIPH_ADC   = 4,
    IO_PERIPH_PWM   = 5,
    IO_PERIPH_TIMER = 6,
} io_periph_type_t;

/* ---- Host resource types ---- */
typedef enum {
    IO_HOST_GPIO = 0,
    IO_HOST_UART = 1,
    IO_HOST_I2C  = 2,
    IO_HOST_ADC  = 3,
} io_host_type_t;

/* ---- Entry flags ---- */
#define IO_BF_INVERT  0x01
#define IO_BF_PULLUP  0x02

/* ---- Bridge mapping entry ----
 * Connects one MCU peripheral to one host resource.
 *
 *   GPIO: mcu_index=port_id, mcu_pin=bit, host_index=pin#
 *   UART: mcu_index=channel, host_index=uart_port, param=baud/100
 *   I2C:  mcu_index=instance, host_index=i2c_port,  param=speed_hz/100
 *   ADC:  mcu_index=channel,  host_index=adc_channel
 *
 * param stores rate / 100 to fit in 16 bits (e.g. 115200 → 1152). */
typedef struct {
    uint8_t  mcu_periph;   /* io_periph_type_t */
    uint8_t  mcu_index;    /* port / channel / instance */
    uint8_t  mcu_pin;      /* pin within port (GPIO), 0 otherwise */
    uint8_t  host_type;    /* io_host_type_t */
    uint8_t  host_index;   /* host pin / port / channel */
    uint8_t  flags;        /* IO_BF_* */
    uint16_t param;        /* baud/100, speed/100, or 0 */
} io_bridge_entry_t;       /* 8 bytes */

/* ================================================================
 *  MCU ops vtable — arch-neutral access to emulated CPU
 *
 *  Each architecture provides one instance (avr_mcu_ops, mcs51_mcu_ops).
 *  Platform code and the webserver use this to avoid any #ifdef.
 * ================================================================ */

typedef struct {
    const char *arch_name;                              /* "avr", "8051" */
    uint8_t     (*get_state)(void *cpu);                /* 0=run,1=sleep,2=halt,3=break */
    void        (*set_state)(void *cpu, uint8_t state);
    uint64_t    (*get_cycles)(void *cpu);
    uint16_t    (*get_pc)(void *cpu);
    const char *(*get_variant)(void *cpu);              /* e.g. "ATmega328P" */
    void        (*reset)(void *cpu);
    int         (*load_firmware)(void *cpu, const char *hex_path);

    void (*install_bridge_cb)(void *cpu, io_bridge_cb_t cb, void *ctx);

    void     (*uart_rx_push)(void *cpu, uint8_t channel, uint8_t byte);
    int      (*uart_tx_pop)(void *cpu, uint8_t channel);
    uint8_t *(*gpio_ext_pins)(void *cpu);
    void     (*i2c_attach_bus)(void *cpu, uint8_t instance, void *bus_ops);
} io_mcu_ops_t;

extern const io_mcu_ops_t avr_mcu_ops;
extern const io_mcu_ops_t mcs51_mcu_ops;

/* ---- Bridge ---- */

#define IO_BRIDGE_MAX_ENTRIES 8

typedef struct {
    io_bridge_entry_t   entries[IO_BRIDGE_MAX_ENTRIES];
    uint8_t             num_entries;
    void               *cpu;
    const io_mcu_ops_t *mcu_ops;
} io_bridge_t;

/* Lifecycle */
int  io_bridge_init(io_bridge_t *br, void *cpu, const io_mcu_ops_t *mcu_ops);

/* Entry management */
int  io_bridge_add(io_bridge_t *br, const io_bridge_entry_t *entry);
int  io_bridge_remove(io_bridge_t *br, int index);

/* NVS / flash serialization */
int  io_bridge_serialize(const io_bridge_t *br, uint8_t *buf, uint32_t len);
int  io_bridge_parse(const uint8_t *data, uint32_t len, io_bridge_t *br);

#endif /* IO_BRIDGE_H */
