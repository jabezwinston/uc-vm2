MCU     = atmega328p
F_CPU   = 16000000UL
CC      = avr-gcc
CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -Wall
OBJCOPY = avr-objcopy

all: i2c_bare.hex

i2c_bare.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

i2c_bare.hex: i2c_bare.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

clean:
	rm -f *.elf *.hex *.o

.PHONY: all clean
