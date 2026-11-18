// SplashShot tray firmware - Arduino UNO R4 WiFi
//
// Weighs up to 6 cups (one HX711 + 1 kg load cell each), decides when a ball lands,
// keeps score, and serves the phone app over its own Wi-Fi network:
//   join Wi-Fi "SplashShot" (password "waterpong"), open http://192.168.4.1
//
// The game logic lives in game.h / detect.h (tested on a laptop, see tests/).
// The app lives in web/index.html and is baked into webapp.h by tools/embed_web.py.
#include <EEPROM.h>
#include <WiFiS3.h>

#include "config.h"
#include "game.h"
#include "hw_types.h"
#include "webapp.h"

#if USE_LED_MATRIX
#include "Arduino_LED_Matrix.h"
ArduinoLEDMatrix matrix;
#endif
#if USE_LED_RINGS
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel rings(6 * LEDS_PER_RING, LED_RING_PIN, NEO_GRB + NEO_KHZ800);
#endif
#if USE_OLED
#include <U8g2lib.h>
#include <Wire.h>
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif

using namespace splash;

Game game;
WiFiServer server(80);
bool wifiUp = false;
static char jsonBuf[3000];
static char reqLine[256];

// ---------------------------------------------------------------- HX711

// Reads one 24-bit sample (channel A, gain 128). Only call when DT is LOW (data ready).
// Interrupts stay on: an interrupt can only stretch a clock pulse by a few microseconds,
// far below the 60 us that would put the HX711 to sleep, and the Wi-Fi link needs them.
static long hxRead(uint8_t dt, uint8_t sck) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 24; i++) {
    digitalWrite(sck, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | (digitalRead(dt) == HIGH ? 1u : 0u);
    digitalWrite(sck, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(sck, HIGH);  // 25th pulse: next reading is channel A, gain 128
  delayMicroseconds(1);
  digitalWrite(sck, LOW);
  if (v & 0x800000u) v |= 0xFF000000u;  // sign-extend 24 -> 32 bits
  return (long)(int32_t)v;
}

void readSensors(uint32_t now) {
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    // A missing HX711 leaves DT pulled HIGH, so that cup just never reports.
    if (digitalRead(HX_DT_PINS[i]) != LOW) continue;
    long raw = hxRead(HX_DT_PINS[i], HX_SCK_PINS[i]);
    if (now < 1000) continue;  // the HX711 needs a moment after power-up
    game.onSample(i, raw, now);
  }
}

// ---------------------------------------------------------------- saved settings

struct Saved {
  uint32_t magic;
  float factor[6];
  uint8_t calibrated[6];
  float threshold;
  uint8_t difficulty, bar;
};
const uint32_t SAVE_MAGIC = 0x53504C31;  // "SPL1"

void loadSettings() {
  Saved s;
  EEPROM.get(0, s);
  if (s.magic != SAVE_MAGIC) return;
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    if (s.calibrated[i] == 1 && isfinite(s.factor[i]) && fabsf(s.factor[i]) > 1) {
      game.spots[i].factor = s.factor[i];
      game.spots[i].calibrated = true;
    }
  }
  if (s.threshold >= 0.5f && s.threshold <= 5.0f) game.cfg.hitThresholdG = s.threshold;
  if (s.difficulty < NUM_DIFFICULTIES) game.difficulty = s.difficulty;
  if (s.bar < NUM_BARS) game.bar = s.bar;
}

void saveSettings() {
  Saved s = {};
  s.magic = SAVE_MAGIC;
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    s.factor[i] = game.spots[i].factor;
    s.calibrated[i] = game.spots[i].calibrated ? 1 : 0;
  }
  s.threshold = game.cfg.hitThresholdG;
  s.difficulty = game.difficulty;
  s.bar = game.bar;
  EEPROM.put(0, s);
}

void saveIfChanged(uint32_t now) {
  static uint32_t changedAt = 0;
  if (game.settingsChanged) {
    game.settingsChanged = false;
    changedAt = now | 1;
  }
  if (changedAt && now - changedAt > 1000) {  // wait for slider drags to finish
    saveSettings();
    changedAt = 0;
    Serial.println("Settings saved");
  }
}

// ---------------------------------------------------------------- buttons

Button startButton = {START_BUTTON_PIN, HIGH, HIGH, 0};
Button tareButton = {TARE_BUTTON_PIN, HIGH, HIGH, 0};

bool pressed(Button& b, uint32_t now) {
  bool r = digitalRead(b.pin);
  if (r != b.reading) {
    b.reading = r;
    b.changedMs = now;
  }
  if (now - b.changedMs > 30 && r != b.stable) {
    b.stable = r;
    return r == LOW;
  }
  return false;
}

// ---------------------------------------------------------------- lights

