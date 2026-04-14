/*
 * ucvm - ESP32 I/O Bridge Backends
 *
 * Connects emulated MCU peripherals to real ESP32 hardware.
 * Currently AVR-specific for GPIO ext_pins and UART drain.
 */
#include "esp_io_bridge.h"

#ifdef CONFIG_UCVM_ENABLE_AVR

#include "src/avr/avr_periph.h"
#include "src/io/io_bridge.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bridge";

/* ---------- Bridge state ---------- */

/* GPIO bridge entries */
#define MAX_GPIO_BRIDGES 16
typedef struct {
    uint8_t avr_port;       /* AVR port id (0=B, 1=C, 2=D) */
    uint8_t avr_pin;        /* AVR pin within port (0-7) */
    gpio_num_t esp_gpio;    /* ESP32 GPIO number */
    uint8_t flags;          /* IO_BRIDGE_FLAG_* */
    uint8_t is_output;      /* 1 if AVR DDR says output */
} gpio_bridge_t;

static gpio_bridge_t gpio_bridges[MAX_GPIO_BRIDGES];
static int gpio_bridge_count = 0;

/* UART bridge */
typedef struct {
    uart_port_t uart_num;   /* ESP32 UART port */
    uint8_t host_type;      /* IO_BRIDGE_UART_HW, _TCP, etc. */
    int active;
} uart_bridge_t;

static uart_bridge_t uart_bridge = { .active = 0 };

/* ADC bridge entries */
#define MAX_ADC_BRIDGES 8
typedef struct {
    uint8_t avr_channel;    /* AVR ADC channel */
    adc_channel_t esp_channel; /* ESP32 ADC channel */
    uint8_t active;
} adc_bridge_t;

static adc_bridge_t adc_bridges[MAX_ADC_BRIDGES];
static int adc_bridge_count = 0;
static adc_oneshot_unit_handle_t adc_handle = NULL;

/* Reference to CPU for ISR context */
static avr_cpu_t *bridge_cpu = NULL;

/* ---------- GPIO ISR handler ---------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    /* arg = pointer to gpio_bridge_t entry */
    gpio_bridge_t *gb = (gpio_bridge_t *)arg;
    if (!bridge_cpu) return;

    /* Read the pin state */
    int level = gpio_get_level(gb->esp_gpio);
    if (gb->flags & IO_BRIDGE_FLAG_INVERT)
        level = !level;

    /* Update the AVR GPIO external pin state.
     * This sets the bit in the gpio peripheral's ext_pins array.
     * Note: This is called from ISR context — keep it minimal. */
    avr_gpio_t *gpio = bridge_cpu->periph_gpio;
    if (gpio) {
        uint8_t mask = 1 << gb->avr_pin;
        if (level)
            gpio->ext_pins[gb->avr_port] |= mask;
        else
            gpio->ext_pins[gb->avr_port] &= ~mask;
    }
}

/* ---------- Bridge callback (called from AVR peripheral writes) ---------- */

static void esp_bridge_callback(void *ctx, uint8_t type, uint8_t resource, uint8_t value)
{
    (void)ctx;

    switch (type) {
    case IO_BRIDGE_GPIO: {
        /* resource = port_id, value = full port value */
        for (int i = 0; i < gpio_bridge_count; i++) {
            gpio_bridge_t *gb = &gpio_bridges[i];
            if (gb->avr_port == resource) {
                int pin_val = (value >> gb->avr_pin) & 1;
                if (gb->flags & IO_BRIDGE_FLAG_INVERT)
                    pin_val = !pin_val;
                gpio_set_level(gb->esp_gpio, pin_val);
            }
        }
        break;
    }
    case IO_BRIDGE_UART:
        /* UART TX is handled by the drain loop, not the callback */
        break;
    default:
        break;
    }
}

/* ---------- GPIO setup ---------- */

