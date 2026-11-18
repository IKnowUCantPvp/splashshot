#!/bin/bash
# Shows what the tray prints over USB: each cup's weight once a second, and every event.
# Ctrl+C to stop. In VS Code: Terminal > Run Task > Serial monitor.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
PORT="${1:-$(python3 tools/find_r4.py)}"
echo "Listening on $PORT at 115200 baud (Ctrl+C to stop)"
arduino-cli monitor --port "$PORT" --config baudrate=115200
