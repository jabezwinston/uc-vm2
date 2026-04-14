SDCC    = sdcc
SDFLAGS = -mmcs51 --opt-code-size

.PHONY: all clean

all: blink/main.ihx uart_hello/main.ihx timer_int/main.ihx ext_int/main.ihx

blink/main.ihx: blink/main.c
	$(SDCC) $(SDFLAGS) -o blink/ $<

uart_hello/main.ihx: uart_hello/main.c
	$(SDCC) $(SDFLAGS) -o uart_hello/ $<

timer_int/main.ihx: timer_int/main.c
	$(SDCC) $(SDFLAGS) --debug -o timer_int/ $<

ext_int/main.ihx: ext_int/main.c
	$(SDCC) $(SDFLAGS) --debug -o ext_int/ $<

clean:
	rm -f */*.ihx */*.hex */*.lk */*.map */*.mem */*.rel */*.rst */*.sym */*.asm */*.lst */*.cdb
