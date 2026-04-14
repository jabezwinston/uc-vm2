/*
 * ucvm - ESP32 entry point
 *
 * Dual-core operation:
 *   Core 1: AVR emulation loop (high priority, tight loop)
 *   Core 0: I/O bridge, web server, GDB stub, WiFi
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "src/core/avr_cpu.h"
#include "src/periph/avr_periph.h"
#include "src/util/ihex.h"

static const char *TAG = "ucvm";

/* Flash buffer — stored in DRAM, loaded from SPIFFS */
#define MAX_FLASH_WORDS 16384
static uint16_t flash_buf[MAX_FLASH_WORDS];

/* Emulation task — pinned to core 1 */
static void emu_task(void *arg)
{
    avr_cpu_t *cpu = (avr_cpu_t *)arg;

    ESP_LOGI(TAG, "Emulation started on core %d", xPortGetCoreID());

    while (1) {
        if (cpu->state == AVR_STATE_RUNNING) {
            avr_cpu_step(cpu);
        } else if (cpu->state == AVR_STATE_SLEEPING) {
            /* Tick timer and check interrupts */
            cpu->cycles += 1;
            if (cpu->periph_timer)
                avr_timer0_tick(cpu, cpu->periph_timer, 1);
            avr_cpu_check_irq(cpu);
        } else {
            /* Halted or break — wait for GDB or reset */
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* Drain UART TX buffer */
        if (cpu->periph_uart) {
            int c;
            while ((c = avr_uart_tx_pop(cpu->periph_uart)) >= 0)
                putchar(c);
        }
    }
}

/* Initialize SPIFFS for firmware storage */
static int init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/firmware",
        .partition_label = "firmware",
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ucvm - Microcontroller Virtual Machine");

    /* Initialize NVS (needed for WiFi, config storage) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize SPIFFS */
    if (init_spiffs() != 0) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        return;
    }

    /* Select variant (could be configured via NVS) */
    const avr_variant_t *variant = &avr_atmega328p;
    ESP_LOGI(TAG, "Variant: %s", variant->name);

    /* Load firmware from SPIFFS */
    memset(flash_buf, 0xFF, sizeof(flash_buf));
    uint32_t flash_words = variant->flash_size / 2;
    if (flash_words > MAX_FLASH_WORDS)
        flash_words = MAX_FLASH_WORDS;

    if (ihex_load("/firmware/avr.hex", flash_buf, flash_words) != 0) {
        ESP_LOGE(TAG, "Failed to load firmware");
        ESP_LOGI(TAG, "Upload firmware to SPIFFS as /firmware/avr.hex");
        return;
    }
    ESP_LOGI(TAG, "Firmware loaded");

    /* Initialize CPU */
    avr_cpu_t *cpu = avr_cpu_init(variant, flash_buf, variant->flash_size);
    if (!cpu) {
        ESP_LOGE(TAG, "CPU init failed");
        return;
    }

    /* TODO: Set up I/O bridge from NVS config */
    /* TODO: Start web server on core 0 */
    /* TODO: Start GDB stub if configured */

    /* Start emulation task on core 1 */
    xTaskCreatePinnedToCore(
        emu_task,
        "emu",
        4096,
        cpu,
        configMAX_PRIORITIES - 1,  /* highest priority */
        NULL,
        1  /* core 1 */
    );

    ESP_LOGI(TAG, "Emulation task started");

    /* Core 0: run I/O bridge polling and other tasks */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        /* TODO: Poll I/O bridge, handle web requests */
    }
}
