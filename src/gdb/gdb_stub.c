/*
 * ucvm - GDB Remote Serial Protocol stub implementation
 *
 * Supports: g/G (registers), m/M (memory), s (step), c (continue),
 *           Z0/z0 (breakpoints), ? (stop reason), qSupported, qAttached
 *
 * AVR-GDB register layout: R0-R31 (32 bytes), SREG (1), SPL (1), SPH (1), PC (4)
 * Total: 39 bytes = 78 hex chars
 */
#include "gdb_stub.h"
#include "../core/avr_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define GDB_BUF_SIZE 1024

struct gdb_state {
    avr_cpu_t *cpu;
    const uint16_t *flash;
    uint32_t flash_words;

    int listen_fd;
    int client_fd;

    /* State — volatile for cross-core visibility (ESP32 dual-core) */
    volatile int is_running;    /* 1 = emulation running, 0 = stopped */
    volatile int single_step;
    volatile int should_quit;
    volatile uint16_t resume_from_pc; /* PC where we resumed — skip BP once at this addr */

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
    (void)ret; /* best-effort: GDB reconnects on failure */
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

static void send_ok(gdb_state_t *gdb)
{
    send_packet(gdb, "OK");
}

static void send_empty(gdb_state_t *gdb)
{
    send_packet(gdb, "");
}

/* ---------- Command handlers ---------- */

static void handle_read_registers(gdb_state_t *gdb)
{
    avr_cpu_t *cpu = gdb->cpu;
    char *p = gdb->resp;

    /* R0-R31: 32 bytes */
    for (int i = 0; i < 32; i++) {
        hex_byte(p, AVR_R(cpu, i));
        p += 2;
    }
    /* SREG: 1 byte */
    hex_byte(p, cpu->sreg);
    p += 2;
    /* SPL, SPH: 2 bytes */
    hex_byte(p, cpu->sp & 0xFF);
    p += 2;
    hex_byte(p, (cpu->sp >> 8) & 0xFF);
    p += 2;
    /* PC: 4 bytes (byte address = word address * 2, little-endian) */
    uint32_t pc_bytes = (uint32_t)cpu->pc * 2;
    hex_byte(p, pc_bytes & 0xFF);        p += 2;
    hex_byte(p, (pc_bytes >> 8) & 0xFF); p += 2;
    hex_byte(p, (pc_bytes >> 16) & 0xFF); p += 2;
    hex_byte(p, (pc_bytes >> 24) & 0xFF); p += 2;

    *p = '\0';
    send_packet(gdb, gdb->resp);
}

static void handle_write_registers(gdb_state_t *gdb, const char *data)
{
    avr_cpu_t *cpu = gdb->cpu;
    const char *p = data;

    /* R0-R31 */
    for (int i = 0; i < 32 && *p && *(p+1); i++) {
        AVR_R(cpu, i) = (hex_val(p[0]) << 4) | hex_val(p[1]);
        p += 2;
    }
    /* SREG */
    if (*p && *(p+1)) {
        cpu->sreg = (hex_val(p[0]) << 4) | hex_val(p[1]);
        p += 2;
    }
    /* SPL, SPH */
    if (*p && *(p+1)) {
        uint8_t spl = (hex_val(p[0]) << 4) | hex_val(p[1]);
        p += 2;
        if (*p && *(p+1)) {
            uint8_t sph = (hex_val(p[0]) << 4) | hex_val(p[1]);
            p += 2;
            cpu->sp = (sph << 8) | spl;
        }
    }
    /* PC (4 bytes, byte address, little-endian) */
    if (*p && *(p+1)) {
        uint32_t pc_bytes = 0;
        for (int i = 0; i < 4 && *p && *(p+1); i++) {
            pc_bytes |= ((uint32_t)((hex_val(p[0]) << 4) | hex_val(p[1]))) << (i * 8);
            p += 2;
        }
        cpu->pc = pc_bytes / 2;
    }
    send_ok(gdb);
}

static void handle_read_memory(gdb_state_t *gdb, const char *data)
{
    int len1, len2;
    uint32_t addr = parse_hex(data, &len1);
    uint32_t length = parse_hex(data + len1 + 1, &len2); /* skip comma */

    if (length > (GDB_BUF_SIZE - 1) / 2)
        length = (GDB_BUF_SIZE - 1) / 2;

    char *p = gdb->resp;
    avr_cpu_t *cpu = gdb->cpu;

    for (uint32_t i = 0; i < length; i++) {
        uint32_t a = addr + i;
        uint8_t val;
        if (a < 0x800000) {
            /* Program memory (flash) */
            val = avr_flash_read_byte(cpu, (uint16_t)a);
        } else if (a >= 0x800000 && a < 0x810000) {
            /* Data memory */
            val = avr_data_read(cpu, (uint16_t)(a - 0x800000));
        } else {
            val = 0;
        }
        hex_byte(p, val);
        p += 2;
    }
    *p = '\0';
    send_packet(gdb, gdb->resp);
}

static void handle_write_memory(gdb_state_t *gdb, const char *data)
{
    int len1, len2;
    uint32_t addr = parse_hex(data, &len1);
    data += len1 + 1; /* skip comma */
    uint32_t length = parse_hex(data, &len2);
    data += len2 + 1; /* skip colon */

    avr_cpu_t *cpu = gdb->cpu;

    for (uint32_t i = 0; i < length && *data && *(data+1); i++) {
        uint8_t val = (hex_val(data[0]) << 4) | hex_val(data[1]);
        data += 2;
        uint32_t a = addr + i;
        if (a >= 0x800000 && a < 0x810000) {
            avr_data_write(cpu, (uint16_t)(a - 0x800000), val);
        }
        /* Flash writes not supported */
    }
    send_ok(gdb);
}

static void handle_set_breakpoint(gdb_state_t *gdb, const char *data)
{
    /* Z0,addr,kind */
    if (data[0] != '0') { send_empty(gdb); return; } /* only sw breakpoints */
    int len;
    uint32_t addr = parse_hex(data + 2, &len); /* skip "0," */
    uint16_t pc = addr / 2; /* byte addr to word addr */

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
    uint16_t pc = addr / 2;

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
        /* Stop reason */
        send_packet(gdb, "S05"); /* SIGTRAP */
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
    case 'c':
        /* Continue — record resume PC so gdb_check_breakpoint skips it once */
        if (len > 1) {
            int hlen;
            uint32_t addr = parse_hex(pkt + 1, &hlen);
            gdb->cpu->pc = addr / 2;
        }
        gdb->single_step = 0;
        gdb->resume_from_pc = gdb->cpu->pc;
        gdb->is_running = 1;
        gdb->cpu->state = AVR_STATE_RUNNING;
        break;
    case 's':
        /* Single step */
        if (len > 1) {
            int hlen;
            uint32_t addr = parse_hex(pkt + 1, &hlen);
            gdb->cpu->pc = addr / 2;
        }
        gdb->single_step = 1;
        gdb->is_running = 1;
        gdb->cpu->state = AVR_STATE_RUNNING;
        break;
    case 'Z':
        handle_set_breakpoint(gdb, pkt + 1);
        break;
    case 'z':
        handle_clear_breakpoint(gdb, pkt + 1);
        break;
    case 'D':
        /* Detach — close client, resume emulation, allow reconnect */
        send_ok(gdb);
        close(gdb->client_fd);
        gdb->client_fd = -1;
        gdb->is_running = 1;
        gdb->single_step = 0;
        gdb->bp_count = 0;
        break;
    case 'k':
        /* Kill — same as detach for embedded targets */
        close(gdb->client_fd);
        gdb->client_fd = -1;
        gdb->is_running = 1;
        gdb->single_step = 0;
        gdb->bp_count = 0;
        break;
    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
            send_packet(gdb, "PacketSize=400");
        } else if (strncmp(pkt, "qAttached", 9) == 0) {
            send_packet(gdb, "1");
        } else if (strncmp(pkt, "qTStatus", 8) == 0) {
            send_packet(gdb, "T0");
        } else {
            send_empty(gdb);
        }
        break;
    case 'H':
        /* Set thread — only one thread */
        send_ok(gdb);
        break;
    case 'v':
        if (strncmp(pkt, "vMustReplyEmpty", 15) == 0)
            send_empty(gdb);
        else
            send_empty(gdb);
        break;
    default:
        send_empty(gdb);
        break;
    }
}

