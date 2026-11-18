#!/usr/bin/env python3
"""Pretend tray for working on the app without hardware.

Runs the tray's real game logic (tray/game.h, compiled from tests/traysim.cpp) with
simulated load cells, and serves web/index.html against it exactly like the Arduino does.

    python3 tools/sim_server.py            # then open http://localhost:8765
    python3 tools/sim_server.py --cells 1  # like the one-load-cell prototype

Poke the pretend tray from another tab or with curl:
    /sim/drop?s=3     a ball lands in cup 3
    /sim/lift?s=2     someone picks cup 2 up      /sim/back?s=2   puts it back
    /sim/unplug?s=5   cup 5's sensor goes missing /sim/plug?s=5
"""
import argparse
import json
import math
import random
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "traysim"
SOURCES = [ROOT / "tests" / "traysim.cpp", ROOT / "tray" / "game.h", ROOT / "tray" / "detect.h"]


def build():
    if BIN.exists() and BIN.stat().st_mtime > max(p.stat().st_mtime for p in SOURCES):
        return
    BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["c++", "-std=c++17", "-O1", "-I", str(ROOT / "tray"), str(SOURCES[0]), "-o", str(BIN)], check=True)


class PretendTray:
    COUNTS, OFFSET = 2100, 84213  # raw counts per gram, raw reading of an empty spot

    def __init__(self, cells):
        self.proc = subprocess.Popen([str(BIN)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()
        self.t0 = time.monotonic()
        self.weight = [180.0] * 6  # cup + water, grams
        self.plugged = [i < cells for i in range(6)]
        self.slosh = [0.0] * 6
        self.slosh_at = [0] * 6
        threading.Thread(target=self.feed, daemon=True).start()

    def now(self):
        return int((time.monotonic() - self.t0) * 1000) + 1

    def send(self, line, reply=False):
        with self.lock:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            return self.proc.stdout.readline().strip() if reply else None

    def feed(self):  # 10 readings a second per cell, like an HX711
        while True:
            now = self.now()
            for i in range(6):
                if not self.plugged[i]:
                    continue
                t = (now - self.slosh_at[i]) / 1000
                wobble = self.slosh[i] * math.exp(-t / 0.35) * math.sin(2 * math.pi * 4 * t)
                grams = self.weight[i] + wobble + random.gauss(0, 0.25)
                self.send(f"s {i} {self.OFFSET + round(grams * self.COUNTS)} {now}")
            self.send(f"k {now}")
            time.sleep(0.1)

    def poke(self, action, cup):
        i = cup - 1
        if action == "drop":
            self.weight[i] += 2.7
            self.slosh[i], self.slosh_at[i] = 3.0, self.now()
        elif action == "lift":
            self.weight[i] -= 180
        elif action == "back":
            self.weight[i] += 180
        elif action == "unplug":
            self.plugged[i] = False
        elif action == "plug":
            self.plugged[i] = True
        else:
            return False
        return True


def make_handler(tray):
    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, body, ctype="application/json"):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urlsplit(self.path)
            if url.path in ("/", "/index.html"):
                self.reply(200, (ROOT / "web" / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif url.path == "/api/state":
                self.reply(200, tray.send(f"j {tray.now()}", reply=True))
            elif url.path == "/api/cmd":
                self.reply(200, tray.send(f"c {tray.now()} {url.query or 'c='}", reply=True))
            elif url.path.startswith("/sim/"):
                cup = int(parse_qs(url.query).get("s", ["1"])[0])
                ok = 1 <= cup <= 6 and tray.poke(url.path[5:], cup)
                self.reply(200 if ok else 400, json.dumps({"ok": ok}))
            else:
                self.reply(404, "not found", "text/plain")

        do_POST = do_GET

        def log_message(self, *args):
            pass

    return Handler


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--cells", type=int, default=6, help="how many load cells are plugged in (1-6)")
    args = ap.parse_args()
    build()
    tray = PretendTray(args.cells)
    print(f"Pretend tray with {args.cells} cell(s): http://localhost:{args.port}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(tray)).serve_forever()
