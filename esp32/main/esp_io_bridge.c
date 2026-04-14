/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Architecture-neutral: shared GPIO/UART/ADC setup, thin dispatch for
 * arch-specific ext_pins updates and UART TX/RX.
 */
#include "esp_io_bridge.h"
#include "src/io/io_bridge.h"

#ifdef CONFIG_UCVM_ENABLE_AVR
#include "src/avr/avr_cpu.h"
#include "src/avr/avr_periph.h"
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
#include "src/mcs51/mcs51_cpu.h"
#include "src/mcs51/mcs51_periph.h"
#endif

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bridge";

/* ---------- Stored CPU reference ---------- */

static void *bridge_cpu = NULL;
static int   bridge_arch = -1;

/* ---------- GPIO bridge state ---------- */

#define MAX_GPIO_BRIDGES 16
typedef struct {
    uint8_t    mcu_port;    /* MCU port id (AVR: 0=B,1=C,2=D; 8051: 0=P0..3=P3) */
    uint8_t    mcu_pin;     /* Pin within port (0-7) */
    gpio_num_t esp_gpio;    /* ESP32 GPIO number */
    uint8_t    flags;       /* IO_BRIDGE_FLAG_* */
} gpio_bridge_t;

static gpio_bridge_t gpio_bridges[MAX_GPIO_BRIDGES];
static int gpio_bridge_count = 0;

/* ---------- UART bridge state ---------- */

typedef struct {
    uart_port_t uart_num;
    uint8_t host_type;
    int active;
} uart_bridge_t;

static uart_bridge_t uart_bridge = { .active = 0 };

/* ---------- ADC bridge state ---------- */

#define MAX_ADC_BRIDGES 8
typedef struct {
    uint8_t        mcu_channel;
    adc_channel_t  esp_channel;
    uint8_t        active;
} adc_bridge_t;

static adc_bridge_t adc_bridges[MAX_ADC_BRIDGES];
static int adc_bridge_count = 0;
static adc_oneshot_unit_handle_t adc_handle = NULL;

/* ================================================================
 *  Architecture dispatch helpers — ext_pins access
 * ================================================================ */

/* Get pointer to ext_pins array from the CPU's gpio peripheral.
 * Both AVR and 8051 store ext_pins in their respective gpio structs. */
static uint8_t *get_ext_pins(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (bridge_arch == ESP_BRIDGE_ARCH_AVR) {
        avr_gpio_t *gpio = ((avr_cpu_t *)bridge_cpu)->periph_gpio;
        return gpio ? gpio->ext_pins : NULL;
    }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (bridge_arch == ESP_BRIDGE_ARCH_MCS51) {
        mcs51_gpio_t *gpio = ((mcs51_cpu_t *)bridge_cpu)->periph_gpio;
        return gpio ? gpio->ext_pins : NULL;
    }
#endif
    return NULL;
}

/* ---------- GPIO ISR handler (arch-neutral via ext_pins) ---------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    gpio_bridge_t *gb = (gpio_bridge_t *)arg;
    uint8_t *ext_pins = get_ext_pins();
    if (!ext_pins) return;

    int level = gpio_get_level(gb->esp_gpio);
    if (gb->flags & IO_BRIDGE_FLAG_INVERT)
        level = !level;

    uint8_t mask = 1 << gb->mcu_pin;
    if (level)
        ext_pins[gb->mcu_port] |= mask;
    else
        ext_pins[gb->mcu_port] &= ~mask;
}

/* ---------- Bridge callback (MCU output → ESP32 GPIO) ---------- */

static void esp_bridge_callback(void *ctx, uint8_t type, uint8_t resource, uint8_t value)
{
    (void)ctx;

    switch (type) {
    case IO_BRIDGE_GPIO:
        /* resource = port_id, value = full port byte */
        for (int i = 0; i < gpio_bridge_count; i++) {
            gpio_bridge_t *gb = &gpio_bridges[i];
            if (gb->mcu_port == resource) {
                int pin_val = (value >> gb->mcu_pin) & 1;
                if (gb->flags & IO_BRIDGE_FLAG_INVERT)
                    pin_val = !pin_val;
                gpio_set_level(gb->esp_gpio, pin_val);
            }
        }
        break;
    case IO_BRIDGE_UART:
        /* TX handled by drain loop in poll, not callback */
        break;
    default:
        break;
    }
}

/* ================================================================
 *  Shared setup functions (identical for all architectures)
 * ================================================================ */