/* ---------- Public API ---------- */

gdb_state_t *gdb_init(avr_cpu_t *cpu, const uint16_t *flash,
                       uint32_t flash_words, int port)
{
    gdb_state_t *gdb = calloc(1, sizeof(*gdb));
    if (!gdb) return NULL;

    gdb->cpu = cpu;
    gdb->flash = flash;
    gdb->flash_words = flash_words;
    gdb->client_fd = -1;
    gdb->is_running = 0;
    gdb->resume_from_pc = 0xFFFF;

    /* Create TCP listener */
    gdb->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gdb->listen_fd < 0) {
        free(gdb);
        return NULL;
    }

    int opt = 1;
    setsockopt(gdb->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(gdb->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(gdb->listen_fd);
        free(gdb);
        return NULL;
    }

    if (listen(gdb->listen_fd, 1) < 0) {
        close(gdb->listen_fd);
        free(gdb);
        return NULL;
    }

    /* Set listen socket non-blocking for poll-based accept */
    int flags = fcntl(gdb->listen_fd, F_GETFL, 0);
    fcntl(gdb->listen_fd, F_SETFL, flags | O_NONBLOCK);

    return gdb;
}

void gdb_wait_connect(gdb_state_t *gdb)
{
    /* Make listen socket blocking for this call */
    int flags = fcntl(gdb->listen_fd, F_GETFL, 0);
    fcntl(gdb->listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    gdb->client_fd = accept(gdb->listen_fd,
                             (struct sockaddr *)&client_addr, &client_len);

    /* Restore non-blocking on listen socket */
    fcntl(gdb->listen_fd, F_SETFL, flags | O_NONBLOCK);

    if (gdb->client_fd >= 0) {
        /* Set client non-blocking */
        int cflags = fcntl(gdb->client_fd, F_GETFL, 0);
        fcntl(gdb->client_fd, F_SETFL, cflags | O_NONBLOCK);
    }
}

void gdb_poll(gdb_state_t *gdb)
{
    /* If no client, try non-blocking accept */
    if (gdb->client_fd < 0 && gdb->listen_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(gdb->listen_fd,
                        (struct sockaddr *)&client_addr, &client_len);
        if (fd >= 0) {
            int cflags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, cflags | O_NONBLOCK);
            /* Stop emulation first to avoid racing with emu task */
            gdb->is_running = 0;
            gdb->cpu->state = AVR_STATE_HALTED;
            gdb->client_fd = fd;
            gdb->should_quit = 0;
            gdb->single_step = 0;
            gdb->resume_from_pc = 0xFFFF;
            gdb->bp_count = 0;
            /* Reset CPU — safe now since emu task sees HALTED state */
            avr_cpu_reset(gdb->cpu);
        }
        return; /* Nothing more to do this cycle */
    }

    if (gdb->client_fd < 0) return;

    char tmp[256];
    ssize_t n = read(gdb->client_fd, tmp, sizeof(tmp));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* Client disconnected — close client but keep listening */
            close(gdb->client_fd);
            gdb->client_fd = -1;
            gdb->is_running = 1; /* Resume emulation when GDB disconnects */
            gdb->single_step = 0;
            /* Don't set should_quit — allow reconnection via poll */
        }
        return;
    }

    /* Process received data */
    for (ssize_t i = 0; i < n; i++) {
        char c = tmp[i];

        if (c == 0x03) {
            /* Ctrl-C: interrupt */
            gdb->is_running = 0;
            gdb->single_step = 0;
            send_packet(gdb, "S02"); /* SIGINT */
            continue;
        }

        if (c == '+' || c == '-') {
            /* ACK/NACK — ignore (we don't use reliable transport) */
            continue;
        }

        if (c == '$') {
            /* Start of packet */
            gdb->buf_len = 0;
            continue;
        }

        if (c == '#') {
            /* End of packet — next 2 chars are checksum (skip them) */
            gdb->buf[gdb->buf_len] = '\0';
            /* Send ACK */
            gdb_send_raw(gdb->client_fd, "+", 1);
            /* Handle the packet */
            handle_packet(gdb, gdb->buf, gdb->buf_len);
            /* Skip checksum */
            i += 2;
            continue;
        }

        /* Accumulate packet data */
        if (gdb->buf_len < GDB_BUF_SIZE - 1)
            gdb->buf[gdb->buf_len++] = c;
    }
}

