/*
 * ucvm - ESP32 entry point
 *
 * Dual-core operation:
 *   Core 1: AVR emulation loop (high priority, tight loop)
 *   Core 0: I/O bridge polling, web server, WiFi
 *
 * Boot sequence:
 *   1. Init NVS, SPIFFS
 *   2. Load variant + I/O bridge config from NVS
 *   3. Connect WiFi
 *   4. Load AVR firmware from SPIFFS
 *   5. Init CPU + peripherals
 *   6. Init I/O bridge backends (GPIO/UART/ADC)
 *   7. Start web server
 *   8. Pin emulation task to core 1
 *   9. Core 0: poll I/O bridge + serve HTTP
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "src/core/avr_cpu.h"
#include "src/periph/avr_periph.h"
#include "src/util/ihex.h"
#include "src/io/io_bridge.h"

#include "esp_io_bridge.h"
#include "esp_wifi_setup.h"
#include "esp_webserver.h"

static const char *TAG = "ucvm";

/* Flash buffer — stored in DRAM, loaded from SPIFFS */
#define MAX_FLASH_WORDS 16384
static uint16_t flash_buf[MAX_FLASH_WORDS];

/* I/O bridge config (runtime, modifiable via web) */
static io_bridge_config_t bridge_config;

/* NVS namespace */
#define NVS_NAMESPACE "ucvm"
#define NVS_KEY_BRIDGE "bridge"
#define NVS_KEY_VARIANT "variant"

/* ---------- NVS Config Storage ---------- */

int esp_config_save(const io_bridge_config_t *config)
{
    uint8_t buf[256];
    int len = io_bridge_serialize(config, buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGE(TAG, "Config serialize failed");
        return -1;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_blob(nvs, NVS_KEY_BRIDGE, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set_blob failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return -1;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "Bridge config saved to NVS (%d bytes, %d entries)",
             len, config->num_entries);
    return 0;
}

static int esp_config_load(io_bridge_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No NVS config found (first boot?)");
        return -1;
    }

    uint8_t buf[256];
    size_t len = sizeof(buf);
    err = nvs_get_blob(nvs, NVS_KEY_BRIDGE, buf, &len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No bridge config in NVS");
        return -1;
    }

    if (io_bridge_parse(buf, len, config) != 0) {
        ESP_LOGW(TAG, "Invalid bridge config in NVS");
        return -1;
    }

    ESP_LOGI(TAG, "Bridge config loaded from NVS (%d entries)", config->num_entries);
    return 0;
}

int esp_variant_save(const char *name)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return -1;
    nvs_set_str(nvs, NVS_KEY_VARIANT, name);
    nvs_commit(nvs);
    nvs_close(nvs);
    return 0;
}

static const avr_variant_t *esp_variant_load(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return &avr_atmega328p; /* default */

    char name[32] = "";
    size_t len = sizeof(name);
    err = nvs_get_str(nvs, NVS_KEY_VARIANT, name, &len);
    nvs_close(nvs);

    if (err == ESP_OK) {
        if (strcasecmp(name, "attiny85") == 0)
            return &avr_attiny85;
    }
    return &avr_atmega328p;
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
    avr_cpu_t *cpu = (avr_cpu_t *)arg;
    ESP_LOGI(TAG, "Emulation started on core %d", xPortGetCoreID());

    while (1) {
        if (cpu->state == AVR_STATE_RUNNING) {
            /* Run a batch of instructions before yielding */
            for (int i = 0; i < 1000; i++) {
                if (cpu->state != AVR_STATE_RUNNING)
                    break;
                avr_cpu_step(cpu);
            }
            taskYIELD();
        } else if (cpu->state == AVR_STATE_SLEEPING) {
            /* In sleep mode: tick timer, check interrupts */
            cpu->cycles += 1;
            if (cpu->periph_timer)
                avr_timer0_tick(cpu, cpu->periph_timer, 1);
            avr_cpu_check_irq(cpu);
            if (cpu->state == AVR_STATE_SLEEPING)
                vTaskDelay(1); /* Don't spin if still sleeping */
        } else {
            /* Halted or break — wait for reset via web UI */
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

    /* 1. Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. Initialize SPIFFS */
    if (init_spiffs() != 0) {
        ESP_LOGE(TAG, "SPIFFS init failed — cannot continue");
        return;
    }

    /* 3. Load variant from NVS */
    const avr_variant_t *variant = esp_variant_load();
    ESP_LOGI(TAG, "Variant: %s (%u bytes flash, %u bytes SRAM)",
             variant->name, variant->flash_size,
             variant->data_size - variant->sram_start);

    /* 4. Load I/O bridge config from NVS */
    memset(&bridge_config, 0, sizeof(bridge_config));
    if (esp_config_load(&bridge_config) != 0) {
        ESP_LOGI(TAG, "Using empty bridge config");
    }

    /* 5. Connect WiFi */
    ESP_LOGI(TAG, "Starting WiFi...");
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi failed — web UI unavailable, continuing anyway");
    } else {
        ESP_LOGI(TAG, "Web UI at http://%s/", wifi_get_ip());
    }

    /* 6. Load AVR firmware from SPIFFS */
    memset(flash_buf, 0xFF, sizeof(flash_buf));
    uint32_t flash_words = variant->flash_size / 2;
    if (flash_words > MAX_FLASH_WORDS)
        flash_words = MAX_FLASH_WORDS;

    int fw_loaded = 0;
    if (ihex_load("/firmware/avr.hex", flash_buf, flash_words) == 0) {
        ESP_LOGI(TAG, "Firmware loaded from /firmware/avr.hex");
        fw_loaded = 1;
    } else {
        ESP_LOGW(TAG, "No firmware found — upload via web UI at http://%s/",
                 wifi_get_ip());
    }

    /* 7. Initialize CPU */
    avr_cpu_t *cpu = avr_cpu_init(variant, flash_buf, variant->flash_size);
    if (!cpu) {
        ESP_LOGE(TAG, "CPU init failed — out of memory?");
        return;
    }

    /* If no firmware, halt CPU until firmware is uploaded */
    if (!fw_loaded)
        cpu->state = AVR_STATE_HALTED;

    /* 8. Initialize I/O bridge backends */
    if (bridge_config.num_entries > 0) {
        esp_bridge_init(cpu, &bridge_config);
    } else {
        ESP_LOGI(TAG, "No I/O bridges configured — configure via web UI");
    }

    /* 9. Start web server */
    if (wifi_is_connected()) {
        if (webserver_start(cpu, &bridge_config) == 0) {
            ESP_LOGI(TAG, "Web server running at http://%s/", wifi_get_ip());
        }
    }

    /* 10. Start emulation task on core 1 */
    xTaskCreatePinnedToCore(
        emu_task,
        "emu",
        4096,
        cpu,
        configMAX_PRIORITIES - 1,
        NULL,
        1  /* core 1 */
    );
    ESP_LOGI(TAG, "Emulation task pinned to core 1");

    /* 11. Core 0: I/O bridge polling loop */
    ESP_LOGI(TAG, "Core 0: I/O bridge poll + HTTP server");
    while (1) {
        esp_bridge_poll(cpu);
        vTaskDelay(pdMS_TO_TICKS(5)); /* ~200 Hz poll rate */
    }
}
