#!/usr/bin/env python3
"""
ucvm non-invasive debugging verification.

Proves that GDB debugging doesn't alter program behavior:
1. Reference run: break at final checkpoint, read all state
2. Breakpoint run: break at EVERY checkpoint, continue through each, read final state
3. Stepping run: single-step through parts of the code, read final state
4. Inspect run: read registers+memory at every stop, read final state
5. Compare all final states — they must match

Tests on both PC (localhost) and ESP32 (if reachable).
"""

import subprocess
import socket
import time
import sys
import os
import re
import tempfile
import select

UCVM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ucvm')
ELF  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_program.elf')
HEX  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_program.hex')

VERBOSE = '--verbose' in sys.argv or '-v' in sys.argv


class Colors:
    GREEN = '\033[92m'
    RED   = '\033[91m'
    CYAN  = '\033[96m'
    YELLOW = '\033[93m'
    RESET = '\033[0m'
    BOLD  = '\033[1m'


# Variables to compare for behavioral equivalence
STATE_VARS = [
    ('g_u8',           'print/x g_u8'),
    ('g_u16',          'print/x g_u16'),
    ('g_i8',           'print g_i8'),
    ('g_i16',          'print g_i16'),
    ('g_i32',          'print g_i32'),
    ('g_char',         'print g_char'),
    ('g_sensor.id',    'print g_sensor.id'),
    ('g_sensor.value', 'print g_sensor.value'),
    ('g_sensor.offset','print g_sensor.offset'),
    ('g_sensor.name',  'print g_sensor.name'),
    ('g_state',        'print g_state'),
    ('gdb_marker',     'print gdb_marker'),
    # Stack pointer and SREG should also match
    ('SP',             'print/x $SP'),
    ('SREG',           'print/x $SREG'),
]


def run_gdb_session(port, commands, host='127.0.0.1', timeout=30):
    """Run a GDB session and return output lines."""
    all_cmds = [
        'set confirm off',
        'set tcp connect-timeout 10',
        f'target remote {host}:{port}',
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


def extract_state(lines):
    """Extract all $N = ... values from GDB output into a dict."""
    values = []
    for line in lines:
        m = re.match(r'\$(\d+)\s*=\s*(.*)', line)
        if m:
            values.append(m.group(2).strip())
    return values


def start_ucvm(port):
    """Start ucvm with GDB stub, return process."""
    proc = subprocess.Popen(
        [UCVM, '-g', str(port), '-q', HEX],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
    )
    deadline = time.time() + 5
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stderr], [], [], 0.1)
        if ready:
            line = proc.stderr.readline()
            if 'waiting for GDB' in line:
                return proc
            if proc.poll() is not None:
                return None
    return proc


def stop_ucvm(proc):
    if proc:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def capture_final_state(port, pre_commands, host='127.0.0.1'):
    """Run GDB with pre_commands, then read all state variables at the end.
    pre_commands should leave the CPU stopped at checkpoint 10.
    Extracts only the LAST len(STATE_VARS) values to ignore intermediate prints."""
    state_cmds = [cmd for _, cmd in STATE_VARS]
    out = run_gdb_session(port, pre_commands + state_cmds, host=host)
    all_values = extract_state(out)
    # Take only the last N values (the state reads), skipping earlier prints
    n = len(STATE_VARS)
    values = all_values[-n:] if len(all_values) >= n else all_values
    state = {}
    for i, (name, _) in enumerate(STATE_VARS):
        state[name] = values[i] if i < len(values) else 'MISSING'
    return state


