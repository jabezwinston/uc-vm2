/*
 * ucvm - ESP32 entry point
 *
 * Dual-core operation:
 *   Core 1: MCU emulation loop (high priority, tight loop)
 *   Core 0: I/O bridge polling, web server, GDB stub, WiFi
 *
 * Supports AVR and 8051 architectures (selectable via NVS / Kconfig).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "src/util/ihex.h"
#include "src/io/io_bridge.h"
#include "src/gdb/gdb_stub.h"
#include "src/gdb/gdb_target.h"

#ifdef CONFIG_UCVM_ENABLE_AVR
#include "src/avr/avr_cpu.h"
#include "src/avr/avr_periph.h"
#endif

#ifdef CONFIG_UCVM_ENABLE_MCS51
#include "src/mcs51/mcs51_cpu.h"
#include "src/mcs51/mcs51_periph.h"
#endif

#include "esp_io_bridge.h"
#include "esp_wifi_setup.h"
#include "esp_webserver.h"

static const char *TAG = "ucvm";

/* ---------- Architecture abstraction ---------- */

typedef enum { ARCH_AVR = 0, ARCH_MCS51 = 1 } ucvm_arch_t;

static ucvm_arch_t active_arch;
static void *g_cpu = NULL;  /* avr_cpu_t* or mcs51_cpu_t* */

/* Arch-neutral state accessors (state enums match: 0=running,1=sleep,2=halt,3=break) */
static uint8_t cpu_get_state(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (active_arch == ARCH_AVR)
        return ((avr_cpu_t *)g_cpu)->state;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (active_arch == ARCH_MCS51)
        return ((mcs51_cpu_t *)g_cpu)->state;
#endif
    return 2; /* halted */
}

static uint16_t cpu_get_pc(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (active_arch == ARCH_AVR)
        return ((avr_cpu_t *)g_cpu)->pc;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (active_arch == ARCH_MCS51)
        return ((mcs51_cpu_t *)g_cpu)->pc;
#endif
    return 0;
}

static void cpu_step(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (active_arch == ARCH_AVR)
        avr_cpu_step(g_cpu);
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (active_arch == ARCH_MCS51)
        mcs51_cpu_step(g_cpu);
#endif
}

/* AVR flash buffer */
#ifdef CONFIG_UCVM_ENABLE_AVR
#define MAX_FLASH_WORDS 16384
static uint16_t flash_buf[MAX_FLASH_WORDS];
#endif

/* I/O bridge */
static io_bridge_t bridge;

/* GDB state */
static gdb_state_t *g_gdb = NULL;

/* NVS */
#define NVS_NAMESPACE "ucvm"
#define NVS_KEY_BRIDGE  "bridge"
#define NVS_KEY_VARIANT "variant"
#define NVS_KEY_ARCH    "arch"

#ifdef CONFIG_UCVM_GDB_ENABLE
#define GDB_ENABLED 1
#ifndef CONFIG_UCVM_GDB_PORT
#define CONFIG_UCVM_GDB_PORT 1234
#endif
#else
#define GDB_ENABLED 0
#endif

/* ---------- NVS Config Storage ---------- */

int esp_config_save(const io_bridge_t *br)
{
    uint8_t buf[256];
    int len = io_bridge_serialize(br, buf, sizeof(buf));
    if (len < 0) return -1;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return -1;
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_BRIDGE, buf, len);
    if (err == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Bridge config saved (%d bytes, %d entries)", len, br->num_entries);
    return (err == ESP_OK) ? 0 : -1;
}

static int esp_config_load(io_bridge_t *br)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return -1;
    uint8_t buf[256];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_BRIDGE, buf, &len);
    nvs_close(nvs);
    if (err != ESP_OK) return -1;
    if (io_bridge_parse(buf, len, br) != 0) return -1;
    ESP_LOGI(TAG, "Bridge config loaded (%d entries)", br->num_entries);
    return 0;
}

int esp_arch_save(ucvm_arch_t arch, const char *variant)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return -1;
    nvs_set_u8(nvs, NVS_KEY_ARCH, (uint8_t)arch);
    nvs_set_str(nvs, NVS_KEY_VARIANT, variant);
    nvs_commit(nvs);
    nvs_close(nvs);
    return 0;
}

int esp_variant_save(const char *name)
{
    return esp_arch_save(active_arch, name);
}

