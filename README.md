# SplashShot tray + app

The tray's Arduino UNO R4 WiFi weighs each cup, works out when a ball lands, keeps score,
and serves a phone app over its **own Wi-Fi network**. No internet, router or campus Wi-Fi
is needed, which matches the build plan.

On your phone: join Wi-Fi **SplashShot** (password **waterpong**) and open **http://192.168.4.1**

The app does what the MVP needs:
- **Zero scales**, remotely or with the tray's Tare button
- a **live cup map**: the 3-2-1 triangle, red = standing, green = hit, matching the LED rings
- the **score**, which updates when a cup detects a ball

It also has the build plan's other screens: Start game, difficulty (Easy / Medium / Hard /
Pro / Adaptive), bar length, an "I scored" button for your own shots, a log of what happened,
a game history with your record at each difficulty, and a Sensors tab for calibrating each
cup and tuning hit sensitivity.

---

## Get the code

```bash
git clone https://github.com/IKnowUCantPvp/splashshot.git
```

First time on a computer:
- **Mac:** `brew install arduino-cli`, then `arduino-cli core install arduino:renesas_uno`.
  On an Apple Silicon Mac also run `softwareupdate --install-rosetta --agree-to-license`.
- **Windows:** use the Arduino IDE. Install the "Arduino UNO R4 Boards" package from the Boards
  Manager, open `tray/tray.ino`, pick *Arduino UNO R4 WiFi* and click Upload. The `.sh`
  scripts below are for Mac and Linux.

## Deploying from VS Code

Open this folder in VS Code, plug the tray's R4 into your Mac over USB, and press
**Cmd+Shift+B** (*Deploy to tray*). That bakes the app into the firmware, compiles it, finds
the board's port and uploads, in about a minute. Then join SplashShot and open 192.168.4.1.

Other tasks are under **Terminal → Run Task**:

| Task | What it does |
|---|---|
| Serial monitor | Shows each cup's weight once a second, plus every event (Ctrl+C to stop) |
| Compile only | Checks the firmware builds, no board needed |
| Run tests | The hit-detection and game logic tests on your laptop |
| Pretend tray (no hardware) | The app against the real game code at http://localhost:8765 |

The same commands work in any terminal: `tools/deploy.sh`, `tools/monitor.sh`.
They need `arduino-cli` (`brew install arduino-cli`) and, on Apple Silicon, Rosetta.
Install the recommended C/C++ extension when VS Code offers it, so the code gets autocomplete.

## Demo for Sid (one load cell)

1. Wire the one HX711 as **cup 1**: DT → **D2**, SCK → **D8**, VCC → 5V, GND → GND.
   Load cell to HX711: red E+, black E−, white A−, green A+.
2. Optional: an LED + 220–330 Ω resistor from **A3** to GND. It lights when a cup is hit,
   which is the "light an LED when the weight changes" test. The R4's own LED matrix
   also shows the cups, so you can skip this.
3. Upload `tray/tray.ino` (see below). Open the Serial Monitor at **115200** and you'll see
   `cup1 0.03g (change 0.01, noise 0.30, NOT CALIBRATED)` once a second.
4. Join **SplashShot** on a phone and open **192.168.4.1**. If the phone says "no internet"
   and offers to switch to mobile data, choose to stay connected.
5. **Calibrate cup 1 once.** Go to the Sensors tab → Calibrate, take everything off, tap
   *Zero the empty spot*, put a known weight on it (a cup of water you weighed on a kitchen
   scale works), type the grams and tap *Save calibration*. The tray remembers it.
6. Put the cup on, tap **Zero scales**, and drop a ping-pong ball in. About a second later
   the cup turns green, the score goes to 1, and the LED lights. With one sensor the game is
   "first to 1", so that hit ends it. Tap **New game** to go again.

**Backup if the hardware isn't ready:** the app has a built-in demo with a pretend tray.
Open `web/index.html` in any browser, or `http://192.168.4.1/?demo`.

## Uploading the firmware

Arduino IDE → board **Arduino UNO R4 WiFi** → open `tray/tray.ino` → Upload.
The demo needs **no extra libraries**; everything it uses ships with the R4 board package.

**On an Apple Silicon Mac** the R4's compiler is an Intel program, so the IDE fails with
"bad CPU type" until Rosetta is installed. Run this once in Terminal:

```bash
softwareupdate --install-rosetta --agree-to-license
```

All the settings (Wi-Fi name, pins, LED options) are in `tray/config.h`.

