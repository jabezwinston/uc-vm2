/*
 * ucvm - ESP32 Web Configuration Server
 *
 * Endpoints:
 *   GET  /              — HTML UI (served from SPIFFS /web/index.html)
 *   GET  /api/status    — JSON: CPU state, cycles, variant
 *   GET  /api/config    — JSON: I/O bridge config
 *   POST /api/config    — Update I/O bridge config (JSON body)
 *   POST /api/firmware  — Upload Intel HEX firmware
 *   POST /api/reset     — Reset AVR CPU
 */
#include "esp_webserver.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "src/periph/avr_periph.h"
#include "src/util/ihex.h"
#include "src/io/io_bridge.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "web";
static httpd_handle_t server = NULL;
static avr_cpu_t *s_cpu = NULL;
static io_bridge_config_t *s_config = NULL;

#define WEB_MOUNT_POINT "/web"
#define INDEX_PATH      WEB_MOUNT_POINT "/index.html"

/* ---------- SPIFFS mount for web content ---------- */

static int web_spiffs_mounted = 0;

static int mount_web_spiffs(void)
{
    if (web_spiffs_mounted)
        return 0;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = WEB_MOUNT_POINT,
        .partition_label = "web",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web SPIFFS mount failed: %s", esp_err_to_name(ret));
        return -1;
    }
    web_spiffs_mounted = 1;

    size_t total = 0, used = 0;
    esp_spiffs_info("web", &total, &used);
    ESP_LOGI(TAG, "Web SPIFFS: %d/%d bytes used", (int)used, (int)total);
    return 0;
}

/* ---------- Handlers ---------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    FILE *fp = fopen(INDEX_PATH, "r");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s", INDEX_PATH);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "index.html not found on SPIFFS");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(fp);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(fp);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!s_cpu) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }

    const char *state_str;
    switch (s_cpu->state) {
    case AVR_STATE_RUNNING:  state_str = "running"; break;
    case AVR_STATE_SLEEPING: state_str = "sleeping"; break;
    case AVR_STATE_HALTED:   state_str = "halted"; break;
    case AVR_STATE_BREAK:    state_str = "break"; break;
    default:                 state_str = "unknown"; break;
    }

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"variant\":\"%s\",\"cycles\":%llu,\"pc\":%u}",
        state_str,
        s_cpu->variant ? s_cpu->variant->name : "none",
        (unsigned long long)s_cpu->cycles,
        (unsigned)s_cpu->pc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!s_config) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No config");
        return ESP_FAIL;
    }

    /* Build JSON response */
    char buf[1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"entries\":[");

    for (uint8_t i = 0; i < s_config->num_entries; i++) {
        const io_bridge_entry_t *e = &s_config->entries[i];
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"type\":%d,\"avr\":%d,\"host\":%d,\"flags\":%d}",
            e->type, e->avr_resource, e->host_resource, e->flags);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* Simple JSON key extraction (no full parser needed for our small payloads) */
static int json_get_int(const char *json, const char *key, int *out)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    *out = atoi(p);
    return 0;
}

