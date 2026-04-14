/*
 * ucvm - PC entry point
 *
 * Loads AVR firmware from Intel HEX file, runs cycle-accurate emulation.
 * UART TX output goes to stdout. Optional GDB stub on TCP.
 */
#include "../src/core/avr_cpu.h"
#include "../src/periph/avr_periph.h"
#include "../src/util/ihex.h"
#include "../src/gdb/gdb_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

/* Max flash size: ATMega328P = 32KB = 16384 words */
#define MAX_FLASH_WORDS 16384

static uint16_t flash[MAX_FLASH_WORDS];
static volatile int running = 1;
static avr_cpu_t *g_cpu = NULL;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ---------- I/O bridge callback for PC ---------- */

/* GPIO port names */
static const char *port_names[] = { "B", "C", "D" };
static uint8_t last_gpio[8] = {0};

static void pc_bridge_callback(void *ctx, uint8_t type, uint8_t resource, uint8_t value)
{
    (void)ctx;
    switch (type) {
    case 1: /* GPIO */
        if (resource < 8 && value != last_gpio[resource]) {
            const char *name = resource < 3 ? port_names[resource] : "?";
            fprintf(stderr, "[GPIO] PORT%s = 0x%02X", name, value);
            /* Show individual pins */
            fprintf(stderr, " (");
            for (int i = 7; i >= 0; i--)
                fprintf(stderr, "%c", (value & (1 << i)) ? '1' : '0');
            fprintf(stderr, ")\n");
            last_gpio[resource] = value;
        }
        break;
    case 2: /* UART — handled by TX drain loop in main */
        break;
    default:
        break;
    }
}

/* ---------- Non-blocking stdin for UART RX ---------- */

static struct termios orig_termios;
static int termios_saved = 0;

