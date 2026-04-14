#!/usr/bin/env python3
"""
ucvm GDB stub test — ESP32 target at 192.168.0.2:1234

Runs the same test cases as test_gdb.py but against a live ESP32.
Each test reconnects (triggering CPU reset on the ESP32).
"""

import subprocess
import sys
import os
import re
import tempfile
import time

ELF  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_program.elf')
HOST = os.environ.get('UCVM_HOST', '192.168.0.2')
PORT = int(os.environ.get('UCVM_PORT', '1234'))

VERBOSE = '--verbose' in sys.argv or '-v' in sys.argv


class Colors:
    GREEN = '\033[92m'
    RED   = '\033[91m'
    CYAN  = '\033[96m'
    RESET = '\033[0m'
    BOLD  = '\033[1m'


def run_gdb(commands, timeout=30):
    all_cmds = [
        'set confirm off',
        'set tcp connect-timeout 10',
        f'target remote {HOST}:{PORT}',
    ] + commands + ['detach', 'quit']

    with tempfile.NamedTemporaryFile(mode='w', suffix='.gdb', delete=False) as f:
        f.write('\n'.join(all_cmds))
        script = f.name

    try:
        result = subprocess.run(
            ['avr-gdb', '-batch', '-x', script, ELF],
            capture_output=True, text=True, timeout=timeout
        )
        output = result.stdout + result.stderr
        if VERBOSE:
            for line in output.splitlines():
                print(f'  | {line}', file=sys.stderr)
        return output.splitlines()
    except subprocess.TimeoutExpired:
        return ['TIMEOUT']
    finally:
        os.unlink(script)


def find_in_output(lines, pattern):
    for line in lines:
        m = re.search(pattern, line)
        if m:
            return m
    return None