static int json_get_str(const char *json, const char *key, char *out, int max)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* NVS save function (implemented in main.c) */
extern int esp_config_save(const io_bridge_config_t *config);

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!s_config) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No config");
        return ESP_FAIL;
    }

    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    char action[16] = "";
    json_get_str(body, "action", action, sizeof(action));

    httpd_resp_set_type(req, "application/json");

    if (strcmp(action, "add") == 0) {
        if (s_config->num_entries >= IO_BRIDGE_MAX_ENTRIES) {
            httpd_resp_send(req, "{\"msg\":\"Max entries reached\"}", -1);
            return ESP_OK;
        }
        int type = 0, avr = 0, host = 0, flags = 0;
        json_get_int(body, "type",  &type);
        json_get_int(body, "avr",   &avr);
        json_get_int(body, "host",  &host);
        json_get_int(body, "flags", &flags);

        io_bridge_entry_t *e = &s_config->entries[s_config->num_entries++];
        e->type          = (uint8_t)type;
        e->avr_resource  = (uint8_t)avr;
        e->host_resource = (uint8_t)host;
        e->flags         = (uint8_t)flags;

        httpd_resp_send(req, "{\"msg\":\"Entry added\"}", -1);
    } else if (strcmp(action, "del") == 0) {
        int index = -1;
        json_get_int(body, "index", &index);
        if (index >= 0 && index < s_config->num_entries) {
            /* Shift remaining entries down */
            for (int i = index; i < s_config->num_entries - 1; i++)
                s_config->entries[i] = s_config->entries[i + 1];
            s_config->num_entries--;
            httpd_resp_send(req, "{\"msg\":\"Entry deleted\"}", -1);
        } else {
            httpd_resp_send(req, "{\"msg\":\"Invalid index\"}", -1);
        }
    } else if (strcmp(action, "save") == 0) {
        if (esp_config_save(s_config) == 0)
            httpd_resp_send(req, "{\"msg\":\"Config saved to NVS\"}", -1);
        else
            httpd_resp_send(req, "{\"msg\":\"NVS save failed\"}", -1);
    } else {
        httpd_resp_send(req, "{\"msg\":\"Unknown action\"}", -1);
    }

    return ESP_OK;
}

static esp_err_t firmware_handler(httpd_req_t *req)
{
    if (!s_cpu) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }

    /* Receive the hex file content */
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 128 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
        return ESP_FAIL;
    }

    /* Write to SPIFFS */
    FILE *fp = fopen("/firmware/avr.hex", "w");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = total_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, fp);
        remaining -= received;
    }
    fclose(fp);

    ESP_LOGI(TAG, "Firmware uploaded (%d bytes)", total_len);

    /* Reload firmware into flash buffer */
    uint16_t *flash = (uint16_t *)s_cpu->flash;
    uint32_t flash_words = s_cpu->flash_size / 2;
    memset(flash, 0xFF, flash_words * 2);

    if (ihex_load("/firmware/avr.hex", flash, flash_words) != 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"msg\":\"HEX parse error\"}", -1);
        return ESP_OK;
    }

    /* Reset CPU */
    avr_cpu_reset(s_cpu);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"msg\":\"Firmware uploaded and CPU reset\"}", -1);
    return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    if (!s_cpu) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }

    /* Check if this is a halt request */
    if (req->content_len > 0) {
        char body[128];
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len > 0) {
            body[len] = '\0';
            char action[16] = "";
            json_get_str(body, "action", action, sizeof(action));
            if (strcmp(action, "halt") == 0) {
                s_cpu->state = AVR_STATE_HALTED;
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"msg\":\"CPU halted\"}", -1);
                return ESP_OK;
            }
        }
    }

    avr_cpu_reset(s_cpu);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"msg\":\"CPU reset\"}", -1);
    return ESP_OK;
}

/* ---------- Public API ---------- */

int webserver_start(avr_cpu_t *cpu, io_bridge_config_t *config)
{
    s_cpu = cpu;
    s_config = config;

    /* Mount web SPIFFS partition */
    if (mount_web_spiffs() != 0) {
        ESP_LOGE(TAG, "Cannot mount web SPIFFS — web UI unavailable");
        return -1;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 8;
    http_config.stack_size = 8192;
    http_config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &http_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return -1;
    }

    /* Register URI handlers */
    const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_handler
    };
    const httpd_uri_t config_get_uri = {
        .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler
    };
    const httpd_uri_t config_post_uri = {
        .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler
    };
    const httpd_uri_t firmware_uri = {
        .uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_handler
    };
    const httpd_uri_t reset_uri = {
        .uri = "/api/reset", .method = HTTP_POST, .handler = reset_handler
    };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &config_get_uri);
    httpd_register_uri_handler(server, &config_post_uri);
    httpd_register_uri_handler(server, &firmware_uri);
    httpd_register_uri_handler(server, &reset_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", http_config.server_port);
    return 0;
}

void webserver_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