static ucvm_arch_t esp_arch_load(char *variant_out, size_t variant_size)
{
    nvs_handle_t nvs;
    ucvm_arch_t default_arch;
#ifdef CONFIG_UCVM_DEFAULT_MCS51
    default_arch = ARCH_MCS51;
    const char *default_var = "at89s52";
#else
    default_arch = ARCH_AVR;
    const char *default_var = "atmega328p";
#endif

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        strncpy(variant_out, default_var, variant_size);
        return default_arch;
    }
    uint8_t arch_val;
    if (nvs_get_u8(nvs, NVS_KEY_ARCH, &arch_val) != ESP_OK)
        arch_val = (uint8_t)default_arch;
    size_t len = variant_size;
    if (nvs_get_str(nvs, NVS_KEY_VARIANT, variant_out, &len) != ESP_OK) {
        const char *v = (arch_val == ARCH_MCS51) ? "at89s52" : "atmega328p";
        strncpy(variant_out, v, variant_size);
    }
    nvs_close(nvs);
    return (ucvm_arch_t)arch_val;
}

/* ---------- SPIFFS init ---------- */

static int init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/firmware",
        .partition_label = "firmware",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return -1;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("firmware", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", (int)used, (int)total);
    return 0;
}

/* ---------- Emulation task — pinned to core 1 ---------- */

