/*
 * ucvm - ESP32 Web Configuration Server
 *
 * Architecture-neutral: uses void* CPU pointer + arch enum.
 */
#include "esp_webserver.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "src/util/ihex.h"
#include "src/io/io_bridge.h"

#ifdef CONFIG_UCVM_ENABLE_AVR
#include "src/avr/avr_cpu.h"
#include "src/avr/avr_periph.h"
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
#include "src/mcs51/mcs51_cpu.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "web";
static httpd_handle_t server = NULL;
static void *s_cpu = NULL;
static int s_arch = 0;  /* 0 = AVR, 1 = MCS51 */
static io_bridge_config_t *s_config = NULL;

#define WEB_MOUNT_POINT "/web"
#define INDEX_PATH      WEB_MOUNT_POINT "/index.html"

/* ---------- Arch-neutral CPU accessors ---------- */

static uint8_t web_cpu_state(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) return ((avr_cpu_t *)s_cpu)->state;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) return ((mcs51_cpu_t *)s_cpu)->state;
#endif
    return 2;
}

static void web_cpu_set_state(uint8_t s)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) ((avr_cpu_t *)s_cpu)->state = s;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) ((mcs51_cpu_t *)s_cpu)->state = s;
#endif
}

static uint64_t web_cpu_cycles(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) return ((avr_cpu_t *)s_cpu)->cycles;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) return ((mcs51_cpu_t *)s_cpu)->cycles;
#endif
    return 0;
}

static uint16_t web_cpu_pc(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) return ((avr_cpu_t *)s_cpu)->pc;
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) return ((mcs51_cpu_t *)s_cpu)->pc;
#endif
    return 0;
}

static const char *web_cpu_variant(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) {
        avr_cpu_t *c = s_cpu;
        return c->variant ? c->variant->name : "avr";
    }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) {
        mcs51_cpu_t *c = s_cpu;
        return c->variant ? c->variant->name : "8051";
    }
#endif
    return "none";
}

static void web_cpu_reset(void)
{
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) avr_cpu_reset(s_cpu);
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) mcs51_cpu_reset(s_cpu);
#endif
}

/* ---------- SPIFFS mount for web content ---------- */

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

/* ---------- Handlers ---------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    FILE *fp = fopen(INDEX_PATH, "r");
    if (!fp) {
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

    const char *state_names[] = { "running", "sleeping", "halted", "break" };
    uint8_t st = web_cpu_state();
    const char *state_str = (st < 4) ? state_names[st] : "unknown";

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"arch\":\"%s\",\"state\":\"%s\",\"variant\":\"%s\",\"cycles\":%llu,\"pc\":%u}",
        s_arch == 0 ? "avr" : "8051",
        state_str, web_cpu_variant(),
        (unsigned long long)web_cpu_cycles(), (unsigned)web_cpu_pc());

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
        json_get_int(body, "type", &type);
        json_get_int(body, "avr", &avr);
        json_get_int(body, "host", &host);
        json_get_int(body, "flags", &flags);
        io_bridge_entry_t *e = &s_config->entries[s_config->num_entries++];
        e->type = (uint8_t)type;
        e->avr_resource = (uint8_t)avr;
        e->host_resource = (uint8_t)host;
        e->flags = (uint8_t)flags;
        httpd_resp_send(req, "{\"msg\":\"Entry added\"}", -1);
    } else if (strcmp(action, "del") == 0) {
        int index = -1;
        json_get_int(body, "index", &index);
        if (index >= 0 && index < s_config->num_entries) {
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
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 128 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
        return ESP_FAIL;
    }

    /* Firmware path depends on architecture */
    const char *fw_path = (s_arch == 0) ? "/firmware/avr.hex" : "/firmware/mcs51.ihx";

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

    /* Reload firmware and reset */
#ifdef CONFIG_UCVM_ENABLE_AVR
    if (s_arch == 0) {
        avr_cpu_t *cpu = s_cpu;
        uint16_t *flash = (uint16_t *)cpu->flash;
        uint32_t flash_words = cpu->flash_size / 2;
        memset(flash, 0xFF, flash_words * 2);
        if (ihex_load(fw_path, flash, flash_words) != 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"msg\":\"HEX parse error\"}", -1);
            return ESP_OK;
        }
        avr_cpu_reset(cpu);
    }
#endif
#ifdef CONFIG_UCVM_ENABLE_MCS51
    if (s_arch == 1) {
        mcs51_cpu_t *cpu = s_cpu;
        memset(cpu->code, 0xFF, cpu->code_size);
        if (ihex_load_bytes(fw_path, cpu->code, cpu->code_size) != 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"msg\":\"HEX parse error\"}", -1);
            return ESP_OK;
        }
        mcs51_cpu_reset(cpu);
    }
#endif

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
    if (req->content_len > 0) {
        char body[128];
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len > 0) {
            body[len] = '\0';
            char action[16] = "";
            json_get_str(body, "action", action, sizeof(action));
            if (strcmp(action, "halt") == 0) {
                web_cpu_set_state(2); /* HALTED */
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"msg\":\"CPU halted\"}", -1);
                return ESP_OK;
            }
        }
    }
    web_cpu_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"msg\":\"CPU reset\"}", -1);
    return ESP_OK;
}

/* ---------- Public API ---------- */

int webserver_start(void *cpu, int arch, io_bridge_config_t *config)
{
    s_cpu = cpu;
    s_arch = arch;
    s_config = config;

    if (mount_web_spiffs() != 0) {
        ESP_LOGE(TAG, "Cannot mount web SPIFFS");
        return -1;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 8;
    http_config.stack_size = 8192;
    http_config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &http_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return -1;
    }

    const httpd_uri_t uris[] = {
        { "/",             HTTP_GET,  index_handler,       NULL },
        { "/api/status",   HTTP_GET,  status_handler,      NULL },
        { "/api/config",   HTTP_GET,  config_get_handler,  NULL },
        { "/api/config",   HTTP_POST, config_post_handler, NULL },
        { "/api/firmware", HTTP_POST, firmware_handler,     NULL },
        { "/api/reset",    HTTP_POST, reset_handler,       NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server started on port %d", http_config.server_port);
    return 0;
}

void webserver_stop(void)
{
    if (server) { httpd_stop(server); server = NULL; }
}