Or from Terminal with `arduino-cli` (`brew install arduino-cli`). Compile, then upload to the
port that `arduino-cli board list` shows:

```bash
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi tray
```

```bash
arduino-cli upload --fqbn arduino:renesas_uno:unor4wifi -p /dev/cu.usbmodemXXXX tray
```

The sketch uses about a third of the R4's flash and 40% of its RAM.

## Full tray (6 cups) and the parts list

The pins follow the **SplashShot Electrical Plan** exactly:

| Cup | HX711 DT | HX711 SCK | Where |
|---|---|---|---|
| 1 | D2 | D8 | tip of the triangle, nearest the robot |
| 2 | D3 | D12 | middle row, left (from the player's side) |
| 3 | D4 | D13 | middle row, right |
| 4 | D5 | A0 | back row, left |
| 5 | D6 | A1 | back row, middle |
| 6 | D7 | A2 | back row, right |

Start button D10, Tare button D11 (other side to GND). Label the spots with tape so the
wiring and the app agree. A cup with no HX711 answering shows as "no sensor", and the game
plays to however many cups are connected.

When the rest of the tray hardware arrives:
- **LED rings**: install the *Adafruit NeoPixel* library, set `USE_LED_RINGS 1` in `config.h`.
  Data on D9, brightness capped at 80/255 as the electrical plan says.
- **OLED**: install *U8g2*, set `USE_OLED 1`. It shows the score, cups left and difficulty.

## How hits are detected

Each cup's reading is averaged over half a second. A cup counts as **hit** when it gains
more than the threshold (1.5 g by default; a ball is 2.7 g) and holds it for a second, so the
water has settled. Other rules:
- A drop of more than 40 g means the cup was **picked up**. It isn't a hit, and the cup
  re-zeroes itself when it's put back.
- A gain of more than 15 g (a hand, a refill) is never a hit. If it stays, the cup re-zeroes
  after 3 s.
- Slow drift (load-cell creep, evaporation) is followed automatically.
- A wrong call can be fixed from the app: tap the cup → *Count as hit* or *Not a hit*.

These rules run in `tray/detect.h` and are tested on a laptop with simulated noise,
sloshing, lifts, refills and drift (`tests/test_tray.cpp`).

## Working on the app without the tray

```bash
python3 tools/sim_server.py
```

Then open http://localhost:8765. This runs the tray's real game code with pretend load cells.
Drop a ball with http://localhost:8765/sim/drop?s=3 (also `lift`, `back`, `unplug`, `plug`).
Use `--cells 1` to act like the one-cell prototype.

After editing `web/index.html`, bake it into the firmware and re-upload:

```bash
python3 tools/embed_web.py
```

Tests and a firmware type-check (works without Rosetta):

```bash
c++ -std=c++17 -I tray tests/test_tray.cpp -o build/test_tray && build/test_tray
```

```bash
tools/check_firmware.sh
```

## Files

| File | What it is |
|---|---|
| `tray/tray.ino` | Hardware: HX711 reading, Wi-Fi network, web server, buttons, LEDs, saving settings |
| `tray/config.h` | Everything you might change: Wi-Fi name, pins, LED/OLED switches |
| `tray/game.h` | Score, game phases, event log, history, app commands, the JSON the app reads |
| `tray/detect.h` | Per-cup weight tracking and hit detection |
| `tray/webapp.h` | The app, gzipped. Generated, don't edit |
| `web/index.html` | The app (one file, no internet needed) |

## The app's API (for the robot later)

- `GET /api/state` returns the whole game as JSON. Weights are in centigrams.
- `POST /api/cmd?c=...` runs a command: `tare`, `start`, `reset`, `you&v=1|-1`,
  `hit&s=3&v=1|0`, `dif&v=0-4`, `bar&v=0-2`, `thr&v=150`, `zero&s=3`, `cal&s=3&v=10000`.

## Not done yet

- **Robot link.** Difficulty and bar length are saved on the tray, but nothing sends them to
  the turret yet. Per the plan, the turret's R4 joins this Wi-Fi network and the tray sends it
  UDP messages. The app already shows "Robot not linked yet".
- **Adaptive difficulty logic** lives on the robot side, so it comes with the robot link.
- **Untested on real hardware:** the Wi-Fi server and HX711 timing. Everything is type-checked
  against the real R4 core, and the game and detection logic are tested on a laptop, but
  none of it has run on a board yet. If the page loads slowly or not at all, watch the
  Serial Monitor first.
- The LED-ring and OLED code is untested because those libraries aren't installed here.
