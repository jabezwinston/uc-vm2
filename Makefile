# ucvm - Microcontroller Virtual Machine
# PC build (x86 Linux)

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -I.
LDFLAGS =

SRCS = src/core/avr_cpu.c \
       src/core/avr_decode.c \
       src/periph/avr_timer.c \
       src/periph/avr_gpio.c \
       src/periph/avr_uart.c \
       src/io/io_bridge.c \
       src/gdb/gdb_stub.c \
       src/util/ihex.c \
       variants/atmega328p.c \
       variants/attiny85.c \
       pc/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean examples

all: ucvm

ucvm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

examples:
	$(MAKE) -C examples

clean:
	rm -f $(OBJS) ucvm
	$(MAKE) -C examples clean
