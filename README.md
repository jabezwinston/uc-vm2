# ucvm - Microcontroller Virtual Machine

Cycle-accurate microcontroller emulator supporting **AVR** (ATMega328P, ATtiny85) and **8051** (AT89S52) architectures. Runs on x86 Linux or ESP32 with I/O bridging to real hardware.

## Features

- Cycle-accurate AVR emulation (all ~130 instructions, SREG flags, interrupts)
- Cycle-accurate 8051 emulation (all 256 opcodes, 2-level priority interrupts)
- Peripherals: GPIO, UART, Timer, TWI/I2C, ADC stub
- GDB remote debugging over TCP (AVR via avr-gdb, 8051 via Python debugger)
- ESP32: dual-core operation, WiFi web UI, I/O bridging to real GPIO/UART/I2C/ADC
- Single unified binary for both architectures
- Runs Arduino sketches compiled with arduino-cli

## Quick Start

### Prerequisites

```sh
# AVR toolchain
sudo apt install gcc-avr avr-libc avr-gdb

# 8051 toolchain
sudo apt install sdcc

# Arduino CLI (optional, for .ino sketches)
# https://arduino.github.io/arduino-cli/installation/

# ESP-IDF 6.x (for ESP32 build)
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
```

### Build (PC)

```sh
make            # builds ./ucvm (AVR + 8051)
```

### Run

```sh
# AVR (default architecture)
./ucvm firmware.hex

# 8051
./ucvm -a 8051 firmware.ihx

# All options
./ucvm -h
```

```
Usage: ./ucvm [options] <firmware.hex>

Options:
  -a <arch>     Architecture: avr (default) or 8051
  -v <variant>  Variant name (default: atmega328p / at89s52)
  -g <port>     Enable GDB stub on TCP port
  -c <freq>     CPU frequency in Hz (default: 16000000 / 11059200)
  -q            Quiet: suppress GPIO messages
  -h            Show this help

AVR variants:   atmega328p, attiny85
8051 variants:  at89s52
```

## Building Firmware

### AVR (avr-gcc)

```sh
# Compile
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -Wall -o main.elf main.c

# Convert to Intel HEX
avr-objcopy -O ihex -R .eeprom main.elf main.hex

# Run on ucvm
./ucvm main.hex
```

### AVR (Arduino CLI)

```sh
# Compile an Arduino sketch
arduino-cli compile --fqbn arduino:avr:uno --output-dir build sketch/sketch.ino

# Run the .hex on ucvm
./ucvm build/sketch.ino.hex
```

### 8051 (SDCC)

```sh
# Compile
sdcc -mmcs51 --opt-code-size -o main.ihx main.c

# With debug symbols (for gdb8051)
sdcc -mmcs51 --opt-code-size --debug -o main.ihx main.c

# Run on ucvm
./ucvm -a 8051 main.ihx
```

## Examples

Build and run the included examples:

```sh
# Build all AVR examples
make -C examples/avr/blink
make -C examples/avr/uart_hello
make -C examples/avr/i2c_bare

# Build all 8051 examples
make examples51

# Run
./ucvm examples/avr/uart_hello/uart_hello.hex
./ucvm examples/avr/i2c_bare/i2c_bare.hex
./ucvm -a 8051 examples/mcs51/uart_hello/main.ihx
./ucvm -a 8051 examples/mcs51/blink/main.ihx
./ucvm -a 8051 examples/mcs51/timer_int/main.ihx
```

| Example | Arch | Description |
|---------|------|-------------|
| `avr/blink` | AVR | Toggle PORTB (GPIO output) |
| `avr/uart_hello` | AVR | Print "Hello from ucvm!" via UART |
| `avr/i2c_bare` | AVR | Bare-C TWI write/read to virtual I2C slave |
| `avr/i2c_wire` | AVR | Arduino Wire library I2C test |
| `avr/tiny_blink` | AVR | ATtiny85 blink |
| `mcs51/blink` | 8051 | Toggle P1 (GPIO output) |
| `mcs51/uart_hello` | 8051 | Print "Hello from 8051!" via UART |
| `mcs51/timer_int` | 8051 | Timer0 interrupt toggles P1.0 |
| `mcs51/ext_int` | 8051 | External INT0 interrupt |

### Arduino sketches

Arduino IDE-compiled sketches run directly. The emulator handles `millis()`, `delay()`, `Serial`, `Wire`, `digitalRead`/`Write`, and `analogRead`.

