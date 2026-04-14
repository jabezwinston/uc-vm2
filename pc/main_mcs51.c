/*
 * ucvm51 - 8051 emulator PC entry point
 *
 * Loads MCS-51 firmware from Intel HEX, runs cycle-accurate emulation.
 * UART TX → stdout. Optional GDB stub on TCP.
 */
#include "../src/mcs51/mcs51_cpu.h"
#include "../src/util/ihex.h"
#include "../src/gdb/gdb_stub.h"
#include "../src/gdb/gdb_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

static volatile int running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ---------- I/O bridge callback ---------- */

#define IO_BRIDGE_GPIO 1
#define IO_BRIDGE_UART 2

static const char *port_names[] = { "P0", "P1", "P2", "P3" };
static uint8_t last_port[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

static void bridge_callback(void *ctx, uint8_t type, uint8_t resource, uint8_t value)
{
    (void)ctx;
    switch (type) {
    case IO_BRIDGE_GPIO:
        if (resource < 4 && value != last_port[resource]) {
            fprintf(stderr, "[GPIO] %s = 0x%02X (", port_names[resource], value);
            for (int i = 7; i >= 0; i--)
                fprintf(stderr, "%c", (value & (1 << i)) ? '1' : '0');
            fprintf(stderr, ")\n");
            last_port[resource] = value;
        }
        break;
    case IO_BRIDGE_UART:
        putchar(value);
        fflush(stdout);
        break;
    }
}

/* ---------- Non-blocking stdin for UART RX ---------- */

static struct termios orig_termios;
static int termios_saved = 0;

static void setup_stdin_nonblock(void)
{
    if (!isatty(STDIN_FILENO)) return;
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    termios_saved = 1;
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

static void restore_stdin(void)
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    }
}

/* ---------- Usage ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <firmware.ihx>\n"
        "\nOptions:\n"
        "  -v <variant>  at89s52 (default)\n"
        "  -g <port>     Enable GDB stub on TCP port\n"
        "  -c <freq>     CPU frequency in Hz (default: 11059200)\n"
        "  -q            Quiet: suppress GPIO messages\n"
        "  -h            Show this help\n",
        prog);
}

/* ---------- Main ---------- */

int main(int argc, char *argv[])
{
    const char *variant_name = "at89s52";
    const char *hex_file = NULL;
    int gdb_port = 0;
    uint32_t cpu_freq = 11059200; /* typical 8051 crystal */
    int quiet = 0;

    int opt;
    while ((opt = getopt(argc, argv, "v:g:c:qh")) != -1) {
        switch (opt) {
        case 'v': variant_name = optarg; break;
        case 'g': gdb_port = atoi(optarg); break;
        case 'c': cpu_freq = (uint32_t)atol(optarg); break;
        case 'q': quiet = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Error: no firmware file specified\n");
        usage(argv[0]);
        return 1;
    }
    hex_file = argv[optind];

    /* Select variant */
    const mcs51_variant_t *variant;
    if (strcasecmp(variant_name, "at89s52") == 0 ||
        strcasecmp(variant_name, "89s52") == 0) {
        variant = &mcs51_at89s52;
    } else {
        fprintf(stderr, "Error: unknown variant '%s'\n", variant_name);
        return 1;
    }

    fprintf(stderr, "ucvm51: %s, %u bytes code, %u Hz\n",
            variant->name, variant->code_size, cpu_freq);

    /* Initialize CPU */
    mcs51_cpu_t *cpu = mcs51_cpu_init(variant);
    if (!cpu) {
        fprintf(stderr, "Error: CPU init failed\n");
        return 1;
    }

    /* Load firmware */
    memset(cpu->code, 0xFF, cpu->code_size);
    if (ihex_load_bytes(hex_file, cpu->code, cpu->code_size) != 0) {
        fprintf(stderr, "Error: failed to load '%s'\n", hex_file);
        mcs51_cpu_free(cpu);
        return 1;
    }
    fprintf(stderr, "ucvm51: loaded '%s'\n", hex_file);

    /* Set up I/O bridge callback */
    cpu->bridge_cb = bridge_callback;
    cpu->bridge_ctx = NULL;

    /* Signal handler */
    signal(SIGINT, sigint_handler);
    setup_stdin_nonblock();

    /* GDB stub */
    gdb_state_t *gdb = NULL;
    if (gdb_port > 0) {
        gdb = gdb_init(cpu, &gdb_target_mcs51, gdb_port);
        if (gdb) {
            fprintf(stderr, "ucvm51: GDB stub listening on port %d\n", gdb_port);
            fprintf(stderr, "ucvm51: waiting for GDB connection...\n");
            gdb_wait_connect(gdb);
            fprintf(stderr, "ucvm51: GDB connected\n");
        }
    }

    /* Main emulation loop */
    uint32_t step_batch = 1000;
    fprintf(stderr, "ucvm51: running...\n");

    while (running) {
        if (gdb) {
            gdb_poll(gdb);
            if (gdb_should_stop(gdb))
                break;
            if (!gdb_is_running(gdb))
                goto gdb_wait;
        }

        for (uint32_t i = 0; i < step_batch && running; i++) {
            if (cpu->state == MCS51_STATE_HALTED ||
                cpu->state == MCS51_STATE_BREAK) {
                if (!gdb) running = 0;
                break;
            }
            if (cpu->state == MCS51_STATE_SLEEPING) {
                cpu->cycles += 1;
                mcs51_cpu_check_irq(cpu);
                continue;
            }

            if (gdb && gdb_check_breakpoint(gdb, cpu->pc)) {
                gdb_notify_stop(gdb, MCS51_STATE_BREAK);
                goto gdb_wait;
            }

            mcs51_cpu_step(cpu);

            if (gdb && gdb_is_single_stepping(gdb)) {
                gdb_notify_stop(gdb, MCS51_STATE_RUNNING);
                goto gdb_wait;
            }
        }

        /* Poll stdin for UART RX */
        /* (UART peripheral handles pushing to SBUF in Phase 2) */

        continue;

    gdb_wait:
        while (running && gdb && !gdb_is_running(gdb)) {
            gdb_poll(gdb);
            if (gdb_should_stop(gdb)) { running = 0; break; }
            usleep(1000);
        }
    }

    fprintf(stderr, "\nucvm51: stopped after %llu cycles (PC=0x%04X)\n",
            (unsigned long long)cpu->cycles, cpu->pc);

    if (gdb) gdb_free(gdb);
    restore_stdin();
    mcs51_cpu_free(cpu);
    return 0;
}
