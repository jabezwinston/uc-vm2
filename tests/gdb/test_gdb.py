#!/usr/bin/env python3
"""
ucvm GDB stub test suite.

Launches ucvm with -g, drives avr-gdb via command scripts,
verifies: connection, registers, memory, breakpoints, stepping,
variable inspection, function calls, stack traces.

Usage: python3 test_gdb.py [--verbose]
"""

import subprocess
import socket
import time
import sys
import os
import re
import tempfile

UCVM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ucvm')
ELF  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_program.elf')
HEX  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_program.hex')
PORT = 4321

VERBOSE = '--verbose' in sys.argv or '-v' in sys.argv


class Colors:
    GREEN = '\033[92m'
    RED   = '\033[91m'
    CYAN  = '\033[96m'
    RESET = '\033[0m'
    BOLD  = '\033[1m'


def run_gdb(commands, timeout=30):
    """Run avr-gdb in batch mode with given commands. Returns stdout lines."""
    all_cmds = ['set confirm off', f'target remote :{PORT}'] + commands + ['detach', 'quit']
    cmd_str = '\n'.join(all_cmds)

    with tempfile.NamedTemporaryFile(mode='w', suffix='.gdb', delete=False) as f:
        f.write(cmd_str)
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
    """Search output lines for a regex pattern. Returns first match or None."""
    for line in lines:
        m = re.search(pattern, line)
        if m:
            return m
    return None