static void emu_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Emulation started on core %d (arch=%s)",
             xPortGetCoreID(), active_arch == ARCH_AVR ? "AVR" : "8051");

    while (1) {
        /* Only halt when a GDB client is connected and has paused us.
         * Without a client, the CPU runs freely (web-only usage). */
        if (g_gdb && gdb_has_client(g_gdb) && !gdb_is_running(g_gdb)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t state = cpu_get_state();

        if (state == 0 /* RUNNING */) {
            int debugging = g_gdb && gdb_has_client(g_gdb);
            if (debugging) {
                /* GDB attached: check breakpoints every step */
                for (int i = 0; i < 50000; i++) {
                    if (cpu_get_state() != 0) break;
                    if (gdb_check_breakpoint(g_gdb, cpu_get_pc())) {
                        gdb_notify_stop(g_gdb, 3); break;
                    }
                    cpu_step();
                    if (gdb_is_single_stepping(g_gdb)) {
                        gdb_notify_stop(g_gdb, 0); break;
                    }
                }
            } else {
                /* No debugger: batched threaded execution */
#ifdef CONFIG_UCVM_ENABLE_AVR
                if (active_arch == ARCH_AVR) {
                    avr_cpu_run(g_cpu, 500000);
                }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
                if (active_arch == ARCH_MCS51) {
                    mcs51_cpu_t *cpu = g_cpu;
                    for (int i = 0; i < 50000 && cpu->state == MCS51_STATE_RUNNING; i++)
                        mcs51_cpu_step(cpu);
                }
#endif
            }
            state = cpu_get_state();
            if (debugging && (state == 2 || state == 3))
                gdb_notify_stop(g_gdb, state);
            vTaskDelay(1);  /* let IDLE1 run to feed watchdog */

        } else if (state == 1 /* SLEEPING */) {
#ifdef CONFIG_UCVM_ENABLE_AVR
            if (active_arch == ARCH_AVR) {
                avr_cpu_t *cpu = g_cpu;
                cpu->cycles += 1;
                if (cpu->periph_timer)
                    avr_timer0_tick(cpu, cpu->periph_timer, 1);
                avr_cpu_check_irq(cpu);
                if (cpu->state == AVR_STATE_SLEEPING)
                    vTaskDelay(1);
            }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
            if (active_arch == ARCH_MCS51) {
                mcs51_cpu_t *cpu = g_cpu;
                cpu->cycles += 1;
                mcs51_cpu_check_irq(cpu);
                if (cpu->state == MCS51_STATE_SLEEPING)
                    vTaskDelay(1);
            }
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ---------- Main ---------- */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ucvm - Microcontroller Virtual Machine");
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. SPIFFS */
    if (init_spiffs() != 0) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        return;
    }

    /* 3. Load architecture + variant from NVS */
    char variant_name[32];
    active_arch = esp_arch_load(variant_name, sizeof(variant_name));
    ESP_LOGI(TAG, "Architecture: %s, Variant: %s",
             active_arch == ARCH_AVR ? "AVR" : "8051", variant_name);

    /* 4. Bridge init + config from NVS (or defaults) */
    const io_mcu_ops_t *mcu_ops = (active_arch == ARCH_AVR)
                                   ? &avr_mcu_ops : &mcs51_mcu_ops;
    io_bridge_init(&bridge, NULL, mcu_ops);
    if (esp_config_load(&bridge) != 0) {
        ESP_LOGI(TAG, "No saved config — loading defaults");
        /* MCU UART 0 -> ESP32 UART0 (console) */
        io_bridge_entry_t uart_entry = {
            .mcu_periph = IO_PERIPH_UART, .mcu_index = 0,
            .host_type  = IO_HOST_UART,   .host_index = 0,
        };
        io_bridge_add(&bridge, &uart_entry);
        /* MCU GPIO B.5 (Arduino pin 13) -> ESP32 GPIO 2 (onboard LED) */
        io_bridge_entry_t led_entry = {
            .mcu_periph = IO_PERIPH_GPIO, .mcu_index = 0, .mcu_pin = 5,
            .host_type  = IO_HOST_GPIO,   .host_index = 2,
        };
        io_bridge_add(&bridge, &led_entry);
    }

    /* 5. WiFi */
    ESP_LOGI(TAG, "Starting WiFi...");
    if (wifi_init_sta() != ESP_OK)
        ESP_LOGW(TAG, "WiFi failed");
    else
        ESP_LOGI(TAG, "Web UI at http://%s/", wifi_get_ip());

    /* 6. Initialize CPU */
    int fw_loaded = 0;

#ifdef CONFIG_UCVM_ENABLE_AVR
    if (active_arch == ARCH_AVR) {
        const avr_variant_t *variant = &avr_atmega328p;
        if (strcasecmp(variant_name, "attiny85") == 0)
            variant = &avr_attiny85;

        ESP_LOGI(TAG, "AVR: %s, %u bytes flash", variant->name, variant->flash_size);

        memset(flash_buf, 0xFF, sizeof(flash_buf));
        uint32_t flash_words = variant->flash_size / 2;
        if (flash_words > MAX_FLASH_WORDS) flash_words = MAX_FLASH_WORDS;

        if (fw_load("/firmware/avr.hex", flash_buf, flash_words) == 0) {
            ESP_LOGI(TAG, "Firmware loaded from /firmware/avr.hex");
            fw_loaded = 1;
        }

        avr_cpu_t *cpu = avr_cpu_init(variant, flash_buf, variant->flash_size);
        if (!cpu) { ESP_LOGE(TAG, "CPU init failed"); return; }
        if (!fw_loaded) cpu->state = AVR_STATE_HALTED;
        g_cpu = cpu;
    }
#endif

#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (active_arch == ARCH_MCS51) {
        const mcs51_variant_t *variant = &mcs51_at89s52;
        ESP_LOGI(TAG, "8051: %s, %u bytes code", variant->name, variant->code_size);

        mcs51_cpu_t *cpu = mcs51_cpu_init(variant);
        if (!cpu) { ESP_LOGE(TAG, "CPU init failed"); return; }

        memset(cpu->code, 0xFF, cpu->code_size);
        if (fw_load_bytes("/firmware/mcs51.ihx", cpu->code, cpu->code_size) == 0) {
            ESP_LOGI(TAG, "Firmware loaded from /firmware/mcs51.ihx");
            fw_loaded = 1;
        }
        if (!fw_loaded) cpu->state = MCS51_STATE_HALTED;
        g_cpu = cpu;
    }
#endif

    if (!g_cpu) {
        ESP_LOGE(TAG, "No CPU initialized — check Kconfig arch settings");
        return;
    }
    if (!fw_loaded)
        ESP_LOGW(TAG, "No firmware — upload via web UI at http://%s/", wifi_get_ip());

    /* Update bridge with CPU reference */
    bridge.cpu = g_cpu;

    /* 7. I/O bridge — always init to install callback */
    esp_bridge_init(&bridge);

    /* 8. Web server */
    if (wifi_is_connected()) {
        if (webserver_start(&bridge) == 0)
            ESP_LOGI(TAG, "Web server running at http://%s/", wifi_get_ip());
    }

    /* 9. GDB stub */
    if (GDB_ENABLED && wifi_is_connected()) {
        int gdb_port = CONFIG_UCVM_GDB_PORT;
        const gdb_target_ops_t *ops = NULL;
#ifdef CONFIG_UCVM_ENABLE_AVR
        if (active_arch == ARCH_AVR) ops = &gdb_target_avr;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
        if (active_arch == ARCH_MCS51) ops = &gdb_target_mcs51;
#endif
        if (ops) {
            g_gdb = gdb_init(g_cpu, ops, gdb_port);
            if (g_gdb)
                ESP_LOGI(TAG, "GDB stub on %s:%d", wifi_get_ip(), gdb_port);
        }
    }

    /* 10. Emulation task on core 1 */
    xTaskCreatePinnedToCore(emu_task, "emu", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);
    ESP_LOGI(TAG, "Emulation task pinned to core 1");

    /* 11. Core 0: polling loop */
    while (1) {
        esp_bridge_poll(&bridge);
        if (g_gdb)
            gdb_poll(g_gdb);
        vTaskDelay(1);
    }
}
