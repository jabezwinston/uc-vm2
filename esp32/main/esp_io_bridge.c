/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Opens ESP32 hardware for each bridge entry and routes data
 * between the emulated MCU and host peripherals.
 * Architecture-neutral — all MCU access via bridge->mcu_ops.
 */
#include "esp_io_bridge.h"
#include "src/io/io_bridge.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static const char *TAG = "bridge";

/* ================================================================
 *  Per-entry runtime handle
 * ================================================================ */

typedef struct esp_handle esp_handle_t;

struct esp_handle {
    uint8_t type;  /* io_host_type_t */
    union {
        struct {
            gpio_num_t pin;
        } gpio;
        struct {
            uart_port_t num;
        } uart;
        struct {
            adc_oneshot_unit_handle_t handle;
            adc_channel_t channel;
        } adc;
        struct {
            i2c_master_bus_handle_t   bus;
            i2c_master_dev_handle_t   dev;
            uint32_t speed_hz;
            uint8_t  tx_buf[128];  int tx_len;
            uint8_t  rx_buf[128];  int rx_len, rx_pos;
            uint8_t  slave_addr;
            int      is_read;
        } i2c;
    };
};

static esp_handle_t *s_handles[IO_BRIDGE_MAX_ENTRIES];

/* Default I2C pins (ESP32 DevKit) */
static struct { uint8_t sda, scl; } s_i2c_pins[2] = {
    { 21, 22 }, { 25, 26 }
};

/* ================================================================
 *  I2C bus-level ops (passed to mcu_ops->i2c_attach_bus)
 *
 *  This struct must match avr_twi_bus_t layout:
 *    { start, write_byte, read_byte, stop, ctx }
 * ================================================================ */

typedef struct {
    int  (*start)(void *ctx);
    int  (*write_byte)(void *ctx, uint8_t byte);
    int  (*read_byte)(void *ctx, int send_ack);
    void (*stop)(void *ctx);
    void *ctx;
} i2c_bus_ops_t;

static int i2c_bus_start(void *ctx)
{
    esp_handle_t *h = ctx;
    h->i2c.tx_len = h->i2c.rx_len = h->i2c.rx_pos = 0;
    h->i2c.slave_addr = 0;
    h->i2c.is_read = 0;
    return 0;
}

static int i2c_bus_write(void *ctx, uint8_t byte)
{
    esp_handle_t *h = ctx;
    if (h->i2c.slave_addr == 0) {
        /* First byte = address + R/W bit */
        h->i2c.slave_addr = byte >> 1;
        h->i2c.is_read = byte & 1;
        if (h->i2c.dev) {
            i2c_master_bus_rm_device(h->i2c.dev);
            h->i2c.dev = NULL;
        }
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = h->i2c.slave_addr,
            .scl_speed_hz    = h->i2c.speed_hz,
        };
        if (i2c_master_bus_add_device(h->i2c.bus, &cfg, &h->i2c.dev) != ESP_OK)
            return 0;
        if (h->i2c.is_read && h->i2c.tx_len > 0) {
            if (i2c_master_transmit_receive(h->i2c.dev,
                    h->i2c.tx_buf, h->i2c.tx_len,
                    h->i2c.rx_buf, sizeof(h->i2c.rx_buf), 100) != ESP_OK) {
                h->i2c.rx_len = 0;
                return 0;
            }
            h->i2c.rx_len = (int)sizeof(h->i2c.rx_buf);
            h->i2c.rx_pos = 0;
            h->i2c.tx_len = 0;
        }
        return 1;  /* ACK */
    }
    if (h->i2c.tx_len < (int)sizeof(h->i2c.tx_buf)) {
        h->i2c.tx_buf[h->i2c.tx_len++] = byte;
        return 1;
    }
    return 0;
}

static int i2c_bus_read(void *ctx, int ack)
{
    esp_handle_t *h = ctx;
    (void)ack;
    if (h->i2c.rx_pos < h->i2c.rx_len)
        return h->i2c.rx_buf[h->i2c.rx_pos++];
    if (h->i2c.dev) {
        uint8_t byte;
        if (i2c_master_receive(h->i2c.dev, &byte, 1, 100) == ESP_OK)
            return byte;
    }
    return 0xFF;
}

static void i2c_bus_stop(void *ctx)
{
    esp_handle_t *h = ctx;
    if (!h->i2c.is_read && h->i2c.tx_len > 0 && h->i2c.dev)
        i2c_master_transmit(h->i2c.dev, h->i2c.tx_buf, h->i2c.tx_len, 100);
    h->i2c.tx_len = 0;
}

