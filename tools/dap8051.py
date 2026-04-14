#!/usr/bin/env python3
"""
DAP (Debug Adapter Protocol) server for 8051 on ucvm.

Speaks DAP over stdin/stdout so VS Code can use it as a debug adapter.
Connects to ucvm's GDB RSP stub and parses SDCC .cdb debug files for
source-level debugging with full VS Code integration: breakpoints,
stepping, variable inspection, register/SFR views.

Usage (called by VS Code, not directly):
    python3 dap8051.py
"""

import sys
import os
import json
import re
import socket
import threading

# Import CdbParser and RspClient from gdb8051
sys.path.insert(0, os.path.dirname(__file__))
from gdb8051 import CdbParser, RspClient, SFR_NAMES, SFR_BY_NAME, parse_regs, decode_psw


# ---------- DAP Protocol I/O ----------

def read_dap_message():
    """Read a DAP message from stdin (Content-Length header + JSON body)."""
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode('utf-8').strip()
        if line == '':
            break
        if ':' in line:
            key, val = line.split(':', 1)
            headers[key.strip()] = val.strip()
    length = int(headers.get('Content-Length', 0))
    if length == 0:
        return None
    body = sys.stdin.buffer.read(length)
    return json.loads(body.decode('utf-8'))


def send_dap_message(msg):
    """Send a DAP message to stdout."""
    body = json.dumps(msg, separators=(',', ':'))
    header = f'Content-Length: {len(body)}\r\n\r\n'
    sys.stdout.buffer.write(header.encode('utf-8'))
    sys.stdout.buffer.write(body.encode('utf-8'))
    sys.stdout.buffer.flush()


def send_response(request, success=True, body=None, message=None):
    resp = {
        'seq': 0,
        'type': 'response',
        'request_seq': request['seq'],
        'command': request['command'],
        'success': success,
    }
    if body:
        resp['body'] = body
    if message:
        resp['message'] = message
    send_dap_message(resp)


def send_event(event, body=None):
    evt = {
        'seq': 0,
        'type': 'event',
        'event': event,
    }
    if body:
        evt['body'] = body
    send_dap_message(evt)


# ---------- 8051 DAP Adapter ----------

