#!/usr/bin/env python3
"""
gdb8051 — Python GDB-like debugger for 8051 (MCS-51) on ucvm.

Connects to ucvm51's GDB RSP stub over TCP.
Parses SDCC .cdb debug files for source-level debugging.

Usage:
    gdb8051.py [-p PORT] [-H HOST] <firmware.cdb>
    gdb8051.py [-p PORT] [-H HOST]                  # no source info

Commands:
    break <func|file:line|0xaddr>   Set breakpoint
    delete <n>                      Delete breakpoint
    step / stepi                    Source-level / instruction step
    next                            Step over function calls
    continue / c                    Resume execution
    print <var|sfr|Rn>              Read variable or register
    info registers                  Show all registers
    info sfr                        Show all SFR values
    info break                      Show breakpoints
    list [file:line]                Show source context
    x /Nx <addr>                    Examine memory (code/iram/xdata)
    backtrace / bt                  Show call stack
    set <var> = <value>             Write register/memory
    reset                           Reset CPU
    detach                          Detach and let CPU run
    quit / q                        Disconnect and exit
"""

import socket
import sys
import os
import re
import readline
import argparse

# ---------- SFR name table ----------

SFR_NAMES = {
    0x80: 'P0',   0x81: 'SP',   0x82: 'DPL',  0x83: 'DPH',  0x87: 'PCON',
    0x88: 'TCON', 0x89: 'TMOD', 0x8A: 'TL0',  0x8B: 'TL1',  0x8C: 'TH0',
    0x8D: 'TH1',  0x90: 'P1',   0x98: 'SCON', 0x99: 'SBUF', 0xA0: 'P2',
    0xA8: 'IE',   0xB0: 'P3',   0xB8: 'IP',   0xC8: 'T2CON',0xC9: 'T2MOD',
    0xCA: 'RCAP2L',0xCB:'RCAP2H',0xCC:'TL2',  0xCD: 'TH2',  0xD0: 'PSW',
    0xE0: 'ACC',  0xF0: 'B',
}
SFR_BY_NAME = {v.lower(): k for k, v in SFR_NAMES.items()}


# ---------- CDB Parser ----------

class CdbParser:
    """Parse SDCC .cdb debug file for symbol and line information."""

    def __init__(self, filename=None):
        self.symbols = {}       # name → {'addr': int, 'type': str, 'scope': str}
        self.functions = {}     # name → addr
        self.addr_to_line = {}  # code_addr → (file, line)
        self.line_to_addr = {}  # (file, line) → code_addr
        self.source_files = {}  # filename → [lines]
        if filename:
            self.parse(filename)

    def parse(self, filename):
        try:
            with open(filename) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith('L:'):
                        self._parse_label(line[2:])
                    elif line.startswith('S:'):
                        self._parse_symbol(line[2:])
                    elif line.startswith('F:'):
                        self._parse_function(line[2:])
        except FileNotFoundError:
            print(f"Warning: CDB file '{filename}' not found — no source info")

    def _parse_label(self, data):
        # L:G$name$scope:ADDR or L:C$file.c$line$scope:ADDR
        m = re.match(r'([^:]+):([0-9A-Fa-f]+)$', data)
        if not m:
            return
        name_part = m.group(1)
        addr = int(m.group(2), 16)

        # Source line mapping: C$file.c$line$...
        cm = re.match(r'C\$([^$]+)\$(\d+)\$', name_part)
        if cm:
            fname = cm.group(1)
            lineno = int(cm.group(2))
            self.addr_to_line[addr] = (fname, lineno)
            self.line_to_addr[(fname, lineno)] = addr
            return

        # Function entry: G$name$0$0
        fm = re.match(r'G\$([^$]+)\$0\$0$', name_part)
        if fm:
            self.functions[fm.group(1)] = addr
            return

        # Global symbol: G$name$scope
        gm = re.match(r'G\$([^$]+)\$', name_part)
        if gm:
            self.symbols[gm.group(1)] = {'addr': addr, 'scope': 'global'}

    def _parse_symbol(self, data):
        # S:G$name$scope({type}),class,...
        m = re.match(r'G\$([^$]+)\$[^(]*\(([^)]*)\),(\w)', data)
        if m:
            name = m.group(1)
            if name in self.symbols:
                self.symbols[name]['type'] = m.group(2)
                self.symbols[name]['class'] = m.group(3)

    def _parse_function(self, data):
        m = re.match(r'G\$([^$]+)\$', data)
        if m:
            name = m.group(1)
            if name not in self.functions:
                self.functions[name] = None  # address filled by L: record

    def get_source_line(self, addr):
        return self.addr_to_line.get(addr)

    def get_addr_for_line(self, filename, lineno):
        return self.line_to_addr.get((filename, lineno))

    def get_func_addr(self, name):
        return self.functions.get(name)

    def load_source(self, filename):
        if filename in self.source_files:
            return self.source_files[filename]
        for search_dir in ['.', 'examples/mcs51/blink', 'examples/mcs51/uart_hello',
                           'examples/mcs51/timer_int', 'examples/mcs51/ext_int']:
            path = os.path.join(search_dir, filename)
            if os.path.exists(path):
                with open(path) as f:
                    lines = f.readlines()
                self.source_files[filename] = lines
                return lines
        return None


