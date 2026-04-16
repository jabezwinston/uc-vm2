/*
 * ucvm - ESP32 Web Configuration Server
 *
 * Zero #ifdef on CPU architecture — all access via br->mcu_ops.
 *
 * API endpoints:
 *   GET  /              — Web UI (index.html from SPIFFS)
 *   GET  /api/status    — CPU state, cycles, PC, variant, entry count
 *   GET  /api/bridge    — All bridge entries
 *   POST /api/bridge    — Add / delete / save entries
 *   POST /api/firmware  — Upload Intel HEX firmware
 *   POST /api/reset     — CPU reset or halt
 */
#include "esp_webserver.h"
#include "esp_io_bridge.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "src/io/io_bridge.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";
static httpd_handle_t server = NULL;
static io_bridge_t *s_br = NULL;

#define WEB_MOUNT_POINT "/web"
#define INDEX_PATH      WEB_MOUNT_POINT "/index.html"

#define MCU  (s_br->mcu_ops)
#define CPU  (s_br->cpu)

/* ---------- SPIFFS mount ---------- */

static int web_spiffs_mounted = 0;

static int mount_web_spiffs(void)
{
    if (web_spiffs_mounted) return 0;
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
    return 0;
}

/* ---------- Minimal JSON helpers ---------- */

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
    while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* ---------- Name tables ---------- */

static const char *periph_name(uint8_t p)
{
    const char *names[] = {"gpio","uart","spi","i2c","adc","pwm","timer"};
    return p < 7 ? names[p] : "?";
}

static const char *host_name(uint8_t h)
{
    const char *names[] = {"gpio","uart","i2c","adc"};
    return h < 4 ? names[h] : "?";
}

/* ---------- GET / ---------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    FILE *fp = fopen(INDEX_PATH, "r");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "index.html not found");
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

/* ---------- GET /api/status ---------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!CPU || !MCU) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }

    const char *state_names[] = {"running","sleeping","halted","break"};
    uint8_t st = MCU->get_state(CPU);
    const char *state_str = (st < 4) ? state_names[st] : "unknown";

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"arch\":\"%s\",\"variant\":\"%s\",\"state\":\"%s\","
        "\"cycles\":%llu,\"pc\":%u,\"entries\":%d}",
        MCU->arch_name, MCU->get_variant(CPU), state_str,
        (unsigned long long)MCU->get_cycles(CPU),
        (unsigned)MCU->get_pc(CPU),
        s_br->num_entries);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ---------- GET /api/bridge ---------- */

static esp_err_t bridge_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char buf[512];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"entries\":[");
    for (int i = 0; i < s_br->num_entries; i++) {
        const io_bridge_entry_t *e = &s_br->entries[i];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"i\":%d,\"mp\":\"%s\",\"mi\":%d,\"pin\":%d,"
            "\"ht\":\"%s\",\"hi\":%d,\"param\":%d,\"flags\":%d}",
            i, periph_name(e->mcu_periph), e->mcu_index, e->mcu_pin,
            host_name(e->host_type), e->host_index, e->param, e->flags);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ---------- POST /api/bridge ---------- */

extern int esp_config_save(const io_bridge_t *br);

static esp_err_t bridge_post_handler(httpd_req_t *req)
{
    char body[256];
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
        if (s_br->num_entries >= IO_BRIDGE_MAX_ENTRIES) {
            httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Max entries\"}", -1);
            return ESP_OK;
        }
        int mp = 0, mi = 0, pin = 0, ht = 0, hi = 0, par = 0, fl = 0;
        json_get_int(body, "mp",    &mp);
        json_get_int(body, "mi",    &mi);
        json_get_int(body, "pin",   &pin);
        json_get_int(body, "ht",    &ht);
        json_get_int(body, "hi",    &hi);
        json_get_int(body, "param", &par);
        json_get_int(body, "flags", &fl);

        io_bridge_entry_t entry = {
            .mcu_periph = (uint8_t)mp,
            .mcu_index  = (uint8_t)mi,
            .mcu_pin    = (uint8_t)pin,
            .host_type  = (uint8_t)ht,
            .host_index = (uint8_t)hi,
            .flags      = (uint8_t)fl,
            .param      = (uint16_t)par,
        };
        int idx = io_bridge_add(s_br, &entry);
        char resp[64];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"index\":%d,\"msg\":\"Entry added\"}", idx);
        httpd_resp_send(req, resp, -1);
    }
    else if (strcmp(action, "del") == 0) {
        int index = -1;
        json_get_int(body, "index", &index);
        esp_bridge_close_entry(index);
        if (io_bridge_remove(s_br, index) == 0)
            httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Entry deleted\"}", -1);
        else
            httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Invalid index\"}", -1);
    }
    else if (strcmp(action, "save") == 0) {
        if (esp_config_save(s_br) == 0)
            httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Saved to NVS\"}", -1);
        else
            httpd_resp_send(req, "{\"ok\":false,\"msg\":\"NVS save failed\"}", -1);
    }
    else {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Unknown action\"}", -1);
    }
    return ESP_OK;
}

/* ---------- POST /api/firmware ---------- */

static esp_err_t firmware_handler(httpd_req_t *req)
{
    if (!CPU || !MCU) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 128 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
        return ESP_FAIL;
    }

    const char *fw_path = "/firmware/upload.hex";
    FILE *fp = fopen(fw_path, "w");
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

    httpd_resp_set_type(req, "application/json");
    if (MCU->load_firmware(CPU, fw_path) == 0)
        httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Firmware loaded\"}", -1);
    else
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"HEX parse error\"}", -1);
    return ESP_OK;
}

/* ---------- POST /api/reset ---------- */

static esp_err_t reset_handler(httpd_req_t *req)
{
    if (!CPU || !MCU) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No CPU");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");

    if (req->content_len > 0) {
        char body[64];
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len > 0) {
            body[len] = '\0';
            char action[16] = "";
            json_get_str(body, "action", action, sizeof(action));
            if (strcmp(action, "halt") == 0) {
                MCU->set_state(CPU, 2);
                httpd_resp_send(req, "{\"ok\":true,\"msg\":\"CPU halted\"}", -1);
                return ESP_OK;
            }
        }
    }
    MCU->reset(CPU);
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"CPU reset\"}", -1);
    return ESP_OK;
}

/* ---------- Public API ---------- */

int webserver_start(io_bridge_t *br)
{
    s_br = br;

    if (mount_web_spiffs() != 0) {
        ESP_LOGE(TAG, "Cannot mount web SPIFFS");
        return -1;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return -1;
    }

    const httpd_uri_t uris[] = {
        { "/",             HTTP_GET,  index_handler,       NULL },
        { "/api/status",   HTTP_GET,  status_handler,      NULL },
        { "/api/bridge",   HTTP_GET,  bridge_get_handler,  NULL },
        { "/api/bridge",   HTTP_POST, bridge_post_handler, NULL },
        { "/api/firmware", HTTP_POST, firmware_handler,    NULL },
        { "/api/reset",    HTTP_POST, reset_handler,       NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
    return 0;
}

void webserver_stop(void)
{
    if (server) { httpd_stop(server); server = NULL; }
}
