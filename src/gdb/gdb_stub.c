/*
 * ucvm - GDB Remote Serial Protocol stub implementation
 *
 * Architecture-neutral: all register/memory access goes through gdb_target_ops_t.
 * Supports: g/G (registers), m/M (memory), s (step), c (continue),
 *           Z0/z0 (breakpoints), ? (stop reason), qSupported, qAttached
 */
#include "gdb_stub.h"
#include "gdb_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

#define GDB_BUF_SIZE 1024

struct gdb_state {
    void *cpu;                       /* Opaque CPU pointer */
    const gdb_target_ops_t *ops;     /* Architecture callbacks */

    int listen_fd;
    int client_fd;

    /* State — volatile for cross-core visibility (ESP32 dual-core) */
    volatile int is_running;
    volatile int single_step;
    volatile int should_quit;
    volatile uint16_t resume_from_pc;

    /* Breakpoints */
    uint16_t breakpoints[GDB_MAX_BREAKPOINTS];
    int bp_count;

    /* Packet buffer */
    char buf[GDB_BUF_SIZE];
    int buf_len;

    /* Response buffer */
    char resp[GDB_BUF_SIZE];
};

/* ---------- Hex helpers ---------- */

static const char hex_chars[] = "0123456789abcdef";

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_byte(char *out, uint8_t val)
{
    out[0] = hex_chars[(val >> 4) & 0xF];
    out[1] = hex_chars[val & 0xF];
}

static uint32_t parse_hex(const char *s, int *len)
{
    uint32_t val = 0;
    int n = 0;
    while (1) {
        int h = hex_val(s[n]);
        if (h < 0) break;
        val = (val << 4) | h;
        n++;
    }
    if (len) *len = n;
    return val;
}

/* ---------- Packet framing ---------- */

static void gdb_send_raw(int fd, const void *buf, size_t len)
{
    ssize_t ret = write(fd, buf, len);
    (void)ret;
}

static void send_packet(gdb_state_t *gdb, const char *data)
{
    if (gdb->client_fd < 0) return;
    int len = strlen(data);
    uint8_t checksum = 0;
    for (int i = 0; i < len; i++)
        checksum += (uint8_t)data[i];
    char frame[GDB_BUF_SIZE + 8];
    int n = snprintf(frame, sizeof(frame), "$%s#%02x", data, checksum);
    gdb_send_raw(gdb->client_fd, frame, n);
}

static void send_ok(gdb_state_t *gdb) { send_packet(gdb, "OK"); }
static void send_empty(gdb_state_t *gdb) { send_packet(gdb, ""); }

/* ---------- Command handlers (architecture-neutral via ops) ---------- */

static void handle_read_registers(gdb_state_t *gdb)
{
    int len = gdb->ops->read_regs(gdb->cpu, gdb->resp, GDB_BUF_SIZE);
    gdb->resp[len] = '\0';
    send_packet(gdb, gdb->resp);
}

static void handle_write_registers(gdb_state_t *gdb, const char *data)
{
    gdb->ops->write_regs(gdb->cpu, data);
    send_ok(gdb);
}

static void handle_read_memory(gdb_state_t *gdb, const char *data)
{
    int len1, len2;
    uint32_t addr = parse_hex(data, &len1);
    uint32_t length = parse_hex(data + len1 + 1, &len2);
    if (length > (GDB_BUF_SIZE - 1) / 2)
        length = (GDB_BUF_SIZE - 1) / 2;

    char *p = gdb->resp;
    for (uint32_t i = 0; i < length; i++) {
        hex_byte(p, gdb->ops->read_mem(gdb->cpu, addr + i));
        p += 2;
    }
    *p = '\0';
    send_packet(gdb, gdb->resp);
}

static void handle_write_memory(gdb_state_t *gdb, const char *data)
{
    int len1, len2;
    uint32_t addr = parse_hex(data, &len1);
    data += len1 + 1;
    uint32_t length = parse_hex(data, &len2);
    data += len2 + 1;

    for (uint32_t i = 0; i < length && *data && *(data+1); i++) {
        uint8_t val = (hex_val(data[0]) << 4) | hex_val(data[1]);
        data += 2;
        gdb->ops->write_mem(gdb->cpu, addr + i, val);
    }
    send_ok(gdb);
}