// What each cup should show right now. A fresh hit blinks for 2 s.
CupLook look(uint8_t i, uint32_t now, bool& blinkOff) {
  const Spot& s = game.spots[i];
  blinkOff = false;
  if (!s.present(now)) return OFF;
  if (s.hit) {
    if (now - game.lastHitMs[i] < 2000) blinkOff = (now / 200) % 2;
    return HIT;
  }
  if (s.lifted) return LIFTED;
  return STANDING;
}

void updateLights(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 100) return;
  last = now;

  bool anyHit = false;
  for (uint8_t i = 0; i < NUM_SPOTS; i++) anyHit |= game.spots[i].hit;
  if (HIT_LED_PIN != 255) digitalWrite(HIT_LED_PIN, anyHit ? HIGH : LOW);

#if USE_LED_MATRIX
  // Triangle of 2x2 blocks, tip (cup 1) at the top. Lit = cup still standing.
  static const uint8_t pos[6][2] = {{5, 0}, {3, 3}, {7, 3}, {1, 6}, {5, 6}, {9, 6}};
  static uint8_t shown[8][12];
  uint8_t frame[8][12] = {};
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    bool blinkOff;
    CupLook l = look(i, now, blinkOff);
    bool on = l == STANDING || (l == HIT && !blinkOff && now - game.lastHitMs[i] < 2000) ||
              (l == LIFTED && (now / 500) % 2);
    if (!on) continue;
    for (uint8_t dy = 0; dy < 2; dy++)
      for (uint8_t dx = 0; dx < 2; dx++) frame[pos[i][1] + dy][pos[i][0] + dx] = 1;
  }
  if (!wifiUp) frame[0][0] = (now / 250) % 2;  // corner blinks until Wi-Fi is up
  if (memcmp(frame, shown, sizeof frame)) {
    memcpy(shown, frame, sizeof frame);
    matrix.renderBitmap(frame, 8, 12);
  }
#endif

#if USE_LED_RINGS
  // Red = standing, green = hit, amber = picked up. Only send when something changed,
  // because sending the LED data pauses interrupts for about 1.5 ms.
  static uint32_t shownColor[6] = {1, 1, 1, 1, 1, 1};
  bool changed = false;
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    bool blinkOff;
    CupLook l = look(i, now, blinkOff);
    uint32_t c = 0;
    if (l == STANDING) c = rings.Color(255, 0, 0);
    if (l == HIT && !blinkOff) c = rings.Color(0, 255, 0);
    if (l == LIFTED) c = rings.Color(160, 90, 0);
    if (c != shownColor[i]) {
      shownColor[i] = c;
      changed = true;
      for (uint8_t k = 0; k < LEDS_PER_RING; k++) rings.setPixelColor(i * LEDS_PER_RING + k, c);
    }
  }
  if (changed) rings.show();
#endif

#if USE_OLED
  static uint32_t lastOled = 0;
  if (now - lastOled >= 500) {
    lastOled = now;
    char line[32];
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 10, "ROBOT");
    oled.drawStr(98, 10, "YOU");
    oled.setFont(u8g2_font_logisoso24_tr);
    snprintf(line, sizeof line, "%d - %d", game.robot(), game.you);
    oled.drawStr((128 - oled.getStrWidth(line)) / 2, 40, line);
    oled.setFont(u8g2_font_6x12_tr);
    if (game.phase == OVER) snprintf(line, sizeof line, game.winner == 'R' ? "ROBOT WINS" : "YOU WIN");
    else if (game.phase == READY) snprintf(line, sizeof line, "Wi-Fi %s", WIFI_SSID);
    else snprintf(line, sizeof line, "%d left  %s", game.cupsLeft(now), DIFFICULTY_NAMES[game.difficulty]);
    oled.drawStr(0, 62, line);
    oled.sendBuffer();
  }
#endif
}

// ---------------------------------------------------------------- web server

// Reads the request line ("GET /api/state HTTP/1.1") into reqLine and skips the headers.
bool readRequest(WiFiClient& c) {
  size_t len = 0;
  bool gotFirstLine = false;
  int lineLen = 0;
  uint8_t buf[128];
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    int n = c.read(buf, sizeof buf);
    if (n <= 0) {
      if (!c.connected()) break;
      delay(1);
      continue;
    }
    for (int i = 0; i < n; i++) {
      char ch = (char)buf[i];
      if (ch == '\r') continue;
      if (ch == '\n') {
        if (gotFirstLine && lineLen == 0) return true;  // blank line: headers done
        gotFirstLine = true;
        lineLen = 0;
        continue;
      }
      lineLen++;
      if (!gotFirstLine && len + 1 < sizeof reqLine) reqLine[len++] = ch;
      reqLine[len] = 0;
    }
  }
  return gotFirstLine;
}

void send(WiFiClient& c, const char* status, const char* type, const uint8_t* body, size_t len,
          bool gzip) {
  char h[256];
  int n = snprintf(h, sizeof h,
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s"
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                   status, type, (unsigned)len, gzip ? "Content-Encoding: gzip\r\n" : "");
  c.write((const uint8_t*)h, n);
  for (size_t i = 0; i < len; i += 1024) c.write(body + i, min((size_t)1024, len - i));
}