class Dap8051Adapter:
    def __init__(self):
        self.rsp = None
        self.cdb = CdbParser()
        self.breakpoints = {}   # file → [(line, bp_id)]
        self.bp_id_counter = 1
        self.source_dirs = ['.']
        self.stopped = True
        self._seq = 1

    def _next_seq(self):
        s = self._seq
        self._seq += 1
        return s

    def connect(self, host, port):
        self.rsp = RspClient(host, port)

    def get_regs(self):
        resp = self.rsp.send('g')
        return parse_regs(resp)

    def read_mem(self, addr, length):
        """Read memory bytes. addr encoding: <0x10000=code, 0xD0000=iram, 0xF0000=xdata"""
        resp = self.rsp.send(f'm{addr:x},{length:x}')
        if resp:
            return bytes(int(resp[i:i+2], 16) for i in range(0, len(resp), 2))
        return b''

    def read_sfr(self, addr):
        data = self.read_mem(0x0D0000 + addr, 1)
        return data[0] if data else 0

    def set_breakpoint(self, addr):
        self.rsp.send(f'Z0,{addr:x},0')

    def clear_breakpoint(self, addr):
        self.rsp.send(f'z0,{addr:x},0')

    def step(self):
        self.rsp.send('s')
        self.stopped = True

    def continue_exec(self):
        self.stopped = False
        resp = self.rsp.send('c')
        self.stopped = True
        return resp

    # ---------- DAP request handlers ----------

    def handle_initialize(self, req):
        send_response(req, body={
            'supportsConfigurationDoneRequest': True,
            'supportsFunctionBreakpoints': True,
            'supportsEvaluateForHovers': True,
            'supportsSetVariable': True,
        })
        send_event('initialized')

    def handle_launch(self, req):
        args = req.get('arguments', {})
        host = args.get('host', 'localhost')
        port = args.get('port', 1234)
        cdb_file = args.get('cdbFile', '')
        self.source_dirs = args.get('sourceDirs', ['.'])

        if cdb_file:
            self.cdb = CdbParser(cdb_file)
            # Auto-add CDB directory to source search path
            cdb_dir = os.path.dirname(os.path.abspath(cdb_file))
            if cdb_dir not in self.source_dirs:
                self.source_dirs.append(cdb_dir)

        try:
            self.connect(host, port)
        except Exception as e:
            send_response(req, success=False, message=f'Connection failed: {e}')
            return

        send_response(req)
        send_event('stopped', {'reason': 'entry', 'threadId': 1, 'allThreadsStopped': True})

    def handle_disconnect(self, req):
        if self.rsp:
            self.rsp.close()
            self.rsp = None
        send_response(req)

    def handle_threads(self, req):
        send_response(req, body={'threads': [{'id': 1, 'name': '8051 Main'}]})

    def _make_frame(self, frame_id, pc):
        """Build a DAP stack frame dict for a given PC address."""
        func_name = self.cdb.get_func_at_addr(pc) or f'0x{pc:04X}'
        src = self.cdb.get_source_line(pc)
        frame = {
            'id': frame_id,
            'name': func_name,
            'instructionPointerReference': f'0x{pc:04X}',
            'line': 0,
            'column': 1,
        }
        if src:
            fname, lineno = src
            frame['line'] = lineno
            source_path = self._find_source(fname)
            if source_path:
                frame['source'] = {'name': fname, 'path': source_path}
        return frame

    def handle_stackTrace(self, req):
        regs = self.get_regs()
        if not regs:
            send_response(req, body={'stackFrames': [], 'totalFrames': 0})
            return

        pc = regs['PC']
        sp = regs['SP']
        frames = [self._make_frame(0, pc)]

        # Walk 8051 stack: grows upward from initial SP=0x07.
        # Each CALL pushes PCL then PCH (SP increments twice).
        # Return addresses are at [SP], [SP-1] for most recent call, etc.
        stack_top = sp
        for i in range(10):
            if stack_top < 2:
                break
            # Read return address: PCH at stack_top, PCL at stack_top-1
            data = self.read_mem(0x0D0000 + stack_top - 1, 2)
            if len(data) < 2:
                break
            ret_pc = (data[1] << 8) | data[0]  # PCH:PCL
            if ret_pc == 0 or ret_pc >= 0xFFFF:
                break
            frames.append(self._make_frame(i + 1, ret_pc))
            stack_top -= 2

        send_response(req, body={'stackFrames': frames, 'totalFrames': len(frames)})

    def handle_scopes(self, req):
        scopes = [
            {'name': 'Locals', 'variablesReference': 4, 'expensive': False},
            {'name': 'Globals', 'variablesReference': 5, 'expensive': False},
            {'name': 'Registers', 'variablesReference': 1, 'expensive': False},
            {'name': 'SFRs', 'variablesReference': 2, 'expensive': False},
            {'name': 'IRAM', 'variablesReference': 3, 'expensive': True},
        ]
        send_response(req, body={'scopes': scopes})

    def _read_var_value(self, addr, size):
        """Read a variable from IRAM and format its value."""
        data = self.read_mem(0x0D0000 + addr, size)
        if not data:
            return '??'
        if size == 1:
            v = data[0]
            return f'0x{v:02X} ({v})'
        elif size == 2:
            v = data[0] | (data[1] << 8)
            return f'0x{v:04X} ({v})'
        else:
            return ' '.join(f'{b:02X}' for b in data)

    def handle_variables(self, req):
        ref = req['arguments']['variablesReference']
        variables = []

        if ref == 1:  # Registers
            regs = self.get_regs()
            if regs:
                variables.append({'name': 'PC', 'value': f'0x{regs["PC"]:04X}', 'variablesReference': 0})
                variables.append({'name': 'ACC', 'value': f'0x{regs["ACC"]:02X} ({regs["ACC"]})', 'variablesReference': 0})
                variables.append({'name': 'B', 'value': f'0x{regs["B"]:02X}', 'variablesReference': 0})
                variables.append({'name': 'PSW', 'value': decode_psw(regs['PSW']), 'variablesReference': 0})
                variables.append({'name': 'SP', 'value': f'0x{regs["SP"]:02X}', 'variablesReference': 0})
                variables.append({'name': 'DPTR', 'value': f'0x{regs["DPTR"]:04X}', 'variablesReference': 0})
                for i in range(8):
                    variables.append({'name': f'R{i}', 'value': f'0x{regs[f"R{i}"]:02X}', 'variablesReference': 0})

        elif ref == 2:  # SFRs
            for addr in sorted(SFR_NAMES.keys()):
                name = SFR_NAMES[addr]
                val = self.read_sfr(addr)
                variables.append({
                    'name': f'{name} (0x{addr:02X})',
                    'value': f'0x{val:02X} ({val})',
                    'variablesReference': 0
                })

        elif ref == 3:  # IRAM (first 128 bytes)
            data = self.read_mem(0x0D0000, 128)
            for i in range(0, min(128, len(data)), 8):
                chunk = data[i:i+8]
                hex_str = ' '.join(f'{b:02X}' for b in chunk)
                variables.append({
                    'name': f'0x{i:02X}',
                    'value': hex_str,
                    'variablesReference': 0
                })

        elif ref == 4:  # Locals
            regs = self.get_regs()
            if regs:
                pc = regs['PC']
                func = self.cdb.get_func_at_addr(pc)
                if func:
                    local_vars = self.cdb.get_locals_for_func(func)
                    for vname, info in sorted(local_vars.items()):
                        addr = info['addr']
                        val = self._read_var_value(addr, 1)
                        variables.append({
                            'name': vname,
                            'value': val,
                            'type': f'byte @ IRAM[0x{addr:02X}]',
                            'variablesReference': 0
                        })
                if not variables:
                    variables.append({'name': '(no locals)', 'value': '', 'variablesReference': 0})

        elif ref == 5:  # Globals
            user_globals = self.cdb.get_user_globals()
            for name, info in sorted(user_globals.items()):
                addr = info.get('addr', 0)
                size = info.get('size', 1)
                val = self._read_var_value(addr, size)
                type_str = info.get('type', '?')
                variables.append({
                    'name': name,
                    'value': val,
                    'type': f'{type_str} @ 0x{addr:02X}',
                    'variablesReference': 0
                })
            if not variables:
                variables.append({'name': '(no globals)', 'value': '', 'variablesReference': 0})

        send_response(req, body={'variables': variables})

    def handle_setBreakpoints(self, req):
        args = req['arguments']
        source = args.get('source', {})
        source_name = source.get('name', '')
        source_path = source.get('path', '')
        requested_lines = [bp['line'] for bp in args.get('breakpoints', [])]

        # Clear old breakpoints for this file
        if source_path in self.breakpoints:
            for _, addr in self.breakpoints[source_path]:
                if addr is not None:
                    self.clear_breakpoint(addr)
            del self.breakpoints[source_path]

        # Extract just the filename for CDB lookup
        fname = os.path.basename(source_path) if source_path else source_name

        result_bps = []
        file_bps = []

        for line in requested_lines:
            addr = self.cdb.get_addr_for_line(fname, line)
            bp_id = self.bp_id_counter
            self.bp_id_counter += 1

            if addr is not None:
                self.set_breakpoint(addr)
                file_bps.append((line, addr))
                result_bps.append({
                    'id': bp_id,
                    'verified': True,
                    'line': line,
                    'source': source,
                })
            else:
                file_bps.append((line, None))
                result_bps.append({
                    'id': bp_id,
                    'verified': False,
                    'line': line,
                    'message': f'No code at {fname}:{line}',
                })

        self.breakpoints[source_path] = file_bps
        send_response(req, body={'breakpoints': result_bps})

    def handle_setFunctionBreakpoints(self, req):
        bps = req['arguments'].get('breakpoints', [])
        result = []
        for bp in bps:
            name = bp.get('name', '')
            addr = self.cdb.get_func_addr(name)
            bp_id = self.bp_id_counter
            self.bp_id_counter += 1
            if addr is not None:
                self.set_breakpoint(addr)
                result.append({'id': bp_id, 'verified': True})
            else:
                result.append({'id': bp_id, 'verified': False, 'message': f'Function {name} not found'})
        send_response(req, body={'breakpoints': result})

    def handle_continue(self, req):
        send_response(req, body={'allThreadsContinued': True})
        # Run continue in background so we can still receive messages
        def do_continue():
            self.continue_exec()
            send_event('stopped', {'reason': 'breakpoint', 'threadId': 1, 'allThreadsStopped': True})
        threading.Thread(target=do_continue, daemon=True).start()

    def handle_next(self, req):
        # Step over = step for now (proper step-over needs return address tracking)
        self.step()
        send_response(req)
        send_event('stopped', {'reason': 'step', 'threadId': 1, 'allThreadsStopped': True})

    def handle_stepIn(self, req):
        self.step()
        send_response(req)
        send_event('stopped', {'reason': 'step', 'threadId': 1, 'allThreadsStopped': True})

    def handle_stepOut(self, req):
        # Step out not cleanly supported; do a continue
        self.handle_continue(req)

    def handle_pause(self, req):
        if self.rsp:
            self.rsp.send_interrupt()
            self.stopped = True
        send_response(req)
        send_event('stopped', {'reason': 'pause', 'threadId': 1, 'allThreadsStopped': True})

    def handle_evaluate(self, req):
        expr = req['arguments'].get('expression', '').strip()
        context = req['arguments'].get('context', 'watch')

        # Try as SFR name
        if expr.lower() in SFR_BY_NAME:
            addr = SFR_BY_NAME[expr.lower()]
            val = self.read_sfr(addr)
            send_response(req, body={'result': f'0x{val:02X} ({val})', 'variablesReference': 0})
            return

        # Try as register
        regs = self.get_regs()
        if regs:
            upper = expr.upper()
            if upper in regs:
                val = regs[upper]
                if upper in ('PC', 'DPTR'):
                    send_response(req, body={'result': f'0x{val:04X}', 'variablesReference': 0})
                else:
                    send_response(req, body={'result': f'0x{val:02X} ({val})', 'variablesReference': 0})
                return

        # Try as hex address
        try:
            addr = int(expr, 0)
            data = self.read_mem(0x0D0000 + addr, 1)
            val = data[0] if data else 0
            send_response(req, body={'result': f'[0x{addr:02X}] = 0x{val:02X}', 'variablesReference': 0})
            return
        except (ValueError, IndexError):
            pass

        # Try as CDB symbol
        if expr in self.cdb.symbols:
            sym = self.cdb.symbols[expr]
            data = self.read_mem(0x0D0000 + sym['addr'], 1)
            val = data[0] if data else 0
            send_response(req, body={'result': f'0x{val:02X} ({val})', 'variablesReference': 0})
            return

        send_response(req, body={'result': f'(unknown: {expr})', 'variablesReference': 0})

    def handle_configurationDone(self, req):
        send_response(req)

    def handle_setVariable(self, req):
        # Minimal support
        send_response(req, body={'value': req['arguments'].get('value', '')})

    def _find_source(self, filename):
        """Search for source file in configured directories + CDB directory."""
        search = list(self.source_dirs)
        # Add CDB file's directory
        if hasattr(self.cdb, 'cdb_dir'):
            search.append(self.cdb.cdb_dir)
        search.append('.')

        for d in search:
            path = os.path.join(d, filename)
            if os.path.exists(path):
                return os.path.abspath(path)
        # Try absolute
        if os.path.exists(filename):
            return os.path.abspath(filename)
        return None


