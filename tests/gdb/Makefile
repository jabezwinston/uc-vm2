MCU     = atmega328p
F_CPU   = 16000000
CC      = avr-gcc
OBJCOPY = avr-objcopy
CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU)UL -O0 -g -Wall

TARGET  = test_program

.PHONY: all clean

all: $(TARGET).hex $(TARGET).elf

$(TARGET).elf: test_program.c
	$(CC) $(CFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

clean:
	rm -f $(TARGET).elf $(TARGET).hex
