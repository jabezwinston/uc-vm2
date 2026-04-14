#!/bin/sh
# Wrapper script that VS Code calls as the debug adapter executable.
# Finds and runs dap8051.py from the ucvm tools directory.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR/../../../tools"

exec python3 "$TOOLS_DIR/dap8051.py"