# ---------- Main loop ----------

def main():
    adapter = Dap8051Adapter()

    handlers = {
        'initialize':             adapter.handle_initialize,
        'launch':                 adapter.handle_launch,
        'disconnect':             adapter.handle_disconnect,
        'threads':                adapter.handle_threads,
        'stackTrace':             adapter.handle_stackTrace,
        'scopes':                 adapter.handle_scopes,
        'variables':              adapter.handle_variables,
        'setBreakpoints':         adapter.handle_setBreakpoints,
        'setFunctionBreakpoints': adapter.handle_setFunctionBreakpoints,
        'configurationDone':      adapter.handle_configurationDone,
        'continue':               adapter.handle_continue,
        'next':                   adapter.handle_next,
        'stepIn':                 adapter.handle_stepIn,
        'stepOut':                adapter.handle_stepOut,
        'pause':                  adapter.handle_pause,
        'evaluate':               adapter.handle_evaluate,
        'setVariable':            adapter.handle_setVariable,
    }

    while True:
        msg = read_dap_message()
        if msg is None:
            break

        cmd = msg.get('command', '')
        handler = handlers.get(cmd)

        if handler:
            try:
                handler(msg)
            except Exception as e:
                send_response(msg, success=False, message=str(e))
        else:
            # Unknown command — respond with success to avoid protocol errors
            send_response(msg)


if __name__ == '__main__':
    main()