class NonInvasiveTest:
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

    def compare_states(self, label, ref, test, relaxed_keys=None):
        """Compare two state dicts. Return True if all match.
        relaxed_keys: set of key names where different values are acceptable
        (e.g. gdb_marker can be 10 or 255 depending on exact stop timing)."""
        if relaxed_keys is None:
            relaxed_keys = set()
        all_match = True
        relaxed_diffs = 0
        for name in ref:
            r = ref[name]
            t = test.get(name, 'MISSING')
            if r != t:
                if name in relaxed_keys:
                    relaxed_diffs += 1
                else:
                    self.check(f'{label}: {name}', False, f'ref={r!r} got={t!r}')
                    all_match = False
        if all_match:
            extra = f' ({relaxed_diffs} timing-dependent vars differ)' if relaxed_diffs else ''
            self.check(f'{label}: all {len(ref) - len(relaxed_keys)} program variables match{extra}', True)
        return all_match

    def run_pc_tests(self):
        """Run all non-invasive tests on PC (localhost)."""
        port = 4444

        print(f'\n{Colors.BOLD}=== PC Non-Invasive Debugging Test ==={Colors.RESET}')

        # ---- Run 1: Reference (minimal GDB interaction) ----
        print(f'\n{Colors.CYAN}[1] Reference run: break final, continue straight{Colors.RESET}')
        proc = start_ucvm(port)
        if not proc:
            self.check('Start ucvm', False, 'failed to start')
            return
        ref_state = capture_final_state(port, [
            'break output_byte',
            # Continue through all 10 checkpoints without stopping in between
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
        ])
        stop_ucvm(proc)
        self.check('Reference state captured',
                   ref_state.get('gdb_marker') not in ('MISSING', None),
                   f'marker={ref_state.get("gdb_marker")}')

        if VERBOSE:
            print(f'  Reference state:', file=sys.stderr)
            for k, v in ref_state.items():
                print(f'    {k} = {v}', file=sys.stderr)

        # ---- Run 2: Breakpoint at every checkpoint ----
        print(f'\n{Colors.CYAN}[2] Breakpoint run: stop at every checkpoint{Colors.RESET}')
        proc = start_ucvm(port)
        bp_state = capture_final_state(port, [
            'break output_byte',
            'continue', 'print gdb_marker',  # cp1
            'continue', 'print gdb_marker',  # cp2
            'continue', 'print gdb_marker',  # cp3
            'continue', 'print gdb_marker',  # cp4
            'continue', 'print gdb_marker',  # cp5
            'continue', 'print gdb_marker',  # cp6
            'continue', 'print gdb_marker',  # cp7
            'continue', 'print gdb_marker',  # cp8
            'continue', 'print gdb_marker',  # cp9
            'continue', 'print gdb_marker',  # cp10
        ])
        stop_ucvm(proc)
        self.compare_states('Breakpoint run vs reference', ref_state, bp_state)

        # ---- Run 3: Heavy inspection at each stop ----
        print(f'\n{Colors.CYAN}[3] Inspect run: read registers+memory at every stop{Colors.RESET}')
        proc = start_ucvm(port)
        inspect_cmds = ['break output_byte']
        for _ in range(10):
            inspect_cmds += [
                'continue',
                'info reg',                    # read all registers
                'x/32xb 0x800100',             # read SRAM
                'print g_sensor',              # read struct
                'print/x $SP',                 # read SP
            ]
        insp_state = capture_final_state(port, inspect_cmds)
        stop_ucvm(proc)
        self.compare_states('Inspect run vs reference', ref_state, insp_state)

        # ---- Run 4: Single-stepping through init ----
        print(f'\n{Colors.CYAN}[4] Step run: stepi through startup, then continue{Colors.RESET}')
        proc = start_ucvm(port)
        step_cmds = [
            'break main',
            'continue',
            # Single-step 20 instructions inside main
            'stepi', 'stepi', 'stepi', 'stepi', 'stepi',
            'stepi', 'stepi', 'stepi', 'stepi', 'stepi',
            'stepi', 'stepi', 'stepi', 'stepi', 'stepi',
            'stepi', 'stepi', 'stepi', 'stepi', 'stepi',
            # Delete main BP, set output_byte BP
            'delete 1',
            'break output_byte',
            # Continue through all checkpoints
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
        ]
        step_state = capture_final_state(port, step_cmds)
        stop_ucvm(proc)
        self.compare_states('Step run vs reference', ref_state, step_state)

        # ---- Run 5: Memory write then undo — verify no side effects ----
        print(f'\n{Colors.CYAN}[5] Write-undo: modify variable, restore, verify final state{Colors.RESET}')
        proc = start_ucvm(port)
        undo_cmds = [
            'break output_byte',
            'continue',  # cp1
            # Modify g_u8 then restore
            'set g_u8 = 0xFF',
            'set g_u8 = 0x42',
            # Continue to completion
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue',
        ]
        undo_state = capture_final_state(port, undo_cmds)
        stop_ucvm(proc)
        self.compare_states('Write-undo run vs reference', ref_state, undo_state)

    def run_esp32_tests(self, host, port):
        """Run non-invasive tests against live ESP32."""
        print(f'\n{Colors.BOLD}=== ESP32 Non-Invasive Debugging Test ==={Colors.RESET}')
        print(f'Target: {host}:{port}')

        # ---- Reference run ----
        print(f'\n{Colors.CYAN}[1] Reference run{Colors.RESET}')
        ref_state = capture_final_state(port, [
            'break output_byte',
            'continue', 'continue', 'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue', 'continue',
        ], host=host)
        time.sleep(1.0)
        self.check('Reference state captured',
                   ref_state.get('gdb_marker') not in ('MISSING', None),
                   f'marker={ref_state.get("gdb_marker")}')

        # ---- Breakpoint run ----
        print(f'\n{Colors.CYAN}[2] Breakpoint run{Colors.RESET}')
        bp_state = capture_final_state(port, [
            'break output_byte',
            'continue', 'print gdb_marker',
            'continue', 'print gdb_marker',
            'continue', 'print gdb_marker',
            'continue', 'continue', 'continue',
            'continue', 'continue', 'continue', 'continue',
        ], host=host)
        time.sleep(1.0)
        self.compare_states('ESP32 breakpoint run vs reference', ref_state, bp_state,
                            relaxed_keys={'gdb_marker'})

        # ---- Inspect run ----
        print(f'\n{Colors.CYAN}[3] Inspect run{Colors.RESET}')
        insp_cmds = ['break output_byte']
        for _ in range(12):  # extra continues for WiFi races
            insp_cmds += ['continue', 'info reg', 'print g_sensor']
        insp_state = capture_final_state(port, insp_cmds, host=host)
        time.sleep(1.0)
        self.compare_states('ESP32 inspect run vs reference', ref_state, insp_state,
                            relaxed_keys={'gdb_marker'})


def main():
    runner = NonInvasiveTest()

    # PC tests
    runner.run_pc_tests()

    # ESP32 tests (if reachable)
    esp_host = os.environ.get('UCVM_HOST', '192.168.0.2')
    esp_port = int(os.environ.get('UCVM_PORT', '1234'))
    try:
        s = socket.create_connection((esp_host, esp_port), timeout=2)
        s.close()
        time.sleep(0.5)
        runner.run_esp32_tests(esp_host, esp_port)
    except (ConnectionRefusedError, OSError, TimeoutError):
        print(f'\n{Colors.YELLOW}ESP32 not reachable at {esp_host}:{esp_port} — skipping{Colors.RESET}')

    # Summary
    total = runner.passed + runner.failed
    print(f'\n{Colors.BOLD}Results: {runner.passed}/{total} passed{Colors.RESET}')
    if runner.errors:
        print(f'{Colors.RED}Failures:{Colors.RESET}')
        for e in runner.errors:
            print(f'  - {e}')
    return runner.failed == 0


if __name__ == '__main__':
    sys.exit(0 if main() else 1)