static void setup_stdin_nonblock(void)
{
    if (!isatty(STDIN_FILENO))
        return;

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

static void poll_stdin_uart(avr_cpu_t *cpu)
{
    if (!cpu->periph_uart)
        return;
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
        avr_uart_rx_push(cpu, cpu->periph_uart, (uint8_t)c);
}

/* ---------- Usage ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <firmware.hex>\n"
        "\n"
        "Options:\n"
        "  -v <variant>  ATmega328P (default) or ATtiny85\n"
        "  -g <port>     Enable GDB stub on TCP port\n"
        "  -c <freq>     CPU frequency in Hz (default: 16000000)\n"
        "  -q            Quiet: suppress GPIO messages\n"
        "  -h            Show this help\n",
        prog);
}

/* ---------- Main ---------- */

int main(int argc, char *argv[])
{
    const char *variant_name = "atmega328p";
    const char *hex_file = NULL;
    int gdb_port = 0;
    uint32_t cpu_freq = 16000000;
    int quiet = 0;

    /* Parse arguments */
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
    const avr_variant_t *variant;
    if (strcasecmp(variant_name, "atmega328p") == 0 ||
        strcasecmp(variant_name, "328p") == 0) {
        variant = &avr_atmega328p;
    } else if (strcasecmp(variant_name, "attiny85") == 0 ||
               strcasecmp(variant_name, "tiny85") == 0 ||
               strcasecmp(variant_name, "t85") == 0) {
        variant = &avr_attiny85;
    } else {
        fprintf(stderr, "Error: unknown variant '%s'\n", variant_name);
        return 1;
    }

    fprintf(stderr, "ucvm: %s, %u bytes flash, %u Hz\n",
            variant->name, variant->flash_size, cpu_freq);

    /* Load firmware */
    memset(flash, 0xFF, sizeof(flash));
    uint32_t flash_words = variant->flash_size / 2;
    if (flash_words > MAX_FLASH_WORDS)
        flash_words = MAX_FLASH_WORDS;

    if (ihex_load(hex_file, flash, flash_words) != 0) {
        fprintf(stderr, "Error: failed to load '%s'\n", hex_file);
        return 1;
    }
    fprintf(stderr, "ucvm: loaded '%s'\n", hex_file);

    /* Initialize CPU */
    avr_cpu_t *cpu = avr_cpu_init(variant, flash, variant->flash_size);
    if (!cpu) {
        fprintf(stderr, "Error: failed to initialize CPU\n");
        return 1;
    }
    g_cpu = cpu;

    /* Set up I/O bridge callback */
    if (!quiet) {
        cpu->bridge_cb = pc_bridge_callback;
        cpu->bridge_ctx = NULL;
    } else {
        /* Still need UART output */
        cpu->bridge_cb = pc_bridge_callback;
    }

    /* Set up signal handler */
    signal(SIGINT, sigint_handler);

    /* Set up non-blocking stdin for UART input */
    setup_stdin_nonblock();

    /* GDB stub */
    gdb_state_t *gdb = NULL;
    if (gdb_port > 0) {
        gdb = gdb_init(cpu, flash, flash_words, gdb_port);
        if (!gdb) {
            fprintf(stderr, "Error: failed to start GDB stub on port %d\n", gdb_port);
            restore_stdin();
            avr_cpu_free(cpu);
            return 1;
        }
        fprintf(stderr, "ucvm: GDB stub listening on port %d\n", gdb_port);
        fprintf(stderr, "ucvm: waiting for GDB connection...\n");
        gdb_wait_connect(gdb);
        fprintf(stderr, "ucvm: GDB connected\n");
    }

    /* Main emulation loop */
    uint64_t cycle_limit = 0; /* 0 = no limit */
    uint32_t step_batch = 1000; /* steps between I/O polls */

    fprintf(stderr, "ucvm: running...\n");

    while (running) {
        if (gdb) {
            /* GDB-controlled execution */
            gdb_poll(gdb);
            if (gdb_should_stop(gdb))
                break;
            /* Wait for GDB to issue 'c' or 's' before running */
            if (!gdb_is_running(gdb))
                goto gdb_wait;
        }

        for (uint32_t i = 0; i < step_batch && running; i++) {
            if (cpu->state == AVR_STATE_HALTED ||
                cpu->state == AVR_STATE_BREAK) {
                if (gdb) {
                    gdb_notify_stop(gdb, cpu->state);
                    goto gdb_wait;
                }
                running = 0;
                break;
            }
            if (cpu->state == AVR_STATE_SLEEPING) {
                /* In sleep mode, still tick timers and check interrupts */
                avr_cpu_check_irq(cpu);
                if (cpu->state == AVR_STATE_SLEEPING) {
                    /* Still sleeping — advance cycles and tick timer */
                    cpu->cycles += 1;
                    if (cpu->periph_timer)
                        avr_timer0_tick(cpu, cpu->periph_timer, 1);
                    avr_cpu_check_irq(cpu);
                }
                continue;
            }

            if (gdb && gdb_check_breakpoint(gdb, cpu->pc)) {
                gdb_notify_stop(gdb, AVR_STATE_BREAK);
                goto gdb_wait;
            }

            avr_cpu_step(cpu);

            if (gdb && gdb_is_single_stepping(gdb)) {
                gdb_notify_stop(gdb, AVR_STATE_RUNNING);
                goto gdb_wait;
            }
        }

        /* Poll UART RX from stdin */
        poll_stdin_uart(cpu);

        /* Drain UART TX */
        if (cpu->periph_uart) {
            int c;
            while ((c = avr_uart_tx_pop(cpu->periph_uart)) >= 0) {
                putchar(c);
                fflush(stdout);
            }
        }

        if (cycle_limit > 0 && cpu->cycles >= cycle_limit) {
            fprintf(stderr, "\nucvm: cycle limit reached (%lu)\n",
                    (unsigned long)cycle_limit);
            break;
        }
        continue;

    gdb_wait:
        /* Wait for GDB command */
        while (running && gdb && !gdb_is_running(gdb)) {
            gdb_poll(gdb);
            if (gdb_should_stop(gdb)) {
                running = 0;
                break;
            }
            usleep(1000);
        }
    }

    fprintf(stderr, "\nucvm: stopped after %lu cycles\n",
            (unsigned long)cpu->cycles);

    /* Cleanup */
    if (gdb) gdb_free(gdb);
    restore_stdin();
    avr_cpu_free(cpu);

    return 0;
}