static void setup_gpio_bridge(const io_bridge_entry_t *entry)
{
    if (gpio_bridge_count >= MAX_GPIO_BRIDGES) {
        ESP_LOGW(TAG, "Max GPIO bridges reached");
        return;
    }

    uint8_t mcu_port = (entry->avr_resource >> 4) & 0x0F;
    uint8_t mcu_pin  = entry->avr_resource & 0x0F;
    gpio_num_t esp_gpio = (gpio_num_t)entry->host_resource;

    /* Log with arch-appropriate port names */
    if (bridge_arch == ESP_BRIDGE_ARCH_AVR) {
        ESP_LOGI(TAG, "GPIO: PORT%c.%d <-> GPIO%d",
                 'B' + mcu_port, mcu_pin, esp_gpio);
    } else {
        ESP_LOGI(TAG, "GPIO: P%d.%d <-> GPIO%d",
                 mcu_port, mcu_pin, esp_gpio);
    }

    gpio_bridge_t *gb = &gpio_bridges[gpio_bridge_count];
    gb->mcu_port = mcu_port;
    gb->mcu_pin  = mcu_pin;
    gb->esp_gpio = esp_gpio;
    gb->flags    = entry->flags;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << esp_gpio),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = (entry->flags & IO_BRIDGE_FLAG_PULLUP)
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (entry->flags & (IO_BRIDGE_FLAG_IRQ_RISE | IO_BRIDGE_FLAG_IRQ_FALL)) {
        if ((entry->flags & IO_BRIDGE_FLAG_IRQ_RISE) &&
            (entry->flags & IO_BRIDGE_FLAG_IRQ_FALL))
            io_conf.intr_type = GPIO_INTR_ANYEDGE;
        else if (entry->flags & IO_BRIDGE_FLAG_IRQ_RISE)
            io_conf.intr_type = GPIO_INTR_POSEDGE;
        else
            io_conf.intr_type = GPIO_INTR_NEGEDGE;
    }

    gpio_config(&io_conf);

    if (io_conf.intr_type != GPIO_INTR_DISABLE)
        gpio_isr_handler_add(esp_gpio, gpio_isr_handler, gb);

    gpio_bridge_count++;
}

static void setup_uart_bridge(const io_bridge_entry_t *entry)
{
    if (uart_bridge.active) {
        ESP_LOGW(TAG, "UART bridge already active");
        return;
    }

    uart_port_t uart_num;
    if (entry->host_resource == IO_BRIDGE_UART_HW) {
        uart_num = UART_NUM_1;
    } else {
        ESP_LOGW(TAG, "UART type %d not supported (only HW)", entry->host_resource);
        return;
    }

    ESP_LOGI(TAG, "UART: MCU UART%d <-> ESP32 UART%d",
             entry->avr_resource, uart_num);

    uart_config_t uart_config = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_param_config(uart_num, &uart_config) != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed");
        return;
    }
    if (uart_driver_install(uart_num, 256, 256, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed");
        return;
    }

    uart_bridge.uart_num  = uart_num;
    uart_bridge.host_type = entry->host_resource;
    uart_bridge.active    = 1;
}

static void setup_adc_bridge(const io_bridge_entry_t *entry)
{
    if (adc_bridge_count >= MAX_ADC_BRIDGES) {
        ESP_LOGW(TAG, "Max ADC bridges reached");
        return;
    }

    if (!adc_handle) {
        adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&init_config, &adc_handle) != ESP_OK) {
            ESP_LOGE(TAG, "ADC init failed");
            return;
        }
    }

    adc_channel_t channel = (adc_channel_t)entry->host_resource;
    ESP_LOGI(TAG, "ADC: MCU ADC%d <-> ESP32 ADC1_CH%d",
             entry->avr_resource, channel);

    adc_oneshot_chan_cfg_t chan_config = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_handle, channel, &chan_config) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return;
    }

    adc_bridges[adc_bridge_count].mcu_channel = entry->avr_resource;
    adc_bridges[adc_bridge_count].esp_channel = channel;
    adc_bridges[adc_bridge_count].active      = 1;
    adc_bridge_count++;
}

/* ================================================================
 *  Public API
 * ================================================================ */