/* ================================================================
 *  Open / close host resources
 * ================================================================ */

static esp_handle_t *open_gpio(const io_bridge_entry_t *e)
{
    esp_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = IO_HOST_GPIO;
    h->gpio.pin = (gpio_num_t)e->host_index;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << h->gpio.pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = (e->flags & IO_BF_PULLUP)
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "GPIO: pin %d", h->gpio.pin);
    return h;
}

static esp_handle_t *open_uart(const io_bridge_entry_t *e)
{
    esp_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = IO_HOST_UART;
    h->uart.num = (uart_port_t)e->host_index;

    if (h->uart.num == UART_NUM_0) {
        /* UART0 = console — use stdio, no driver needed */
        ESP_LOGI(TAG, "UART: console (UART0)");
    } else {
        uint32_t baud = e->param ? (uint32_t)e->param * 100 : 9600;
        uart_config_t cfg = {
            .baud_rate  = (int)baud,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_param_config(h->uart.num, &cfg) != ESP_OK ||
            uart_driver_install(h->uart.num, 256, 256, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "UART%d init failed", h->uart.num);
            free(h);
            return NULL;
        }
        /* UART1 defaults (GPIO 9/10) conflict with flash */
        int tx_pin = (h->uart.num == UART_NUM_1) ? 4  : 17;
        int rx_pin = (h->uart.num == UART_NUM_1) ? 5  : 16;
        uart_set_pin(h->uart.num, tx_pin, rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGI(TAG, "UART: port %d (TX=%d RX=%d) @ %lu baud",
                 h->uart.num, tx_pin, rx_pin, (unsigned long)baud);
    }
    return h;
}

static esp_handle_t *open_adc(const io_bridge_entry_t *e)
{
    esp_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = IO_HOST_ADC;
    h->adc.channel = (adc_channel_t)e->host_index;

    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_cfg, &h->adc.handle) != ESP_OK) {
        free(h);
        return NULL;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(h->adc.handle, h->adc.channel, &chan_cfg);
    ESP_LOGI(TAG, "ADC: channel %d", h->adc.channel);
    return h;
}

static esp_handle_t *open_i2c(const io_bridge_entry_t *e, io_bridge_t *br)
{
    esp_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = IO_HOST_I2C;

    uint8_t port = e->host_index;
    uint8_t sda  = s_i2c_pins[port].sda;
    uint8_t scl  = s_i2c_pins[port].scl;
    h->i2c.speed_hz = e->param ? (uint32_t)e->param * 100 : 100000;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = (i2c_port_num_t)port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &h->i2c.bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C%d bus init failed", port);
        free(h);
        return NULL;
    }

    /* Attach to MCU I2C peripheral via mcu_ops */
    if (br->mcu_ops && br->mcu_ops->i2c_attach_bus) {
        i2c_bus_ops_t *ops = calloc(1, sizeof(*ops));
        ops->start      = i2c_bus_start;
        ops->write_byte = i2c_bus_write;
        ops->read_byte  = i2c_bus_read;
        ops->stop       = i2c_bus_stop;
        ops->ctx        = h;
        br->mcu_ops->i2c_attach_bus(br->cpu, e->mcu_index, ops);
    }

    ESP_LOGI(TAG, "I2C: port %d (SDA=%d SCL=%d) @ %lu Hz",
             port, sda, scl, (unsigned long)h->i2c.speed_hz);
    return h;
}

static esp_handle_t *open_entry(const io_bridge_entry_t *e, io_bridge_t *br)
{
    switch (e->host_type) {
    case IO_HOST_GPIO: return open_gpio(e);
    case IO_HOST_UART: return open_uart(e);
    case IO_HOST_ADC:  return open_adc(e);
    case IO_HOST_I2C:  return open_i2c(e, br);
    default:           return NULL;
    }
}

static void close_handle(esp_handle_t *h)
{
    if (!h) return;
    switch (h->type) {
    case IO_HOST_GPIO:
        gpio_reset_pin(h->gpio.pin);
        break;
    case IO_HOST_UART:
        if (h->uart.num != UART_NUM_0)
            uart_driver_delete(h->uart.num);
        break;
    case IO_HOST_ADC:
        adc_oneshot_del_unit(h->adc.handle);
        break;
    case IO_HOST_I2C:
        if (h->i2c.dev) i2c_master_bus_rm_device(h->i2c.dev);
        if (h->i2c.bus) i2c_del_master_bus(h->i2c.bus);
        break;
    }
    free(h);
}

/* ================================================================
 *  Bridge callback — MCU output → host hardware
 * ================================================================ */