```sh
# Compile any Arduino example
arduino-cli compile --fqbn arduino:avr:uno --output-dir /tmp sketch.ino
./ucvm /tmp/sketch.ino.hex
```

## GDB Debugging

### AVR (avr-gdb)

```sh
# Terminal 1: start ucvm with GDB stub
./ucvm -g 1234 firmware.hex

# Terminal 2: connect avr-gdb
avr-gdb firmware.elf
(gdb) target remote :1234
(gdb) break main
(gdb) continue
(gdb) info registers
(gdb) print variable_name
(gdb) step
(gdb) backtrace
```

Compile with `-g` for debug symbols:

```sh
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -O0 -g -o firmware.elf firmware.c
avr-objcopy -O ihex -R .eeprom firmware.elf firmware.hex
./ucvm -g 1234 firmware.hex
```

### 8051 (gdb8051.py)

Since SDCC doesn't have a GDB backend, ucvm includes a Python debugger that parses SDCC `.cdb` debug files:

```sh
# Terminal 1: start ucvm with GDB stub
./ucvm -a 8051 -g 1234 firmware.ihx

# Terminal 2: connect gdb8051
python3 tools/gdb8051.py -p 1234 firmware.cdb
```

```
(gdb8051) break main
(gdb8051) continue
(gdb8051) info registers
(gdb8051) info sfr
(gdb8051) print P1
(gdb8051) list
(gdb8051) step
(gdb8051) backtrace
(gdb8051) x /16x 0x20
(gdb8051) quit
```

Compile with `--debug` for `.cdb` symbol files:

```sh
sdcc -mmcs51 --debug -o firmware.ihx firmware.c
```

### GDB on ESP32 (over WiFi)

When running on ESP32, the GDB stub listens on the WiFi IP:

```sh
# Connect avr-gdb to ESP32
avr-gdb firmware.elf
(gdb) target remote 192.168.0.2:1234
(gdb) break main
(gdb) continue
```

### Automated GDB Tests

```sh
# Build test program and run 29 GDB tests
make -C tests/gdb
python3 tests/gdb/test_gdb.py

# Non-invasive debugging verification (8 tests)
python3 tests/gdb/test_noninvasive.py
```

### VS Code Integration

The project includes full VS Code debug configurations for both architectures.

**AVR**: Uses `cppdbg` type with `avr-gdb` connecting to ucvm's GDB RSP stub. Requires the C/C++ extension.

**8051**: Uses a custom DAP (Debug Adapter Protocol) adapter (`tools/dap8051.py`) since there is no ELF file or standard GDB for SDCC output. The adapter parses `.cdb` debug symbols and speaks DAP over stdin/stdout, giving full VS Code integration:
- Breakpoints (click gutter or by function name)
- Step In / Step Over / Continue
- Variables sidebar: Registers, SFRs, IRAM
- Watch expressions (SFR names, register names, memory addresses)
- Call stack
- Source-level stepping with `.cdb` line mapping

**Setup:**

1. Install the local debug extension:
   ```
   cd .vscode/extensions/ucvm-8051-debug
   # VS Code auto-discovers extensions in .vscode/extensions/
   ```

2. Open the workspace in VS Code, go to Run & Debug (Ctrl+Shift+D)

3. Select a configuration from the dropdown:

| Configuration | What it does |
|---------------|-------------|
| AVR: Debug i2c_bare | avr-gdb → ucvm, stops at entry |
| AVR: Debug test_program | avr-gdb → ucvm with test program |
| AVR: Debug on ESP32 (WiFi) | avr-gdb → ESP32 at 192.168.0.2:1234 |
| 8051: Debug blink | DAP adapter → ucvm 8051 RSP stub |
| 8051: Debug timer_int | DAP adapter → ucvm 8051 timer interrupt |
| 8051: Debug uart_hello | DAP adapter → ucvm 8051 UART |
| 8051: Debug on ESP32 (WiFi) | DAP adapter → ESP32 at 192.168.0.2:1234 |
| ucvm: Debug emulator (AVR) | gdb → ucvm binary itself |
| ucvm: Debug emulator (8051) | gdb → ucvm binary in 8051 mode |

4. Press F5 to start debugging. The preLaunchTask automatically builds and starts ucvm with the GDB stub.

## ESP32

### Build

```sh
source ~/Git_repos/esp-idf/export.sh
cd esp32
idf.py set-target esp32
idf.py menuconfig    # Configure WiFi SSID/password, arch, GDB
idf.py build
idf.py flash -p /dev/ttyUSB0
```

