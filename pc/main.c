/*
 * ucvm - Microcontroller Virtual Machine
 * Unified PC entry point — AVR and 8051 architectures
 *
 * Select architecture with -a avr|8051 (default: avr)
 */
#include "../src/avr/avr_cpu.h"
#include "../src/avr/avr_periph.h"
#include "../src/mcs51/mcs51_cpu.h"
#include "../src/mcs51/mcs51_periph.h"
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

/* Architecture enum */
typedef enum { ARCH_AVR, ARCH_MCS51 } ucvm_arch_t;

/* Max AVR flash: 32KB = 16384 words */
#define MAX_FLASH_WORDS 16384

static uint16_t avr_flash[MAX_FLASH_WORDS];
static volatile int running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ---------- I/O bridge callbacks ---------- */

/* AVR GPIO port names */
static const char *avr_port_names[] = { "B", "C", "D" };
/* MCS51 GPIO port names */
static const char *mcs51_port_names[] = { "P0", "P1", "P2", "P3" };

static uint8_t last_gpio[8] = {0};
static ucvm_arch_t g_arch;

static void pc_bridge_callback(void *ctx, uint8_t type, uint8_t resource, uint8_t value)
{
    (void)ctx;
    switch (type) {
    case IO_PERIPH_GPIO:
        if (resource < 8 && value != last_gpio[resource]) {
            if (g_arch == ARCH_AVR) {
                const char *name = resource < 3 ? avr_port_names[resource] : "?";
                fprintf(stderr, "[GPIO] PORT%s = 0x%02X (", name, value);
            } else {
                const char *name = resource < 4 ? mcs51_port_names[resource] : "?";
                fprintf(stderr, "[GPIO] %s = 0x%02X (", name, value);
            }
            for (int i = 7; i >= 0; i--)
                fprintf(stderr, "%c", (value & (1 << i)) ? '1' : '0');
            fprintf(stderr, ")\n");
            last_gpio[resource] = value;
        }
        break;
    case IO_PERIPH_UART:
        if (g_arch == ARCH_MCS51) {
            putchar(value);
            fflush(stdout);
        }
        /* AVR UART handled by TX drain loop */
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

/* ---------- Usage ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <firmware.hex>\n"
        "\n"
        "Options:\n"
        "  -a <arch>     Architecture: avr (default) or 8051\n"
        "  -v <variant>  Variant name (default: atmega328p / at89s52)\n"
        "  -g <port>     Enable GDB stub on TCP port\n"
        "  -c <freq>     CPU frequency in Hz (default: 16000000 / 11059200)\n"
        "  -q            Quiet: suppress GPIO messages\n"
        "  -h            Show this help\n"
        "\n"
        "AVR variants:   atmega328p, attiny85\n"
        "8051 variants:  at89s52\n",
        prog);
}

/* ================================================================== */
/*  AVR emulation                                                     */
/* ================================================================== */

static int run_avr(const char *variant_name, const char *hex_file,
                   int gdb_port, uint32_t cpu_freq, int quiet)
{
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
        fprintf(stderr, "Error: unknown AVR variant '%s'\n", variant_name);
        return 1;
    }

    fprintf(stderr, "ucvm: AVR %s, %u bytes flash, %u Hz\n",
            variant->name, variant->flash_size, cpu_freq);

    /* Load firmware */
    memset(avr_flash, 0xFF, sizeof(avr_flash));
    uint32_t flash_words = variant->flash_size / 2;
    if (flash_words > MAX_FLASH_WORDS)
        flash_words = MAX_FLASH_WORDS;

    if (fw_load(hex_file, avr_flash, flash_words) != 0) {
        fprintf(stderr, "Error: failed to load '%s'\n", hex_file);
        return 1;
    }
    fprintf(stderr, "ucvm: loaded '%s'\n", hex_file);

    /* Initialize CPU */
    avr_cpu_t *cpu = avr_cpu_init(variant, avr_flash, variant->flash_size);
    if (!cpu) {
        fprintf(stderr, "Error: CPU init failed\n");
        return 1;
    }

    /* I/O bridge callback */
    (void)quiet;
    cpu->bridge_cb = pc_bridge_callback;
    cpu->bridge_ctx = NULL;

    /* Attach virtual I2C slave at address 0x50 (like an EEPROM) for testing */
    if (cpu->periph_twi) {
        avr_twi_bus_t *i2c_bus = avr_twi_create_virtual_slave(0x50, !quiet);
        avr_twi_set_bus(cpu->periph_twi, i2c_bus);
        fprintf(stderr, "ucvm: virtual I2C slave at 0x50\n");
    }

    /* Signal handler + stdin */
    signal(SIGINT, sigint_handler);
    setup_stdin_nonblock();

    /* GDB stub */
    gdb_state_t *gdb = NULL;
    if (gdb_port > 0) {
        gdb = gdb_init_avr(cpu, avr_flash, flash_words, gdb_port);
        if (!gdb) {
            fprintf(stderr, "Error: failed to start GDB stub on port %d\n", gdb_port);
            restore_stdin();
            avr_cpu_free(cpu);
            return 1;
        }
        fprintf(stderr, "ucvm: GDB stub listening on port %d\n", gdb_port);
        fprintf(stderr, "ucvm: waiting for GDB connection...\n");
    }

    /* Main emulation loop */
    uint32_t step_batch = 1000;
    fprintf(stderr, "ucvm: running...\n");

    while (running) {
        if (gdb) {
            gdb_poll(gdb);
            if (gdb_should_stop(gdb))
                break;
            if (!gdb_is_running(gdb)) {
                usleep(1000);
                continue;
            }
        }

        for (uint32_t i = 0; i < step_batch && running; i++) {
            if (cpu->state == AVR_STATE_HALTED ||
                cpu->state == AVR_STATE_BREAK) {
                if (gdb) {
                    gdb_notify_stop(gdb, cpu->state);
                    break;
                }
                running = 0;
                break;
            }
            if (cpu->state == AVR_STATE_SLEEPING) {
                avr_cpu_check_irq(cpu);
                if (cpu->state == AVR_STATE_SLEEPING) {
                    cpu->cycles += 1;
                    if (cpu->periph_timer)
                        avr_timer0_tick(cpu, cpu->periph_timer, 1);
                    avr_cpu_check_irq(cpu);
                }
                continue;
            }

            if (gdb && gdb_check_breakpoint(gdb, cpu->pc)) {
                gdb_notify_stop(gdb, AVR_STATE_BREAK);
                break;
            }

            avr_cpu_step(cpu);

            if (gdb && gdb_is_single_stepping(gdb)) {
                gdb_notify_stop(gdb, AVR_STATE_RUNNING);
                break;
            }
        }

        /* Poll UART */
        if (cpu->periph_uart) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1)
                avr_uart_rx_push(cpu, cpu->periph_uart, (uint8_t)c);
        }
        if (cpu->periph_uart) {
            int c;
            while ((c = avr_uart_tx_pop(cpu->periph_uart)) >= 0) {
                putchar(c);
                fflush(stdout);
            }
        }
    }

    fprintf(stderr, "\nucvm: stopped after %lu cycles\n",
            (unsigned long)cpu->cycles);

    if (gdb) gdb_free(gdb);
    restore_stdin();
    avr_cpu_free(cpu);
    return 0;
}