static void setup_gpio_bridge(const io_bridge_entry_t *entry)
{
    if (gpio_bridge_count >= MAX_GPIO_BRIDGES) {
        ESP_LOGW(TAG, "Max GPIO bridges reached");
        return;
    }

    uint8_t avr_port = (entry->avr_resource >> 4) & 0x0F;
    uint8_t avr_pin  = entry->avr_resource & 0x0F;
    gpio_num_t esp_gpio = (gpio_num_t)entry->host_resource;

    ESP_LOGI(TAG, "GPIO bridge: AVR port%c.%d <-> ESP32 GPIO%d",
             'B' + avr_port, avr_pin, esp_gpio);

    gpio_bridge_t *gb = &gpio_bridges[gpio_bridge_count];
    gb->avr_port  = avr_port;
    gb->avr_pin   = avr_pin;
    gb->esp_gpio  = esp_gpio;
    gb->flags     = entry->flags;
    gb->is_output = 1; /* Default to output; updated by poll */

    /* Configure as output initially (AVR DDR determines direction) */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << esp_gpio),
        .mode         = GPIO_MODE_INPUT_OUTPUT, /* bidirectional */
        .pull_up_en   = (entry->flags & IO_BRIDGE_FLAG_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    /* If interrupt flags are set, enable interrupt */
    if (entry->flags & (IO_BRIDGE_FLAG_IRQ_RISE | IO_BRIDGE_FLAG_IRQ_FALL)) {
        if ((entry->flags & IO_BRIDGE_FLAG_IRQ_RISE) && (entry->flags & IO_BRIDGE_FLAG_IRQ_FALL))
            io_conf.intr_type = GPIO_INTR_ANYEDGE;
        else if (entry->flags & IO_BRIDGE_FLAG_IRQ_RISE)
            io_conf.intr_type = GPIO_INTR_POSEDGE;
        else
            io_conf.intr_type = GPIO_INTR_NEGEDGE;
    }

    gpio_config(&io_conf);

    /* Install ISR if interrupt enabled */
    if (io_conf.intr_type != GPIO_INTR_DISABLE) {
        gpio_isr_handler_add(esp_gpio, gpio_isr_handler, gb);
    }

    gpio_bridge_count++;
}

/* ---------- UART setup ---------- */

static void setup_uart_bridge(const io_bridge_entry_t *entry)
{
    if (uart_bridge.active) {
        ESP_LOGW(TAG, "UART bridge already active");
        return;
    }

    uart_port_t uart_num;
    if (entry->host_resource == IO_BRIDGE_UART_HW) {
        uart_num = UART_NUM_1; /* Use UART1 (UART0 is console) */
    } else {
        ESP_LOGW(TAG, "UART bridge type %d not supported yet (only HW)",
                 entry->host_resource);
        return;
    }

    ESP_LOGI(TAG, "UART bridge: AVR UART%d <-> ESP32 UART%d",
             entry->avr_resource, uart_num);

    uart_config_t uart_config = {
        .baud_rate  = 9600,  /* Default; AVR UBRR doesn't affect ESP32 baud */
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(uart_num, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_driver_install(uart_num, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return;
    }

    uart_bridge.uart_num  = uart_num;
    uart_bridge.host_type = entry->host_resource;
    uart_bridge.active    = 1;
}

/* ---------- ADC setup ---------- */

static void setup_adc_bridge(const io_bridge_entry_t *entry)
{
    if (adc_bridge_count >= MAX_ADC_BRIDGES) {
        ESP_LOGW(TAG, "Max ADC bridges reached");
        return;
    }

    /* Initialize ADC unit on first use */
    if (!adc_handle) {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
        };
        esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(err));
            return;
        }
    }

    adc_channel_t channel = (adc_channel_t)entry->host_resource;

    ESP_LOGI(TAG, "ADC bridge: AVR ADC%d <-> ESP32 ADC1_CH%d",
             entry->avr_resource, channel);

    adc_oneshot_chan_cfg_t chan_config = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(adc_handle, channel, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return;
    }

    adc_bridges[adc_bridge_count].avr_channel = entry->avr_resource;
    adc_bridges[adc_bridge_count].esp_channel = channel;
    adc_bridges[adc_bridge_count].active      = 1;
    adc_bridge_count++;
}

/* ---------- Public API ---------- */