void esp_bridge_init(void *cpu, int arch, const io_bridge_config_t *config)
{
    bridge_cpu  = cpu;
    bridge_arch = arch;
    gpio_bridge_count = 0;
    adc_bridge_count  = 0;
    uart_bridge.active = 0;

    gpio_install_isr_service(0);

    /* Install bridge callback on the CPU (arch-specific field) */
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (arch == ESP_BRIDGE_ARCH_AVR) {
        avr_cpu_t *c = cpu;
        c->bridge_cb  = esp_bridge_callback;
        c->bridge_ctx = NULL;
    }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (arch == ESP_BRIDGE_ARCH_MCS51) {
        mcs51_cpu_t *c = cpu;
        c->bridge_cb  = esp_bridge_callback;
        c->bridge_ctx = NULL;
    }
#endif

    /* Process bridge entries — all shared */
    for (uint8_t i = 0; i < config->num_entries; i++) {
        const io_bridge_entry_t *entry = &config->entries[i];
        switch (entry->type) {
        case IO_BRIDGE_GPIO: setup_gpio_bridge(entry); break;
        case IO_BRIDGE_UART: setup_uart_bridge(entry); break;
        case IO_BRIDGE_ADC:  setup_adc_bridge(entry);  break;
        default: ESP_LOGW(TAG, "Unknown bridge type %d", entry->type); break;
        }
    }

    ESP_LOGI(TAG, "Bridge init (%s): %d GPIO, %d ADC, UART %s",
             arch == ESP_BRIDGE_ARCH_AVR ? "AVR" : "8051",
             gpio_bridge_count, adc_bridge_count,
             uart_bridge.active ? "active" : "inactive");
}

void esp_bridge_deinit(void)
{
    for (int i = 0; i < gpio_bridge_count; i++) {
        gpio_isr_handler_remove(gpio_bridges[i].esp_gpio);
        gpio_reset_pin(gpio_bridges[i].esp_gpio);
    }
    gpio_bridge_count = 0;

    if (uart_bridge.active) {
        uart_driver_delete(uart_bridge.uart_num);
        uart_bridge.active = 0;
    }

    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
    adc_bridge_count = 0;

    gpio_uninstall_isr_service();
    bridge_cpu  = NULL;
    bridge_arch = -1;
}

void esp_bridge_poll(void)
{
    if (!bridge_cpu) return;

    /* ---- GPIO input: read ESP32 pins → update MCU ext_pins ---- */
    uint8_t *ext_pins = get_ext_pins();
    if (ext_pins) {
        for (int i = 0; i < gpio_bridge_count; i++) {
            gpio_bridge_t *gb = &gpio_bridges[i];
            int level = gpio_get_level(gb->esp_gpio);
            if (gb->flags & IO_BRIDGE_FLAG_INVERT)
                level = !level;
            uint8_t mask = 1 << gb->mcu_pin;
            if (level)
                ext_pins[gb->mcu_port] |= mask;
            else
                ext_pins[gb->mcu_port] &= ~mask;
        }
    }

    /* ---- UART RX: ESP32 UART → MCU ---- */
    if (uart_bridge.active) {
        uint8_t rx_buf[16];
        int len = uart_read_bytes(uart_bridge.uart_num, rx_buf, sizeof(rx_buf), 0);
        for (int i = 0; i < len; i++) {
#ifdef CONFIG_UCVM_ENABLE_AVR
            if (bridge_arch == ESP_BRIDGE_ARCH_AVR) {
                avr_cpu_t *c = bridge_cpu;
                if (c->periph_uart)
                    avr_uart_rx_push(c, c->periph_uart, rx_buf[i]);
            }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
            if (bridge_arch == ESP_BRIDGE_ARCH_MCS51) {
                mcs51_cpu_t *c = bridge_cpu;
                if (c->periph_uart)
                    mcs51_uart_rx_push(c, c->periph_uart, rx_buf[i]);
            }
#endif
        }
    }

    /* ---- UART TX: MCU → ESP32 UART (or console) ---- */
    {
        uint8_t tx_buf[32];
        int tx_len = 0;
        int c;

#ifdef CONFIG_UCVM_ENABLE_AVR
        if (bridge_arch == ESP_BRIDGE_ARCH_AVR) {
            avr_cpu_t *avr = bridge_cpu;
            if (avr->periph_uart) {
                while ((c = avr_uart_tx_pop(avr->periph_uart)) >= 0 &&
                       tx_len < (int)sizeof(tx_buf))
                    tx_buf[tx_len++] = (uint8_t)c;
            }
        }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
        if (bridge_arch == ESP_BRIDGE_ARCH_MCS51) {
            mcs51_cpu_t *mcu = bridge_cpu;
            if (mcu->periph_uart) {
                while ((c = mcs51_uart_tx_pop(mcu->periph_uart)) >= 0 &&
                       tx_len < (int)sizeof(tx_buf))
                    tx_buf[tx_len++] = (uint8_t)c;
            }
        }
#endif

        if (tx_len > 0) {
            if (uart_bridge.active)
                uart_write_bytes(uart_bridge.uart_num, tx_buf, tx_len);
            else
                for (int i = 0; i < tx_len; i++)
                    putchar(tx_buf[i]);
        }
    }

    (void)adc_bridges; /* ADC polling — future */
}