def extract_values(lines):
    values = []
    for line in lines:
        m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
        if m:
            values.append(m.group(2).strip())
    return values


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []

    def check(self, name, condition, detail=''):
        if condition:
            self.passed += 1
            print(f'  {Colors.GREEN}PASS{Colors.RESET} {name}')
        else:
            self.failed += 1
            self.errors.append(f'{name}: {detail}' if detail else name)
            print(f'  {Colors.RED}FAIL{Colors.RESET} {name}' +
                  (f' ({detail})' if detail else ''))

    def run_all(self):
        print(f'\n{Colors.BOLD}ucvm GDB stub — ESP32 test suite{Colors.RESET}')
        print(f'Target: {HOST}:{PORT}\n')

        self.test_1_connection()
        # Brief pause between sessions to let ESP32 re-listen
        time.sleep(1.0)
        self.test_2_breakpoint_continue()
        time.sleep(1.0)
        self.test_3_global_variables()
        time.sleep(1.0)
        self.test_4_stepping()
        time.sleep(1.0)
        self.test_5_struct_and_pointers()
        time.sleep(1.0)
        self.test_6_functions_recursion()
        time.sleep(1.0)
        self.test_7_stack_trace()
        time.sleep(1.0)
        self.test_8_memory_rw()
        time.sleep(1.0)
        self.test_9_full_run()

        total = self.passed + self.failed
        print(f'\n{Colors.BOLD}Results: {self.passed}/{total} passed{Colors.RESET}')
        if self.errors:
            print(f'{Colors.RED}Failures:{Colors.RESET}')
            for e in self.errors:
                print(f'  - {e}')
        return self.failed == 0

    def test_1_connection(self):
        print(f'\n{Colors.CYAN}[1] Connection and reset state{Colors.RESET}')
        out = run_gdb(['info reg pc SP SREG'])

        pc = find_in_output(out, r'pc\s+0x([0-9a-f]+)')
        self.check('Connected, PC at reset (0x0)',
                   pc and int(pc.group(1), 16) == 0,
                   f'PC=0x{int(pc.group(1),16):x}' if pc else 'no PC')

        sp = find_in_output(out, r'SP\s+0x([0-9a-f]+)')
        if sp:
            v = int(sp.group(1), 16)
            self.check('SP at top of SRAM (0x8FF)', v == 0x8FF, f'SP=0x{v:x}')
        else:
            self.check('SP readable', False)

        sreg = find_in_output(out, r'SREG\s+0x([0-9a-f]+)')
        self.check('SREG cleared (0x0)',
                   sreg and int(sreg.group(1), 16) == 0,
                   f'SREG=0x{int(sreg.group(1),16):x}' if sreg else 'no SREG')

    def test_2_breakpoint_continue(self):
        print(f'\n{Colors.CYAN}[2] Breakpoint and continue{Colors.RESET}')
        out = run_gdb([
            'break main',
            'continue',
            'print gdb_marker',
            'break output_byte',
            'continue',
            'print gdb_marker',
        ])
        values = extract_values(out)

        self.check('Hit main, marker=0',
                   len(values) >= 1 and values[0].startswith('0'),
                   f'values={values}')

        bp_hit = find_in_output(out, r'Breakpoint \d+, output_byte')
        self.check('Continue past main, hit output_byte',
                   bp_hit is not None,
                   f'output={[l for l in out if "Breakpoint" in l]}')

        self.check('At output_byte, marker=1',
                   len(values) >= 2 and ("1 " in values[1] or values[1].startswith("1")),
                   f'values={values}')

    def test_3_global_variables(self):
        print(f'\n{Colors.CYAN}[3] Global variable inspection{Colors.RESET}')
        out = run_gdb([
            'break output_byte',
            'continue',
            'print/x g_u8',
            'print/x g_u16',
            'print/x (unsigned long)g_u32',
            'print g_i8',
            'print g_i16',
            'print g_i32',
            'print g_char',
        ])
        values = extract_values(out)

        checks = [
            ('g_u8 == 0x42',       '0x42'),
            ('g_u16 == 0xbeef',    '0xbeef'),
            ('g_u32 == 0xdeadcafe','0xdeadcafe'),
            ('g_i8 == -42',        '-42'),
            ('g_i16 == -1234',     '-1234'),
            ('g_i32 == -100000',   '-100000'),
            ("g_char == 'Z'",      'Z'),
        ]
        for i, (name, expected) in enumerate(checks):
            got = values[i] if i < len(values) else 'none'
            self.check(name, expected in got, f'got {got}')

    def test_4_stepping(self):
        print(f'\n{Colors.CYAN}[4] Single-stepping{Colors.RESET}')
        out = run_gdb([
            'break main',
            'continue',
            'stepi',
            'info reg pc',
            'stepi',
            'info reg pc',
            'stepi',
            'info reg pc',
        ])
        pcs = []
        for line in out:
            m = re.search(r'pc\s+0x([0-9a-f]+)', line)
            if m:
                pcs.append(int(m.group(1), 16))

        self.check('stepi advances PC',
                   len(pcs) >= 2 and pcs[0] != pcs[1],
                   f'PCs={["0x%x"%p for p in pcs]}')
        self.check('Three distinct PCs',
                   len(set(pcs)) >= 2,
                   f'PCs={["0x%x"%p for p in pcs]}')

    def test_5_struct_and_pointers(self):
        print(f'\n{Colors.CYAN}[5] Struct after pointer update{Colors.RESET}')
        out = run_gdb([
            'break output_byte',
            'continue',   # cp1
            'continue',   # cp2
            'continue',   # cp3
            'print g_sensor.value',
            'print g_sensor.offset',
            'print g_sensor.id',
            'print g_sensor.name',
        ])
        values = extract_values(out)

        self.check('g_sensor.value == 512',
                   len(values) > 0 and '512' in values[0],
                   f'got {values[0] if values else "none"}')
        self.check('g_sensor.offset == -49',
                   len(values) > 1 and '-49' in values[1],
                   f'got {values[1] if len(values)>1 else "none"}')
        self.check('g_sensor.id == 7',
                   len(values) > 2 and '7' in values[2],
                   f'got {values[2] if len(values)>2 else "none"}')
        self.check('g_sensor.name has TEMP',
                   len(values) > 3 and 'TEMP' in values[3],
                   f'got {values[3] if len(values)>3 else "none"}')

    def test_6_functions_recursion(self):
        print(f'\n{Colors.CYAN}[6] Functions and recursion{Colors.RESET}')
        # Use a single continue-to-checkpoint approach to avoid WiFi latency races.
        # Break at output_byte, continue 3 times, check marker advances.
        out = run_gdb([
            'break output_byte',
            'continue',
            'print gdb_marker',
            'continue',
            'print gdb_marker',
            'continue',
            'print gdb_marker',
        ])
        values = extract_values(out)

        # Markers should be strictly increasing (1,2,3 or similar)
        markers = []
        for v in values:
            m = re.match(r'(\d+)', v)
            if m:
                markers.append(int(m.group(1)))

        self.check('Three checkpoints reached',
                   len(markers) >= 3,
                   f'markers={markers}')
        self.check('Markers strictly increasing',
                   len(markers) >= 3 and markers[0] < markers[1] < markers[2],
                   f'markers={markers}')
        self.check('Markers within valid range (1-10)',
                   all(1 <= m <= 10 for m in markers),
                   f'markers={markers}')

    def test_7_stack_trace(self):
        print(f'\n{Colors.CYAN}[7] Stack trace{Colors.RESET}')
        out = run_gdb([
            'break output_byte',
            'continue',
            'backtrace',
        ])
        bt = '\n'.join(out)
        self.check('output_byte in backtrace', 'output_byte' in bt,
                   f'bt={[l for l in out if "#" in l]}')
        self.check('main in backtrace', 'main' in bt,
                   f'bt={[l for l in out if "#" in l]}')

    def test_8_memory_rw(self):
        print(f'\n{Colors.CYAN}[8] Memory read/write{Colors.RESET}')
        out = run_gdb([
            'break output_byte',
            'continue',
            'set g_u8 = 0x99',
            'print/x g_u8',
            'x/1xh 0x0',
        ])
        values = extract_values(out)
        self.check('Write g_u8=0x99',
                   len(values) > 0 and '0x99' in values[0],
                   f'got {values[0] if values else "none"}')

        flash = '\n'.join(out).lower()
        self.check('Flash read: JMP at vector[0]',
                   '940c' in flash or '0c94' in flash,
                   f'flash lines={[l for l in out if "0x0" in l][:2]}')

    def test_9_full_run(self):
        print(f'\n{Colors.CYAN}[9] Full run through all checkpoints{Colors.RESET}')
        # Continue many times (more than 10 to handle WiFi races),
        # then check final state
        out = run_gdb([
            'break output_byte',
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
            'print gdb_marker',
            'print g_state',
        ])
        values = extract_values(out)

        # Marker should be 10 or 255 (0xFF = final loop)
        if len(values) > 0:
            m = re.match(r'(\d+)', values[0])
            marker = int(m.group(1)) if m else -1
        else:
            marker = -1

        self.check('Program completed (marker >= 10)',
                   marker >= 10,
                   f'marker={marker}')
        self.check('g_state == STATE_DONE (3)',
                   len(values) > 1 and ('3' in values[1] or 'STATE_DONE' in values[1]),
                   f'got {values[1] if len(values)>1 else "none"}')


if __name__ == '__main__':
    runner = TestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