int gdb_is_running(gdb_state_t *gdb)
{
    return gdb->is_running;
}

int gdb_is_single_stepping(gdb_state_t *gdb)
{
    return gdb->single_step;
}

int gdb_check_breakpoint(gdb_state_t *gdb, uint16_t pc)
{
    /* After continue, skip BP at the resume PC so we don't re-trigger
     * the breakpoint we just resumed from. Consumed once PC moves. */
    if (gdb->resume_from_pc != 0xFFFF) {
        if (pc == gdb->resume_from_pc)
            return 0; /* still at resume point — skip */
        gdb->resume_from_pc = 0xFFFF; /* PC moved — re-enable BP checking */
    }
    for (int i = 0; i < gdb->bp_count; i++) {
        if (gdb->breakpoints[i] == pc)
            return 1;
    }
    return 0;
}

void gdb_notify_stop(gdb_state_t *gdb, int reason)
{
    gdb->is_running = 0;
    gdb->single_step = 0;
    (void)reason;
    send_packet(gdb, "S05"); /* SIGTRAP */
}

int gdb_should_stop(gdb_state_t *gdb)
{
    return gdb->should_quit;
}

void gdb_free(gdb_state_t *gdb)
{
    if (!gdb) return;
    if (gdb->client_fd >= 0) close(gdb->client_fd);
    if (gdb->listen_fd >= 0) close(gdb->listen_fd);
    free(gdb);
}