static void handle_set_breakpoint(gdb_state_t *gdb, const char *data)
{
    if (data[0] != '0') { send_empty(gdb); return; }
    int len;
    uint32_t addr = parse_hex(data + 2, &len);
    uint16_t pc = addr / 2;  /* byte addr to word addr for AVR; 8051 uses byte addr directly */
    /* For 8051, PC is already byte-addressed. For AVR, GDB sends byte addresses.
     * The target ops get_pc returns byte address, so breakpoints should use byte addresses too.
     * Let's store byte addresses uniformly. */
    pc = (uint16_t)addr; /* Store as byte address */

    if (gdb->bp_count >= GDB_MAX_BREAKPOINTS) {
        send_packet(gdb, "E01");
        return;
    }
    gdb->breakpoints[gdb->bp_count++] = pc;
    send_ok(gdb);
}

static void handle_clear_breakpoint(gdb_state_t *gdb, const char *data)
{
    if (data[0] != '0') { send_empty(gdb); return; }
    int len;
    uint32_t addr = parse_hex(data + 2, &len);
    uint16_t pc = (uint16_t)addr;

    for (int i = 0; i < gdb->bp_count; i++) {
        if (gdb->breakpoints[i] == pc) {
            gdb->breakpoints[i] = gdb->breakpoints[--gdb->bp_count];
            break;
        }
    }
    send_ok(gdb);
}

static void handle_packet(gdb_state_t *gdb, const char *pkt, int len)
{
    if (len == 0) return;

    switch (pkt[0]) {
    case '?':
        send_packet(gdb, "S05");
        break;
    case 'g':
        handle_read_registers(gdb);
        break;
    case 'G':
        handle_write_registers(gdb, pkt + 1);
        break;
    case 'm':
        handle_read_memory(gdb, pkt + 1);
        break;
    case 'M':
        handle_write_memory(gdb, pkt + 1);
        break;
    case 'c': {
        if (len > 1) {
            int hlen;
            uint32_t addr = parse_hex(pkt + 1, &hlen);
            gdb->ops->set_pc(gdb->cpu, addr);
        }
        gdb->single_step = 0;
        gdb->resume_from_pc = (uint16_t)gdb->ops->get_pc(gdb->cpu);
        gdb->is_running = 1;
        gdb->ops->set_state(gdb->cpu, 0); /* RUNNING */
        break;
    }
    case 's': {
        if (len > 1) {
            int hlen;
            uint32_t addr = parse_hex(pkt + 1, &hlen);
            gdb->ops->set_pc(gdb->cpu, addr);
        }
        gdb->single_step = 1;
        gdb->is_running = 1;
        gdb->ops->set_state(gdb->cpu, 0); /* RUNNING */
        break;
    }
    case 'Z':
        handle_set_breakpoint(gdb, pkt + 1);
        break;
    case 'z':
        handle_clear_breakpoint(gdb, pkt + 1);
        break;
    case 'D':
        send_ok(gdb);
        close(gdb->client_fd);
        gdb->client_fd = -1;
        gdb->is_running = 1;
        gdb->single_step = 0;
        gdb->bp_count = 0;
        break;
    case 'k':
        close(gdb->client_fd);
        gdb->client_fd = -1;
        gdb->is_running = 1;
        gdb->single_step = 0;
        gdb->bp_count = 0;
        break;
    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0)
            send_packet(gdb, "PacketSize=400");
        else if (strncmp(pkt, "qAttached", 9) == 0)
            send_packet(gdb, "1");
        else if (strncmp(pkt, "qTStatus", 8) == 0)
            send_packet(gdb, "T0");
        else
            send_empty(gdb);
        break;
    case 'H':
        send_ok(gdb);
        break;
    case 'v':
        send_empty(gdb);
        break;
    default:
        send_empty(gdb);
        break;
    }
}

/* ---------- Public API ---------- */