void esp_bridge_init(avr_cpu_t *cpu, const io_bridge_config_t *config)
{
    bridge_cpu = cpu;
    gpio_bridge_count = 0;
    adc_bridge_count = 0;
    uart_bridge.active = 0;

    /* Install GPIO ISR service (shared across all GPIO interrupts) */
    gpio_install_isr_service(0);

    /* Set bridge callback on CPU */
    cpu->bridge_cb  = esp_bridge_callback;
    cpu->bridge_ctx = NULL;

    /* Process each bridge entry */
    for (uint8_t i = 0; i < config->num_entries; i++) {
        const io_bridge_entry_t *entry = &config->entries[i];
        switch (entry->type) {
        case IO_BRIDGE_GPIO:
            setup_gpio_bridge(entry);
            break;
        case IO_BRIDGE_UART:
            setup_uart_bridge(entry);
            break;
        case IO_BRIDGE_ADC:
            setup_adc_bridge(entry);
            break;
        default:
            ESP_LOGW(TAG, "Unknown bridge type %d", entry->type);
            break;
        }
    }

    ESP_LOGI(TAG, "Bridge init: %d GPIO, %d ADC, UART %s",
             gpio_bridge_count, adc_bridge_count,
             uart_bridge.active ? "active" : "inactive");
}

void esp_bridge_deinit(void)
{
    /* Remove GPIO ISRs */
    for (int i = 0; i < gpio_bridge_count; i++) {
        gpio_isr_handler_remove(gpio_bridges[i].esp_gpio);
        gpio_reset_pin(gpio_bridges[i].esp_gpio);
    }
    gpio_bridge_count = 0;

    /* Uninstall UART driver */
    if (uart_bridge.active) {
        uart_driver_delete(uart_bridge.uart_num);
        uart_bridge.active = 0;
    }

    /* Delete ADC unit */
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
    adc_bridge_count = 0;

    gpio_uninstall_isr_service();
    bridge_cpu = NULL;
}

void esp_bridge_poll(avr_cpu_t *cpu)
{
    /* Poll GPIO inputs: read ESP32 pins, update AVR ext_pins */
    avr_gpio_t *gpio = cpu->periph_gpio;
    if (gpio) {
        for (int i = 0; i < gpio_bridge_count; i++) {
            gpio_bridge_t *gb = &gpio_bridges[i];
            int level = gpio_get_level(gb->esp_gpio);
            if (gb->flags & IO_BRIDGE_FLAG_INVERT)
                level = !level;
            uint8_t mask = 1 << gb->avr_pin;
            if (level)
                gpio->ext_pins[gb->avr_port] |= mask;
            else
                gpio->ext_pins[gb->avr_port] &= ~mask;
        }
    }

    /* Poll UART RX: read from ESP32 UART, push to AVR */
    if (uart_bridge.active && cpu->periph_uart) {
        uint8_t rx_buf[16];
        int len = uart_read_bytes(uart_bridge.uart_num, rx_buf, sizeof(rx_buf), 0);
        for (int i = 0; i < len; i++) {
            avr_uart_rx_push(cpu, cpu->periph_uart, rx_buf[i]);
        }
    }

    /* Drain AVR UART TX: forward to ESP32 UART */
    if (uart_bridge.active && cpu->periph_uart) {
        int c;
        uint8_t tx_buf[32];
        int tx_len = 0;
        while ((c = avr_uart_tx_pop(cpu->periph_uart)) >= 0 && tx_len < (int)sizeof(tx_buf)) {
            tx_buf[tx_len++] = (uint8_t)c;
        }
        if (tx_len > 0) {
            uart_write_bytes(uart_bridge.uart_num, tx_buf, tx_len);
        }
    } else if (cpu->periph_uart) {
        /* No UART bridge — drain to console */
        int c;
        while ((c = avr_uart_tx_pop(cpu->periph_uart)) >= 0) {
            putchar(c);
        }
    }

    /* Poll ADC: read ESP32 ADC values (for future AVR ADC peripheral) */
    /* Note: AVR ADC peripheral not yet emulated; values stored for when it is */
    (void)adc_bridges;
}

int esp_bridge_uart_tx_pop(avr_cpu_t *cpu)
{
    if (!cpu->periph_uart)
        return -1;
    return avr_uart_tx_pop(cpu->periph_uart);
}

void esp_bridge_uart_rx_push(avr_cpu_t *cpu, uint8_t byte)
{
    if (cpu->periph_uart)
        avr_uart_rx_push(cpu, cpu->periph_uart, byte);
}

#endif /* CONFIG_UCVM_ENABLE_AVR */