/* ================================================================== */
/*  MCS-51 (8051) emulation                                           */
/* ================================================================== */

static int run_mcs51(const char *variant_name, const char *hex_file,
                     int gdb_port, uint32_t cpu_freq, int quiet)
{
    /* Select variant */
    const mcs51_variant_t *variant;
    if (strcasecmp(variant_name, "at89s52") == 0 ||
        strcasecmp(variant_name, "89s52") == 0) {
        variant = &mcs51_at89s52;
    } else {
        fprintf(stderr, "Error: unknown 8051 variant '%s'\n", variant_name);
        return 1;
    }

    fprintf(stderr, "ucvm: 8051 %s, %u bytes code, %u Hz\n",
            variant->name, variant->code_size, cpu_freq);
    (void)quiet;

    /* Initialize CPU */
    mcs51_cpu_t *cpu = mcs51_cpu_init(variant);
    if (!cpu) {
        fprintf(stderr, "Error: CPU init failed\n");
        return 1;
    }

    /* Load firmware */
    memset(cpu->code, 0xFF, cpu->code_size);
    if (fw_load_bytes(hex_file, cpu->code, cpu->code_size) != 0) {
        fprintf(stderr, "Error: failed to load '%s'\n", hex_file);
        mcs51_cpu_free(cpu);
        return 1;
    }
    fprintf(stderr, "ucvm: loaded '%s'\n", hex_file);

    /* I/O bridge callback */
    cpu->bridge_cb = pc_bridge_callback;
    cpu->bridge_ctx = NULL;

    /* Signal handler + stdin */
    signal(SIGINT, sigint_handler);
    setup_stdin_nonblock();

    /* GDB stub */
    gdb_state_t *gdb = NULL;
    if (gdb_port > 0) {
        gdb = gdb_init(cpu, &gdb_target_mcs51, gdb_port);
        if (gdb)
            fprintf(stderr, "ucvm: GDB stub listening on port %d\n", gdb_port);
    }

    /* Main emulation loop */
    uint32_t step_batch = 1000;
    fprintf(stderr, "ucvm: running...\n");

    while (running) {
        if (gdb) {
            gdb_poll(gdb);
            if (gdb_should_stop(gdb))
                break;
            if (!gdb_is_running(gdb)) {
                usleep(1000);
                continue;
            }
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
                break;
            }

            mcs51_cpu_step(cpu);

            if (gdb && gdb_is_single_stepping(gdb)) {
                gdb_notify_stop(gdb, MCS51_STATE_RUNNING);
                break;
            }
        }
    }

    fprintf(stderr, "\nucvm: stopped after %llu cycles (PC=0x%04X)\n",
            (unsigned long long)cpu->cycles, cpu->pc);

    if (gdb) gdb_free(gdb);
    restore_stdin();
    mcs51_cpu_free(cpu);
    return 0;
}