gdb_state_t *gdb_init(void *cpu, const gdb_target_ops_t *ops, int port)
{
    gdb_state_t *gdb = calloc(1, sizeof(*gdb));
    if (!gdb) return NULL;

    gdb->cpu = cpu;
    gdb->ops = ops;
    gdb->client_fd = -1;
    gdb->is_running = 0;
    gdb->resume_from_pc = 0xFFFF;

    gdb->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gdb->listen_fd < 0) { free(gdb); return NULL; }

    int opt = 1;
    setsockopt(gdb->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(gdb->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(gdb->listen_fd); free(gdb); return NULL;
    }
    if (listen(gdb->listen_fd, 1) < 0) {
        close(gdb->listen_fd); free(gdb); return NULL;
    }

    int flags = fcntl(gdb->listen_fd, F_GETFL, 0);
    fcntl(gdb->listen_fd, F_SETFL, flags | O_NONBLOCK);

    return gdb;
}

/* Legacy AVR init — implemented in gdb_target_avr.c */

int gdb_has_client(gdb_state_t *gdb) { return gdb->client_fd >= 0; }

void gdb_poll(gdb_state_t *gdb)
{
    /* Auto-accept if no client — use select() to avoid blocking
     * (lwip's O_NONBLOCK on accept is unreliable on some ESP-IDF versions) */
    if (gdb->client_fd < 0 && gdb->listen_fd >= 0) {
        fd_set rfds;
        struct timeval tv = {0, 0};  /* zero timeout = non-blocking poll */
        FD_ZERO(&rfds);
        FD_SET(gdb->listen_fd, &rfds);
        if (select(gdb->listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            return;  /* no pending connection */

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(gdb->listen_fd,
                        (struct sockaddr *)&client_addr, &client_len);
        if (fd >= 0) {
            int cflags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, cflags | O_NONBLOCK);
            gdb->is_running = 0;
            gdb->ops->set_state(gdb->cpu, 2); /* HALTED */
            gdb->client_fd = fd;
            gdb->should_quit = 0;
            gdb->single_step = 0;
            gdb->resume_from_pc = 0xFFFF;
            gdb->bp_count = 0;
            gdb->ops->reset(gdb->cpu);
        }
        return;
    }

    if (gdb->client_fd < 0) return;

    char tmp[256];
    ssize_t n = read(gdb->client_fd, tmp, sizeof(tmp));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(gdb->client_fd);
            gdb->client_fd = -1;
            gdb->is_running = 1;
            gdb->single_step = 0;
        }
        return;
    }

    for (ssize_t i = 0; i < n; i++) {
        char c = tmp[i];
        if (c == 0x03) {
            gdb->is_running = 0;
            gdb->single_step = 0;
            send_packet(gdb, "S02");
            continue;
        }
        if (c == '+' || c == '-') continue;
        if (c == '$') { gdb->buf_len = 0; continue; }
        if (c == '#') {
            gdb->buf[gdb->buf_len] = '\0';
            gdb_send_raw(gdb->client_fd, "+", 1);
            handle_packet(gdb, gdb->buf, gdb->buf_len);
            i += 2;
            continue;
        }
        if (gdb->buf_len < GDB_BUF_SIZE - 1)
            gdb->buf[gdb->buf_len++] = c;
    }
}

int gdb_is_running(gdb_state_t *gdb) { return gdb->is_running; }
int gdb_is_single_stepping(gdb_state_t *gdb) { return gdb->single_step; }

int gdb_check_breakpoint(gdb_state_t *gdb, uint16_t pc)
{
    /* pc here is the native PC — convert to byte address for comparison */
    (void)pc;
    uint32_t bp_pc = gdb->ops->get_pc(gdb->cpu);

    if (gdb->resume_from_pc != 0xFFFF) {
        if (bp_pc == gdb->resume_from_pc)
            return 0;
        gdb->resume_from_pc = 0xFFFF;
    }
    for (int i = 0; i < gdb->bp_count; i++) {
        if (gdb->breakpoints[i] == (uint16_t)bp_pc)
            return 1;
    }
    return 0;
}

void gdb_notify_stop(gdb_state_t *gdb, int reason)
{
    gdb->is_running = 0;
    gdb->single_step = 0;
    (void)reason;
    send_packet(gdb, "S05");
}

int gdb_should_stop(gdb_state_t *gdb) { return gdb->should_quit; }

void gdb_free(gdb_state_t *gdb)
{
    if (!gdb) return;
    if (gdb->client_fd >= 0) close(gdb->client_fd);
    if (gdb->listen_fd >= 0) close(gdb->listen_fd);
    free(gdb);
}