def extract_value(lines, prefix):
    """Extract a GDB print result: $N = value"""
    for line in lines:
        m = re.match(r'\$\d+\s*=\s*(.*)', line)
        if m:
            return m.group(1).strip()
    return None


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []
        self.ucvm_proc = None

    def start_ucvm(self):
        self.ucvm_proc = subprocess.Popen(
            [UCVM, '-g', str(PORT), '-q', HEX],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        # Wait for "waiting for GDB connection" on stderr
        import select
        deadline = time.time() + 5
        while time.time() < deadline:
            ready, _, _ = select.select([self.ucvm_proc.stderr], [], [], 0.1)
            if ready:
                line = self.ucvm_proc.stderr.readline()
                if VERBOSE:
                    print(f'  ucvm: {line.rstrip()}', file=sys.stderr)
                if 'waiting for GDB' in line:
                    return True
                if self.ucvm_proc.poll() is not None:
                    return False
        return False

    def restart_ucvm(self):
        """Restart ucvm for a fresh test session (GDB stub is single-use)."""
        self.stop_ucvm()
        return self.start_ucvm()

    def stop_ucvm(self):
        if self.ucvm_proc:
            self.ucvm_proc.terminate()
            try:
                self.ucvm_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.ucvm_proc.kill()
            self.ucvm_proc = None

    def check(self, name, condition, detail=''):
        if condition:
            self.passed += 1
            print(f'  {Colors.GREEN}PASS{Colors.RESET} {name}')
        else:
            self.failed += 1
            msg = f'{name}: {detail}' if detail else name
            self.errors.append(msg)
            print(f'  {Colors.RED}FAIL{Colors.RESET} {name}' +
                  (f' ({detail})' if detail else ''))

    def run_all(self):
        print(f'\n{Colors.BOLD}ucvm GDB stub test suite{Colors.RESET}\n')

        if not os.path.exists(ELF) or not os.path.exists(HEX):
            print(f'{Colors.RED}ERROR: test binaries not found. Run make in tests/gdb/{Colors.RESET}')
            return False

        self.test_1_connection()
        self.test_2_registers()
        self.test_3_breakpoint_continue()
        self.test_4_global_variables()
        self.test_5_stepping()
        self.test_6_struct_and_pointers()
        self.test_7_functions_and_recursion()
        self.test_8_stack_trace()
        self.test_9_memory_rw()
        self.test_10_full_run()

        # Summary
        total = self.passed + self.failed
        print(f'\n{Colors.BOLD}Results: {self.passed}/{total} passed{Colors.RESET}')
        if self.errors:
            print(f'{Colors.RED}Failures:{Colors.RESET}')
            for e in self.errors:
                print(f'  - {e}')
        return self.failed == 0

    # ---- Tests ----

    def test_1_connection(self):
        print(f'\n{Colors.CYAN}[1] Connection and initial state{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb(['info reg pc', 'info reg SP'])
        self.stop_ucvm()

        # Should connect and show PC near 0
        pc_match = find_in_output(out, r'pc\s+0x([0-9a-f]+)')
        self.check('GDB connects and reads PC',
                   pc_match is not None,
                   f'output={[l for l in out if "pc" in l.lower()][:2]}')

        sp_match = find_in_output(out, r'SP\s+0x([0-9a-f]+)')
        if sp_match:
            sp_val = int(sp_match.group(1), 16)
            self.check('SP near top of SRAM',
                       sp_val >= 0x800 and sp_val <= 0x8FF,
                       f'SP=0x{sp_val:04X}')
        else:
            self.check('SP readable', False, 'SP not found in output')

    def test_2_registers(self):
        print(f'\n{Colors.CYAN}[2] Register read/write{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'set $r24 = 0xAB',
            'set $r25 = 0xCD',
            'print/x $r24',
            'print/x $r25',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$\d+\s*=\s*(.*)', line)
            if m:
                values.append(m.group(1).strip())

        self.check('Register write r24=0xAB',
                   len(values) > 0 and '0xab' in values[0],
                   f'got {values[0] if values else "none"}')
        self.check('Register write r25=0xCD',
                   len(values) > 1 and '0xcd' in values[1],
                   f'got {values[1] if len(values) > 1 else "none"}')

    def test_3_breakpoint_continue(self):
        print(f'\n{Colors.CYAN}[3] Breakpoint and continue{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break main',
            'continue',
            'print gdb_marker',
            'break output_byte',
            'continue',
            'print gdb_marker',
        ])
        self.stop_ucvm()

        # First print (at main entry) — marker should be 0
        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        self.check('Break at main(), marker=0',
                   len(values) >= 1 and values[0].startswith('0'),
                   f'values={values}')

        self.check('Break at output_byte, marker=1',
                   len(values) >= 2 and "'\\001'" in values[1] or (len(values) >= 2 and values[1].startswith('1')),
                   f'values={values}')

        # Check breakpoint hit message
        bp_hit = find_in_output(out, r'Breakpoint \d+, (main|output_byte)')
        self.check('Breakpoint hit reported', bp_hit is not None)

    def test_4_global_variables(self):
        print(f'\n{Colors.CYAN}[4] Global variable inspection{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break output_byte',
            'continue',   # stop at checkpoint 1
            'print/x g_u8',
            'print/x g_u16',
            'print/x (unsigned long)g_u32',
            'print g_i8',
            'print g_i16',
            'print g_i32',
            'print g_char',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        if VERBOSE:
            print(f'  values={values}', file=sys.stderr)

        self.check('g_u8 == 0x42',
                   len(values) > 0 and '0x42' in values[0],
                   f'got {values[0] if values else "none"}')

        self.check('g_u16 == 0xbeef',
                   len(values) > 1 and '0xbeef' in values[1],
                   f'got {values[1] if len(values) > 1 else "none"}')

        self.check('g_u32 == 0xdeadcafe',
                   len(values) > 2 and '0xdeadcafe' in values[2],
                   f'got {values[2] if len(values) > 2 else "none"}')

        self.check('g_i8 == -42',
                   len(values) > 3 and '-42' in values[3],
                   f'got {values[3] if len(values) > 3 else "none"}')

        self.check('g_i16 == -1234',
                   len(values) > 4 and '-1234' in values[4],
                   f'got {values[4] if len(values) > 4 else "none"}')

        self.check('g_i32 == -100000',
                   len(values) > 5 and '-100000' in values[5],
                   f'got {values[5] if len(values) > 5 else "none"}')

        self.check("g_char == 'Z'",
                   len(values) > 6 and ('Z' in values[6] or '90' in values[6]),
                   f'got {values[6] if len(values) > 6 else "none"}')

    def test_5_stepping(self):
        print(f'\n{Colors.CYAN}[5] Single-stepping{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break main',
            'continue',
            'stepi',
            'info reg pc',
            'stepi',
            'info reg pc',
            'stepi',
            'info reg pc',
            'next',
            'info reg pc',
        ])
        self.stop_ucvm()

        # Extract PC values after each step
        pcs = []
        for line in out:
            m = re.search(r'pc\s+0x([0-9a-f]+)', line)
            if m:
                pcs.append(int(m.group(1), 16))

        self.check('stepi advances PC', len(pcs) >= 3 and pcs[0] != pcs[1],
                   f'PCs={["0x%x" % p for p in pcs]}')

        self.check('Multiple stepi shows progression',
                   len(pcs) >= 3 and len(set(pcs)) >= 2,
                   f'PCs={["0x%x" % p for p in pcs]}')

    def test_6_struct_and_pointers(self):
        print(f'\n{Colors.CYAN}[6] Struct and pointer operations{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break output_byte',
            'continue',  # checkpoint 1
            'continue',  # checkpoint 2
            'continue',  # checkpoint 3
            'print g_sensor.value',
            'print g_sensor.offset',
            'print g_sensor.id',
            'print g_sensor.name',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        self.check('g_sensor.value == 512 after update',
                   len(values) > 0 and '512' in values[0],
                   f'got {values[0] if values else "none"}')

        self.check('g_sensor.offset == -49',
                   len(values) > 1 and '-49' in values[1],
                   f'got {values[1] if len(values) > 1 else "none"}')

        self.check('g_sensor.id == 7',
                   len(values) > 2 and ('7' in values[2]),
                   f'got {values[2] if len(values) > 2 else "none"}')

        self.check('g_sensor.name contains TEMP',
                   len(values) > 3 and 'TEMP' in values[3],
                   f'got {values[3] if len(values) > 3 else "none"}')

    def test_7_functions_and_recursion(self):
        print(f'\n{Colors.CYAN}[7] Function calls and recursion{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break output_byte',
            'continue',  # cp 1
            'continue',  # cp 2
            'continue',  # cp 3
            'continue',  # cp 4
            'continue',  # cp 5 (functions)
            'print gdb_marker',
            'continue',  # cp 6 (recursion)
            'print gdb_marker',
            'continue',  # cp 7 (fn pointers)
            'print gdb_marker',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        self.check('Checkpoint 5 (functions) reached',
                   len(values) > 0 and '5' in values[0],
                   f'got {values[0] if values else "none"}')

        self.check('Checkpoint 6 (recursion) reached',
                   len(values) > 1 and '6' in values[1],
                   f'got {values[1] if len(values) > 1 else "none"}')

        self.check('Checkpoint 7 (fn pointers) reached',
                   len(values) > 2 and '7' in values[2],
                   f'got {values[2] if len(values) > 2 else "none"}')

    def test_8_stack_trace(self):
        print(f'\n{Colors.CYAN}[8] Stack trace{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break output_byte',
            'continue',
            'backtrace',
        ])
        self.stop_ucvm()

        bt_text = '\n'.join(out)
        self.check('output_byte in backtrace', 'output_byte' in bt_text,
                   f'bt={[l for l in out if "#" in l]}')
        self.check('main in backtrace', 'main' in bt_text,
                   f'bt={[l for l in out if "#" in l]}')

    def test_9_memory_rw(self):
        print(f'\n{Colors.CYAN}[9] Memory read/write{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break main',
            'continue',
            # Write to a global and read back
            'set g_u8 = 0x99',
            'print/x g_u8',
            # Read flash: first instruction should be JMP (0x940C)
            'x/1xh 0x0',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        self.check('Write g_u8=0x99 and read back',
                   len(values) > 0 and '0x99' in values[0],
                   f'got {values[0] if values else "none"}')

        flash_text = '\n'.join(out).lower()
        self.check('Flash read: first word is JMP (0x940c)',
                   '940c' in flash_text or '0c94' in flash_text,
                   f'flash={[l for l in out if "0x0" in l][:2]}')

    def test_10_full_run(self):
        print(f'\n{Colors.CYAN}[10] Full program run through all checkpoints{Colors.RESET}')
        self.start_ucvm()
        out = run_gdb([
            'break output_byte',
            # Run through all 10 checkpoints
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
            'print gdb_marker',
            'print g_state',
        ])
        self.stop_ucvm()

        values = []
        for line in out:
            m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
            if m:
                values.append(m.group(2).strip())

        self.check('All 10 checkpoints completed (marker=10)',
                   len(values) > 0 and '10' in values[0],
                   f'got {values[0] if values else "none"}')

        self.check('g_state == STATE_DONE (3)',
                   len(values) > 1 and ('3' in values[1] or 'STATE_DONE' in values[1]),
                   f'got {values[1] if len(values) > 1 else "none"}')


if __name__ == '__main__':
    runner = TestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
