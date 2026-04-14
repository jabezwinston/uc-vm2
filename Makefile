# ucvm - Microcontroller Virtual Machine
# PC build (x86 Linux)

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -I.
LDFLAGS =

# ---- Shared sources ----
SHARED_SRCS = src/io/io_bridge.c \
              src/gdb/gdb_stub.c \
              src/util/ihex.c

# ---- GDB target ops (per-architecture) ----
AVR_GDB_SRCS   = src/gdb/gdb_target_avr.c
MCS51_GDB_SRCS = src/gdb/gdb_target_mcs51.c

# ---- AVR emulator ----
AVR_SRCS = src/core/avr_cpu.c \
           src/core/avr_decode.c \
           src/periph/avr_timer.c \
           src/periph/avr_gpio.c \
           src/periph/avr_uart.c \
           variants/atmega328p.c \
           variants/attiny85.c \
           pc/main.c

AVR_OBJS = $(AVR_SRCS:.c=.o) $(SHARED_SRCS:.c=.o) $(AVR_GDB_SRCS:.c=.o)

# ---- 8051 emulator ----
MCS51_SRCS = src/mcs51/mcs51_cpu.c \
             src/mcs51/mcs51_decode.c \
             src/mcs51/mcs51_timer.c \
             src/mcs51/mcs51_uart.c \
             variants/at89s52.c \
             pc/main_mcs51.c

# 8051 shared: io_bridge + ihex + gdb_stub + mcs51 gdb target
MCS51_OBJS = $(MCS51_SRCS:.c=.o) $(SHARED_SRCS:.c=.o) $(MCS51_GDB_SRCS:.c=.o)

# ---- Targets ----
.PHONY: all clean examples examples51

all: ucvm ucvm51

ucvm: $(AVR_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

ucvm51: $(MCS51_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

examples:
	$(MAKE) -C examples

examples51:
	$(MAKE) -C examples/mcs51

clean:
	rm -f $(AVR_OBJS) $(MCS51_OBJS) ucvm ucvm51
	-$(MAKE) -C examples clean 2>/dev/null
	-$(MAKE) -C examples/mcs51 clean 2>/dev/null
