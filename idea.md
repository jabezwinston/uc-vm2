# Microcontroller - Virtual Machine

- Emulate AVR (with clock cycle accuracy) on ESP32 or x86 PC
- Instruction emulation in software
- Support I/O bridging
  - Examples:  
    - AVR GPIO to ESP32 GPIO
    - AVR UART to ESP32 UART (or) bit-banged UART (or) TCP/UDP
    - PIC ADC to ESP32 ADC
  - Configure I/O bridging via web interface
    - I/O bridge information stored in binary format in flash
    - Figure out optimal binary format for all supported I/O bridge types
  - Extremely low I/O latency is needed (mandatory)
  - Interrupt handling (all I/O)
- x86 PC, use webassembly & show I/O in web interface
- AVR supports multiple vatiants
  - For now focus on ATMega328P & ATtiny85 
- Implement GDB stub for debugging emulated AVR
- Utilize both cores of ESP32
- Use ESP-IDF for development ($HOME/Git-repos/esp-idf)
- For x86 PC, use Makefile (not CMake) for build system
- Extendable to be used with 8051 or PIC
- Be wary of timing issues, resource constraints(RAM) , and I/O latency. This is not QEMU !!
- RAM contraint - 48kB (don't exceed). Keep as much as possible in flash.

## Testing
- Create examples for AVR & verify
- Test on PC first, then on ESP32