### Configuration (menuconfig)

Under **ucvm Configuration**:

| Option | Default | Description |
|--------|---------|-------------|
| Enable AVR emulation | y | Include AVR support |
| Enable 8051 emulation | y | Include 8051 support |
| Default architecture | AVR | Architecture on first boot |
| WiFi SSID | D-Link_DIR-615 | WiFi network name |
| WiFi Password | local1234 | WiFi password |
| Enable GDB stub | y | GDB debugging over TCP |
| GDB TCP port | 1234 | Port for GDB connections |

### Partition Table

| Partition | Size | Purpose |
|-----------|------|---------|
| factory | 1 MB | Application firmware |
| firmware | 192 KB | SPIFFS: MCU firmware (.hex/.ihx) |
| web | 64 KB | SPIFFS: Web UI (index.html) |
| nvs | 24 KB | Non-volatile storage (config) |

### Upload Firmware via curl

Once the ESP32 is running and connected to WiFi:

```sh
# Upload AVR firmware
curl -s -X POST -H "Content-Type: text/plain" \
  --data-binary @firmware.hex \
  http://192.168.0.2/api/firmware

# Upload 8051 firmware
curl -s -X POST -H "Content-Type: text/plain" \
  --data-binary @firmware.ihx \
  http://192.168.0.2/api/firmware

# Check status
curl -s http://192.168.0.2/api/status | python3 -m json.tool

# Reset CPU
curl -s -X POST http://192.168.0.2/api/reset

# Halt CPU
curl -s -X POST -d '{"action":"halt"}' http://192.168.0.2/api/reset
```

### Web API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI |
| `/api/status` | GET | CPU state, cycles, variant, arch |
| `/api/config` | GET | I/O bridge configuration |
| `/api/config` | POST | Modify bridge config (add/del/save) |
| `/api/firmware` | POST | Upload Intel HEX firmware |
| `/api/reset` | POST | Reset or halt CPU |

### I/O Bridge Configuration

Configure via web UI or REST API. Maps emulated MCU peripherals to ESP32 hardware:

```sh
# Add GPIO bridge: AVR PORTB.5 (Arduino LED) -> ESP32 GPIO2
curl -s -X POST -d '{"action":"add","type":1,"avr":5,"host":2,"flags":0}' \
  http://192.168.0.2/api/config

# Add UART bridge: AVR UART0 -> ESP32 UART1
curl -s -X POST -d '{"action":"add","type":2,"avr":0,"host":0,"flags":0}' \
  http://192.168.0.2/api/config

# Add I2C bridge: AVR TWI -> ESP32 I2C (SDA=21, SCL=22)
curl -s -X POST -d '{"action":"add","type":5,"avr":0,"host":0,"flags":0}' \
  http://192.168.0.2/api/config

# Save to NVS (persists across reboots)
curl -s -X POST -d '{"action":"save"}' http://192.168.0.2/api/config
```

Bridge types: 1=GPIO, 2=UART, 3=ADC, 5=I2C

## Architecture

```
src/
  avr/          AVR CPU core, decoder, peripherals, variants
  mcs51/        8051 CPU core, decoder, peripherals, variant
  gdb/          GDB RSP stub (shared) + per-arch target ops
  io/           I/O bridge framework
  util/         Intel HEX loader
pc/
  main.c        Unified PC entry point
esp32/
  main/         ESP32 app: WiFi, web server, I/O bridge backends
  components/   Shared emulator as ESP-IDF component
  web/          Web UI (SPIFFS)
examples/
  avr/          AVR examples (avr-gcc + Arduino)
  mcs51/        8051 examples (SDCC)
tests/
  gdb/          GDB integration tests (29 tests + 8 non-invasive)
tools/
  gdb8051.py    Python 8051 debugger (parses SDCC .cdb files)
```

## Virtual I2C Slave (PC)

When running on PC, ucvm automatically attaches a virtual I2C slave at address **0x50** (EEPROM-like device with 256-byte register map). This allows testing I2C firmware without real hardware:

```sh
# Bare-C I2C test (writes 0xAA,0xBB,0xCC to reg 0x10, reads back)
./ucvm examples/avr/i2c_bare/i2c_bare.hex

# Arduino Wire test (writes 0xDE,0xAD,0xBE,0xEF, reads back)
./ucvm examples/avr/i2c_wire/i2c_wire.ino.hex
```

## License

MIT
