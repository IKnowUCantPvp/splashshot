#!/bin/bash
# Deploy to the tray: bakes web/index.html into the firmware, compiles it, and uploads it
# to the UNO R4 WiFi plugged in over USB.
#   tools/deploy.sh            find the board automatically
#   tools/deploy.sh /dev/cu.usbmodem1234   use this port
# In VS Code: Cmd+Shift+B.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
FQBN=arduino:renesas_uno:unor4wifi

command -v arduino-cli >/dev/null || { echo "arduino-cli is missing. Install it with: brew install arduino-cli"; exit 1; }

echo "1/3  Baking the app into the firmware"
python3 tools/embed_web.py

echo "2/3  Compiling"
arduino-cli compile --fqbn "$FQBN" --build-path build/fw tray | grep -E "^(Sketch uses|Global variables)" || true

PORT="${1:-$(python3 tools/find_r4.py)}"
echo "3/3  Uploading to $PORT"
arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir build/fw tray

echo
echo "Done. In about 10 s the tray's Wi-Fi \"SplashShot\" is up (password waterpong)."
echo "Join it on your phone and open http://192.168.4.1"
