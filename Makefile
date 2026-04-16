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
AVR_SRCS = src/avr/avr_cpu.c \
           src/avr/avr_decode.c \
           src/avr/avr_timer.c \
           src/avr/avr_gpio.c \
           src/avr/avr_uart.c \
           src/avr/avr_twi.c \
           src/avr/avr_twi_slave.c \
           src/avr/avr_mcu_ops.c \
           src/avr/atmega328p.c \
           src/avr/attiny85.c

# ---- 8051 emulator ----
MCS51_SRCS = src/mcs51/mcs51_cpu.c \
             src/mcs51/mcs51_decode.c \
             src/mcs51/mcs51_timer.c \
             src/mcs51/mcs51_uart.c \
             src/mcs51/mcs51_gpio.c \
             src/mcs51/mcs51_mcu_ops.c \
             src/mcs51/at89s52.c

# ---- Unified binary (AVR + 8051) ----
ALL_SRCS = $(AVR_SRCS) $(MCS51_SRCS) $(SHARED_SRCS) \
           $(AVR_GDB_SRCS) $(MCS51_GDB_SRCS) \
           pc/main.c

ALL_OBJS = $(ALL_SRCS:.c=.o)

# ---- Targets ----
.PHONY: all clean examples examples51

all: ucvm

ucvm: $(ALL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

examples:
	$(MAKE) -C examples

examples51:
	$(MAKE) -C examples/mcs51

clean:
	rm -f $(ALL_OBJS) ucvm
	-$(MAKE) -C examples clean 2>/dev/null
	-$(MAKE) -C examples/mcs51 clean 2>/dev/null