# ---------- RSP Client ----------

class RspClient:
    """GDB Remote Serial Protocol client over TCP."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(5)
        # Consume initial data
        try:
            self.sock.recv(256)
        except socket.timeout:
            pass

    def send(self, data):
        """Send a RSP packet and return the response data (without framing)."""
        checksum = sum(ord(c) for c in data) & 0xFF
        pkt = f'${data}#{checksum:02x}'
        self.sock.sendall(pkt.encode())
        return self._recv_packet()

    def _recv_packet(self):
        """Receive a RSP response packet. Returns data string."""
        buf = b''
        deadline = 10  # seconds
        self.sock.settimeout(deadline)
        try:
            while True:
                chunk = self.sock.recv(1024)
                if not chunk:
                    break
                buf += chunk
                # Look for complete packet: $data#xx
                decoded = buf.decode(errors='replace')
                m = re.search(r'\$([^#]*?)#([0-9a-fA-F]{2})', decoded)
                if m:
                    # Send ACK
                    self.sock.sendall(b'+')
                    return m.group(1)
                # Also check for just '+' (ACK to our packet)
                if decoded.strip() == '+':
                    buf = b''
                    continue
        except socket.timeout:
            pass
        return ''

    def send_interrupt(self):
        """Send Ctrl-C to halt execution."""
        self.sock.sendall(b'\x03')
        return self._recv_packet()

    def close(self):
        try:
            self.send('D')  # Detach
        except Exception:
            pass
        self.sock.close()


# ---------- Register decoding ----------

def decode_psw(psw):
    flags = []
    if psw & 0x80: flags.append('CY')
    if psw & 0x40: flags.append('AC')
    if psw & 0x20: flags.append('F0')
    if psw & 0x04: flags.append('OV')
    if psw & 0x01: flags.append('P')
    bank = (psw >> 3) & 3
    return f"0x{psw:02X} [{'|'.join(flags) if flags else '-'}] Bank={bank}"

def parse_regs(hex_str):
    """Parse register hex string: PC(2)+ACC+B+PSW+SP+DPL+DPH+R0-R7(8) = 32 hex chars"""
    if len(hex_str) < 32:
        return None
    regs = {}
    p = 0
    def get(n):
        nonlocal p
        v = int(hex_str[p:p+n*2], 16)
        # Little-endian for multi-byte
        if n == 2:
            lo = int(hex_str[p:p+2], 16)
            hi = int(hex_str[p+2:p+4], 16)
            v = (hi << 8) | lo
        p += n * 2
        return v

    regs['PC']  = get(2)
    regs['ACC'] = get(1)
    regs['B']   = get(1)
    regs['PSW'] = get(1)
    regs['SP']  = get(1)
    regs['DPL'] = get(1)
    regs['DPH'] = get(1)
    regs['DPTR'] = (regs['DPH'] << 8) | regs['DPL']
    for i in range(8):
        regs[f'R{i}'] = get(1)
    return regs


# ---------- Debugger ----------

class Debugger:
    def __init__(self, host, port, cdb_file=None):
        self.cdb = CdbParser(cdb_file)
        print(f"Connecting to {host}:{port}...")
        self.rsp = RspClient(host, port)
        print("Connected.")
        # Initial stop
        resp = self.rsp.send('?')
        self.breakpoints = []
        self.bp_counter = 1

    def show_location(self):
        regs = parse_regs(self.rsp.send('g'))
        if not regs:
            return
        pc = regs['PC']
        loc = self.cdb.get_source_line(pc)
        if loc:
            print(f"  Stopped at {loc[0]}:{loc[1]}  (PC=0x{pc:04X})")
            self._show_source_context(loc[0], loc[1], 1)
        else:
            print(f"  Stopped at PC=0x{pc:04X}")

    def _show_source_context(self, filename, line, context=3):
        lines = self.cdb.load_source(filename)
        if not lines:
            return
        start = max(0, line - context - 1)
        end = min(len(lines), line + context)
        for i in range(start, end):
            marker = '>' if i + 1 == line else ' '
            print(f"  {marker} {i+1:4d}  {lines[i].rstrip()}")

    def cmd_break(self, arg):
        if not arg:
            print("Usage: break <function|file:line|0xaddr>")
            return
        addr = None
        if arg.startswith('0x') or arg.startswith('0X'):
            addr = int(arg, 16)
        elif ':' in arg:
            parts = arg.split(':')
            addr = self.cdb.get_addr_for_line(parts[0], int(parts[1]))
            if addr is None:
                print(f"No code at {arg}")
                return
        else:
            addr = self.cdb.get_func_addr(arg)
            if addr is None:
                # Try as SFR or symbol
                print(f"Unknown symbol '{arg}'")
                return

        resp = self.rsp.send(f'Z0,{addr:x},1')
        bp_num = self.bp_counter
        self.bp_counter += 1
        self.breakpoints.append({'num': bp_num, 'addr': addr, 'name': arg})
        print(f"Breakpoint {bp_num} at 0x{addr:04X}" +
              (f" ({arg})" if not arg.startswith('0x') else ''))

    def cmd_delete(self, arg):
        if not arg:
            print("Usage: delete <breakpoint_number>")
            return
        num = int(arg)
        for bp in self.breakpoints:
            if bp['num'] == num:
                self.rsp.send(f'z0,{bp["addr"]:x},1')
                self.breakpoints.remove(bp)
                print(f"Deleted breakpoint {num}")
                return
        print(f"No breakpoint {num}")

    def cmd_continue(self):
        self.rsp.send('c')
        self.show_location()

    def cmd_step(self):
        self.rsp.send('s')
        self.show_location()

    def cmd_stepi(self):
        self.rsp.send('s')
        self.show_location()

    def cmd_next(self):
        # Step over: single-step and if we land at same or deeper nesting, keep stepping
        # Simplified: just use 's' (no call detection for now)
        self.rsp.send('s')
        self.show_location()

    def cmd_info_registers(self):
        regs = parse_regs(self.rsp.send('g'))
        if not regs:
            print("Cannot read registers")
            return
        print(f"  PC   = 0x{regs['PC']:04X}")
        print(f"  ACC  = 0x{regs['ACC']:02X} ({regs['ACC']})")
        print(f"  B    = 0x{regs['B']:02X}")
        print(f"  PSW  = {decode_psw(regs['PSW'])}")
        print(f"  SP   = 0x{regs['SP']:02X}")
        print(f"  DPTR = 0x{regs['DPTR']:04X}")
        bank = (regs['PSW'] >> 3) & 3
        r_str = ' '.join(f"R{i}=0x{regs[f'R{i}']:02X}" for i in range(8))
        print(f"  R0-R7 (bank {bank}): {r_str}")

    def cmd_info_sfr(self):
        # Read SFR space via memory read at 0x0D0080-0x0D00FF
        resp = self.rsp.send('m0D0080,80')
        if len(resp) < 256:
            print("Cannot read SFR space")
            return
        print("  SFR registers:")
        for addr in sorted(SFR_NAMES.keys()):
            idx = (addr - 0x80) * 2
            val = int(resp[idx:idx+2], 16)
            name = SFR_NAMES[addr]
            extra = ''
            if addr == 0xD0:  # PSW
                extra = f'  {decode_psw(val)}'
            print(f"    {name:8s} (0x{addr:02X}) = 0x{val:02X}{extra}")

    def cmd_info_break(self):
        if not self.breakpoints:
            print("No breakpoints set")
            return
        for bp in self.breakpoints:
            print(f"  #{bp['num']}  0x{bp['addr']:04X}  {bp['name']}")

    def cmd_print(self, arg):
        if not arg:
            print("Usage: print <var|sfr_name|Rn>")
            return

        # Check SFR by name
        if arg.lower() in SFR_BY_NAME:
            addr = SFR_BY_NAME[arg.lower()]
            resp = self.rsp.send(f'm{0x0D0000 + addr:06x},1')
            val = int(resp[:2], 16) if len(resp) >= 2 else 0
            print(f"  {SFR_NAMES[addr]} (0x{addr:02X}) = 0x{val:02X} ({val})")
            return

        # Check register Rn
        rm = re.match(r'[Rr](\d)', arg)
        if rm:
            regs = parse_regs(self.rsp.send('g'))
            if regs:
                n = int(rm.group(1))
                print(f"  R{n} = 0x{regs[f'R{n}']:02X} ({regs[f'R{n}']})")
            return

        # Check named registers
        if arg.upper() in ('ACC', 'A', 'B', 'SP', 'PSW', 'PC', 'DPTR', 'DPL', 'DPH'):
            regs = parse_regs(self.rsp.send('g'))
            if regs:
                key = 'ACC' if arg.upper() == 'A' else arg.upper()
                if key in regs:
                    val = regs[key]
                    width = 4 if key in ('PC', 'DPTR') else 2
                    print(f"  {key} = 0x{val:0{width}X} ({val})")
            return

        # Check CDB symbols
        if arg in self.cdb.symbols:
            sym = self.cdb.symbols[arg]
            addr = sym.get('addr', 0)
            resp = self.rsp.send(f'm{0x0D0000 + addr:06x},1')
            val = int(resp[:2], 16) if len(resp) >= 2 else 0
            print(f"  {arg} (IRAM 0x{addr:02X}) = 0x{val:02X} ({val})")
            return

        print(f"Unknown symbol '{arg}'")

    def cmd_examine(self, arg):
        # x /Nx <addr>  — examine N bytes at address
        m = re.match(r'/(\d+)x\s+(0x[0-9a-fA-F]+|\d+)', arg) if arg else None
        if not m:
            print("Usage: x /Nx <addr>  (e.g., x /16x 0x00)")
            return
        count = int(m.group(1))
        addr = int(m.group(2), 0)
        # Determine memory space
        if addr < 0x100:
            mem_addr = 0x0D0000 + addr  # IRAM
            space = "IRAM"
        elif addr < 0x10000:
            mem_addr = addr  # Code
            space = "CODE"
        else:
            mem_addr = addr
            space = "XDATA" if addr >= 0x0F0000 else "CODE"

        resp = self.rsp.send(f'm{mem_addr:x},{count:x}')
        if not resp:
            print("Read error")
            return
        print(f"  {space} 0x{addr:04X}:", end='')
        for i in range(0, min(len(resp), count * 2), 2):
            if i > 0 and (i // 2) % 16 == 0:
                print(f"\n  {space} 0x{addr + i // 2:04X}:", end='')
            print(f" {resp[i:i+2]}", end='')
        print()

    def cmd_list(self, arg):
        if arg:
            if ':' in arg:
                parts = arg.split(':')
                self._show_source_context(parts[0], int(parts[1]), 5)
                return
        regs = parse_regs(self.rsp.send('g'))
        if regs:
            loc = self.cdb.get_source_line(regs['PC'])
            if loc:
                self._show_source_context(loc[0], loc[1], 8)
            else:
                print(f"  No source for PC=0x{regs['PC']:04X}")

    def cmd_backtrace(self):
        regs = parse_regs(self.rsp.send('g'))
        if not regs:
            return
        pc = regs['PC']
        sp = regs['SP']
        loc = self.cdb.get_source_line(pc)
        loc_str = f" at {loc[0]}:{loc[1]}" if loc else ''
        print(f"  #0  PC=0x{pc:04X}{loc_str}")

        # Walk stack: 8051 stack has 2-byte return addresses
        frame = 1
        while sp >= 0x08 and frame < 10:
            # Read 2 bytes from stack (PCH at SP, PCL at SP-1)
            resp = self.rsp.send(f'm{0x0D0000 + sp:06x},1')
            pch = int(resp[:2], 16) if len(resp) >= 2 else 0
            resp = self.rsp.send(f'm{0x0D0000 + sp - 1:06x},1')
            pcl = int(resp[:2], 16) if len(resp) >= 2 else 0
            ret_addr = (pch << 8) | pcl
            if ret_addr == 0:
                break
            loc = self.cdb.get_source_line(ret_addr)
            loc_str = f" at {loc[0]}:{loc[1]}" if loc else ''
            print(f"  #{frame}  PC=0x{ret_addr:04X}{loc_str}")
            sp -= 2
            frame += 1

    def cmd_reset(self):
        # Send custom reset via disconnect+reconnect — or just write PC=0
        self.rsp.send('G' + '00' * 16)  # Zero all registers including PC
        print("CPU reset")

    def run(self):
        self.show_location()
        while True:
            try:
                line = input('\033[96m(gdb8051)\033[0m ').strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not line:
                continue

            parts = line.split(None, 1)
            cmd = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ''

            if cmd in ('quit', 'q'):
                break
            elif cmd in ('continue', 'c'):
                self.cmd_continue()
            elif cmd == 'step':
                self.cmd_step()
            elif cmd == 'stepi':
                self.cmd_stepi()
            elif cmd == 'next' or cmd == 'n':
                self.cmd_next()
            elif cmd in ('break', 'b'):
                self.cmd_break(arg)
            elif cmd == 'delete':
                self.cmd_delete(arg)
            elif cmd == 'info':
                if arg.startswith('reg'):
                    self.cmd_info_registers()
                elif arg.startswith('sfr'):
                    self.cmd_info_sfr()
                elif arg.startswith('break') or arg.startswith('bp'):
                    self.cmd_info_break()
                else:
                    print("info: registers, sfr, break")
            elif cmd == 'print' or cmd == 'p':
                self.cmd_print(arg)
            elif cmd == 'x':
                self.cmd_examine(arg)
            elif cmd == 'list' or cmd == 'l':
                self.cmd_list(arg)
            elif cmd in ('backtrace', 'bt'):
                self.cmd_backtrace()
            elif cmd == 'reset':
                self.cmd_reset()
            elif cmd == 'detach':
                self.rsp.close()
                print("Detached")
                return
            elif cmd == 'set':
                # set ACC = 0xFF etc
                m = re.match(r'(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', arg)
                if m:
                    self.cmd_print(m.group(1))  # Show current value
                    # TODO: implement set
                    print("  (set not fully implemented yet)")
                else:
                    print("Usage: set <reg> = <value>")
            elif cmd == 'help' or cmd == 'h':
                print(__doc__)
            else:
                print(f"Unknown command: {cmd}. Type 'help' for commands.")

        try:
            self.rsp.close()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description='8051 debugger for ucvm')
    parser.add_argument('cdb', nargs='?', help='SDCC .cdb debug file')
    parser.add_argument('-p', '--port', type=int, default=1234, help='GDB TCP port')
    parser.add_argument('-H', '--host', default='127.0.0.1', help='Host')
    args = parser.parse_args()

    try:
        dbg = Debugger(args.host, args.port, args.cdb)
        dbg.run()
    except ConnectionRefusedError:
        print(f"Cannot connect to {args.host}:{args.port}")
        print("Start ucvm51 with -g option first:")
        print(f"  ./ucvm51 -g {args.port} firmware.ihx")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