void route(WiFiClient& c, const char* path, const char* query) {
  uint32_t now = millis();
  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    send(c, "200 OK", "text/html; charset=utf-8", WEBAPP_GZ, WEBAPP_GZ_LEN, true);
  } else if (!strcmp(path, "/api/state")) {
    size_t n = game.stateJson(jsonBuf, sizeof jsonBuf, now);
    send(c, "200 OK", "application/json", (const uint8_t*)jsonBuf, n, false);
  } else if (!strcmp(path, "/api/cmd")) {
    game.command(query, jsonBuf, sizeof jsonBuf, now);
    send(c, "200 OK", "application/json", (const uint8_t*)jsonBuf, strlen(jsonBuf), false);
  } else {
    send(c, "404 Not Found", "text/plain", (const uint8_t*)"not found", 9, false);
  }
}

void serveWeb() {
  if (!wifiUp) return;
  WiFiClient client = server.available();
  if (!client) return;
  if (readRequest(client)) {
    char* path = strchr(reqLine, ' ');  // "GET /api/cmd?c=tare HTTP/1.1"
    if (path) {
      path++;
      char* end = strchr(path, ' ');
      if (end) *end = 0;
      char* query = strchr(path, '?');
      if (query) *query++ = 0;
      route(client, path, query ? query : "");
    }
  }
  client.stop();
}

// ---------------------------------------------------------------- Wi-Fi

// Keeps sensors and lights running while we wait on Wi-Fi.
void idle(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    uint32_t now = millis();
    readSensors(now);
    game.tick(now);
    updateLights(now);
    delay(2);
  }
}

void startWifi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Wi-Fi module not responding");
    return;
  }
#if JOIN_EXISTING_WIFI
  Serial.print("Joining Wi-Fi ");
  Serial.println(WIFI_SSID);
  for (uint8_t tries = 0; tries < 3 && WiFi.status() != WL_CONNECTED; tries++) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    idle(1000);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Could not join. Check WIFI_SSID / WIFI_PASS in config.h");
    return;
  }
  uint32_t t0 = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < 10000) idle(200);
#else
  WiFi.config(IPAddress(192, 168, 4, 1));
  if (WiFi.beginAP(WIFI_SSID, WIFI_PASS) != WL_AP_LISTENING) {
    Serial.println("Could not start the Wi-Fi network");
    return;
  }
  idle(10000);  // the network takes a few seconds to come up (Arduino's own example waits 10 s)
#endif
  server.begin();
  wifiUp = true;
  Serial.print("Wi-Fi \"" WIFI_SSID "\" is up. Open http://");
  Serial.println(WiFi.localIP());
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < NUM_SPOTS; i++) {
    pinMode(HX_SCK_PINS[i], OUTPUT);
    digitalWrite(HX_SCK_PINS[i], LOW);
    pinMode(HX_DT_PINS[i], INPUT_PULLUP);
  }
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TARE_BUTTON_PIN, INPUT_PULLUP);
  if (HIT_LED_PIN != 255) pinMode(HIT_LED_PIN, OUTPUT);

#if USE_LED_MATRIX
  matrix.begin();
#endif
#if USE_LED_RINGS
  rings.begin();
  rings.setBrightness(RING_BRIGHTNESS);
  rings.clear();
  rings.show();
#endif
#if USE_OLED
  oled.begin();
#endif

  for (auto& s : game.spots) s.factor = DEFAULT_COUNTS_PER_GRAM;
  game.begin();
  loadSettings();
  Serial.println("SplashShot tray starting");
  startWifi();
}

void loop() {
  uint32_t now = millis();
  readSensors(now);
  game.tick(now);
  if (pressed(startButton, now)) game.startGame(now);
  if (pressed(tareButton, now)) game.tareAll(now);

  serveWeb();

  now = millis();
  readSensors(now);
  updateLights(now);
  saveIfChanged(now);

  if (const char* e = game.takeNewEvent()) {
    Serial.print("> ");
    Serial.println(e);
  }
#if SERIAL_DEBUG
  static uint32_t lastPrint = 0;
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    bool any = false;
    for (uint8_t i = 0; i < NUM_SPOTS; i++) {
      const Spot& s = game.spots[i];
      if (!s.present(now)) continue;
      any = true;
      Serial.print("cup");
      Serial.print(i + 1);
      Serial.print(' ');
      Serial.print(s.grams, 2);
      Serial.print("g (change ");
      Serial.print(s.change(), 2);
      Serial.print(", noise ");
      Serial.print(s.spread, 2);
      Serial.print(s.calibrated ? ")  " : ", NOT CALIBRATED)  ");
    }
    Serial.println(any ? "" : "no load cells found: check DT/SCK wiring (cup 1 = D2/D8)");
  }
#endif
}
