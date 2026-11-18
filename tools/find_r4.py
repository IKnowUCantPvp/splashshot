#!/usr/bin/env python3
"""Print the USB port of the plugged-in Arduino UNO R4 WiFi (used by deploy.sh and monitor.sh).
Exits with status 1 and a hint if none is found."""
import json
import subprocess
import sys

FQBN = "arduino:renesas_uno:unor4wifi"

try:
    out = subprocess.run(["arduino-cli", "board", "list", "--format", "json"],
                         capture_output=True, text=True, check=True).stdout
    ports = json.loads(out).get("detected_ports", [])
except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as e:
    sys.exit(f"Couldn't list boards with arduino-cli: {e}")

matches = [p["port"]["address"] for p in ports
           if any(b.get("fqbn") == FQBN for b in p.get("matching_boards", []))]
if not matches:  # not recognised by name: fall back to any USB modem port
    matches = [p["port"]["address"] for p in ports if "usbmodem" in p["port"]["address"]]
if not matches:
    sys.exit("No UNO R4 WiFi found. Plug it in over USB (a data cable, not a charge-only one) "
             "and try again.")
if len(matches) > 1:
    print(f"More than one board plugged in, using {matches[0]}", file=sys.stderr)
print(matches[0])