static void esp_bridge_cb(void *ctx, uint8_t periph_type,
                           uint8_t resource, uint8_t value)
{
    io_bridge_t *br = ctx;
    for (int i = 0; i < br->num_entries; i++) {
        const io_bridge_entry_t *e = &br->entries[i];
        if (e->mcu_periph != periph_type || e->mcu_index != resource)
            continue;
        esp_handle_t *h = s_handles[i];
        if (!h) continue;

        /* GPIO: instant signal propagation via callback.
         * UART: handled by poll (uart_tx_pop) to avoid double-send. */
        if (e->host_type == IO_HOST_GPIO) {
            int level = (value >> e->mcu_pin) & 1;
            if (e->flags & IO_BF_INVERT) level ^= 1;
            gpio_set_level(h->gpio.pin, level);
        }
    }
}

/* ================================================================
 *  Public API
 * ================================================================ */

void esp_bridge_init(io_bridge_t *br)
{
    gpio_install_isr_service(0);

    /* Open host resources for each entry */
    for (int i = 0; i < br->num_entries; i++)
        s_handles[i] = open_entry(&br->entries[i], br);

    /* Install bridge callback via mcu_ops */
    if (br->mcu_ops && br->mcu_ops->install_bridge_cb)
        br->mcu_ops->install_bridge_cb(br->cpu, esp_bridge_cb, br);

    ESP_LOGI(TAG, "Bridge: %d entries active", br->num_entries);
}

void esp_bridge_deinit(io_bridge_t *br)
{
    (void)br;
    for (int i = 0; i < IO_BRIDGE_MAX_ENTRIES; i++) {
        if (s_handles[i]) {
            close_handle(s_handles[i]);
            s_handles[i] = NULL;
        }
    }
    gpio_uninstall_isr_service();
}

void esp_bridge_close_entry(int index)
{
    if (index >= 0 && index < IO_BRIDGE_MAX_ENTRIES && s_handles[index]) {
        close_handle(s_handles[index]);
        /* Shift handles down to match entry array after removal */
        for (int i = index; i < IO_BRIDGE_MAX_ENTRIES - 1; i++)
            s_handles[i] = s_handles[i + 1];
        s_handles[IO_BRIDGE_MAX_ENTRIES - 1] = NULL;
    }
}

void esp_bridge_poll(io_bridge_t *br)
{
    if (!br || !br->mcu_ops) return;

    for (int i = 0; i < br->num_entries; i++) {
        const io_bridge_entry_t *e = &br->entries[i];

        /* Lazily open handles for entries added at runtime */
        if (!s_handles[i])
            s_handles[i] = open_entry(e, br);
        esp_handle_t *h = s_handles[i];
        if (!h) continue;

        switch (e->mcu_periph) {
        case IO_PERIPH_GPIO:
            /* Host GPIO → MCU ext_pins */
            if (br->mcu_ops->gpio_ext_pins) {
                int level = gpio_get_level(h->gpio.pin);
                if (e->flags & IO_BF_INVERT) level ^= 1;
                uint8_t *ext = br->mcu_ops->gpio_ext_pins(br->cpu);
                if (ext) {
                    uint8_t mask = 1 << e->mcu_pin;
                    if (level) ext[e->mcu_index] |= mask;
                    else       ext[e->mcu_index] &= ~mask;
                }
            }
            break;

        case IO_PERIPH_UART: {
            if (h->uart.num == UART_NUM_0) {
                /* Console: use stdio */
                if (br->mcu_ops->uart_tx_pop) {
                    int v, n = 0;
                    while ((v = br->mcu_ops->uart_tx_pop(br->cpu, e->mcu_index)) >= 0) {
                        putchar(v);
                        n++;
                    }
                    if (n) fflush(stdout);
                }
            } else {
                /* Host UART RX → MCU UART */
                if (br->mcu_ops->uart_rx_push) {
                    uint8_t rx[16];
                    int n = uart_read_bytes(h->uart.num, rx, sizeof(rx), 0);
                    for (int j = 0; j < n; j++)
                        br->mcu_ops->uart_rx_push(br->cpu, e->mcu_index, rx[j]);
                }
                /* MCU UART TX → Host UART */
                if (br->mcu_ops->uart_tx_pop) {
                    uint8_t tx[32];
                    int len = 0, v;
                    while (len < (int)sizeof(tx) &&
                           (v = br->mcu_ops->uart_tx_pop(br->cpu, e->mcu_index)) >= 0)
                        tx[len++] = (uint8_t)v;
                    if (len > 0)
                        uart_write_bytes(h->uart.num, tx, len);
                }
            }
            break;
        }

        default:
            break;
        }
    }
}