/* ================================================================== */
/*  Main                                                              */
/* ================================================================== */

int main(int argc, char *argv[])
{
    const char *arch_name = "avr";
    const char *variant_name = NULL; /* NULL = use default for arch */
    const char *hex_file = NULL;
    int gdb_port = 0;
    uint32_t cpu_freq = 0; /* 0 = use default for arch */
    int quiet = 0;

    int opt;
    while ((opt = getopt(argc, argv, "a:v:g:c:qh")) != -1) {
        switch (opt) {
        case 'a': arch_name = optarg; break;
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

    /* Select architecture */
    if (strcasecmp(arch_name, "avr") == 0) {
        g_arch = ARCH_AVR;
        if (!variant_name) variant_name = "atmega328p";
        if (!cpu_freq) cpu_freq = 16000000;
        return run_avr(variant_name, hex_file, gdb_port, cpu_freq, quiet);
    } else if (strcasecmp(arch_name, "8051") == 0 ||
               strcasecmp(arch_name, "mcs51") == 0) {
        g_arch = ARCH_MCS51;
        if (!variant_name) variant_name = "at89s52";
        if (!cpu_freq) cpu_freq = 11059200;
        return run_mcs51(variant_name, hex_file, gdb_port, cpu_freq, quiet);
    } else {
        fprintf(stderr, "Error: unknown architecture '%s' (use avr or 8051)\n",
                arch_name);
        return 1;
    }
}
