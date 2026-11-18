// game.h - the game on the tray: six cup spots, scores, difficulty, event log,
// game history, the commands the app sends and the JSON state the app reads.
//
// Plain C++ like detect.h, so tests/test_tray.cpp and tools/sim_server.py can run it
// on a laptop. tray.ino owns the hardware and calls into this.
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "detect.h"

namespace splash {

const uint8_t NUM_SPOTS = 6;
const char* const DIFFICULTY_NAMES[] = {"Easy", "Medium", "Hard", "Pro", "Adaptive"};
const uint8_t NUM_DIFFICULTIES = 5;
const char* const BAR_NAMES[] = {"Short", "Medium", "Long"};
const uint8_t NUM_BARS = 3;

enum Phase : uint8_t { READY = 0, PLAYING = 1, OVER = 2 };

struct Event {
  uint16_t id;
  uint32_t t;
  char text[44];
};

struct GameRecord {
  uint8_t difficulty, robot, you;
  char winner;  // 'R' robot, 'Y' you
  uint16_t seconds;
};

// Small JSON writer into a fixed buffer.
struct Json {
  char* p;
  size_t cap, len = 0;
  Json(char* buf, size_t c) : p(buf), cap(c) {
    if (cap) p[0] = 0;
  }
  void raw(const char* s) {
    while (*s && len + 1 < cap) p[len++] = *s++;
    p[len] = 0;
  }
  void num(long v) {
    char b[16];
    snprintf(b, sizeof b, "%ld", v);
    raw(b);
  }
  void str(const char* s) {
    raw("\"");
    for (; *s && len + 3 < cap; s++) {
      char c = *s;
      if (c == '"' || c == '\\') p[len++] = '\\';
      if ((unsigned char)c < 0x20) c = ' ';
      p[len++] = c;
    }
    p[len] = 0;
    raw("\"");
  }
  void key(const char* k) {
    str(k);
    raw(":");
  }
};

// "12.3" without printf float support (not every Arduino core has it).
inline void formatTenths(char* out, size_t cap, float v) {
  long t = lroundf(v * 10.0f);
  const char* sign = t < 0 ? "-" : "";
  t = labs(t);
  snprintf(out, cap, "%s%ld.%ld", sign, t / 10, t % 10);
}

class Game {
 public:
  static const uint8_t MAX_EVENTS = 12;
  static const uint8_t MAX_HISTORY = 8;

  DetectConfig cfg;
  Spot spots[NUM_SPOTS];
  uint8_t phase = READY;
  uint8_t you = 0;
  uint8_t difficulty = 1;  // Medium
  uint8_t bar = 0;         // Short
  char winner = 0;
  uint32_t startMs = 0, endMs = 0;
  bool settingsChanged = false;        // tray.ino saves these to EEPROM
  uint32_t lastHitMs[NUM_SPOTS] = {};  // for LED effects

  void begin() {
    for (auto& s : spots) {
      s.cfg = &cfg;
      s.tare();
    }
  }

  uint8_t robot() const {
    uint8_t c = 0;
    for (const auto& s : spots) c += s.hit ? 1 : 0;
    return c;
  }
  uint8_t cupsInPlay(uint32_t now) const {
    uint8_t c = 0;
    for (const auto& s : spots) c += s.present(now) ? 1 : 0;
    return c;
  }
  // Cups each side has to sink: one per connected sensor, so a 1-cell prototype plays to 1.
  uint8_t target(uint32_t now) const {
    uint8_t c = cupsInPlay(now);
    return c ? c : NUM_SPOTS;
  }
  uint8_t cupsLeft(uint32_t now) const {
    uint8_t c = 0;
    for (const auto& s : spots) c += (s.present(now) && !s.hit) ? 1 : 0;
    return c;
  }

  // ---- inputs ----

  void onSample(uint8_t i, long raw, uint32_t now) {
    Spot& s = spots[i];
    char g[12];
    switch (s.addSample(raw, now, phase != OVER)) {
      case SpotEvent::Hit:
        lastHitMs[i] = now;
        if (phase == READY) beginPlay(now, false);
        formatTenths(g, sizeof g, s.hitGain);
        log(now, "Cup %d hit (+%s g)", i + 1, g);
        checkOver(now);
        break;
      case SpotEvent::Lifted:
        log(now, "Cup %d picked up", i + 1);
        break;
      case SpotEvent::Back:
        log(now, "Cup %d put back", i + 1);
        break;
      case SpotEvent::Rezeroed:
        log(now, "Cup %d weight changed, re-zeroed", i + 1);
        break;
      case SpotEvent::None:
        break;
    }
  }

  // Call every loop: notices sensors coming and going and ends the game.
  void tick(uint32_t now) {
    for (uint8_t i = 0; i < NUM_SPOTS; i++) {
      bool p = spots[i].present(now);
      if (p != wasPresent[i]) {
        wasPresent[i] = p;
        if (!p) log(now, "Cup %d sensor lost", i + 1);
        else if (now > 5000) log(now, "Cup %d sensor connected", i + 1);  // quiet at boot
      }
    }
    checkOver(now);
  }

  void tareAll(uint32_t now) {
    for (auto& s : spots) s.tare();
    log(now, "Scales zeroed");
  }

  void startGame(uint32_t now) {
    for (auto& s : spots) {
      s.setHit(false);
      s.tare();
    }
    you = 0;
    beginPlay(now, true);
  }

  void resetGame(uint32_t now) {
    for (auto& s : spots) s.setHit(false);
    you = 0;
    winner = 0;
    phase = READY;
    startMs = endMs = 0;
    log(now, "Game reset");
  }

  // Runs one command from the app ("c=tare", "c=hit&s=3&v=1", ...). Writes {"ok":...} to out.
  bool command(const char* query, char* out, size_t cap, uint32_t now) {
    char c[12] = "", sv[12] = "", vv[16] = "";
    param(query, "c", c, sizeof c);
    bool hasS = param(query, "s", sv, sizeof sv);
    param(query, "v", vv, sizeof vv);
    long v = atol(vv);
    int s = hasS ? atoi(sv) - 1 : -1;  // cups are numbered 1-6 in the app
    const char* err = nullptr;

    if (!strcmp(c, "tare")) {
      tareAll(now);
    } else if (!strcmp(c, "start")) {
      startGame(now);
    } else if (!strcmp(c, "reset")) {
      resetGame(now);
    } else if (!strcmp(c, "you")) {
      err = addYou(v >= 0 ? 1 : -1, now);
    } else if (!strcmp(c, "hit")) {
      err = validSpot(s) ? setHitByHand(s, v != 0, now) : "Unknown cup";
    } else if (!strcmp(c, "dif")) {
      if (v < 0 || v >= NUM_DIFFICULTIES) err = "Unknown difficulty";
      else if (difficulty != v) {
        difficulty = (uint8_t)v;
        settingsChanged = true;
        log(now, "Difficulty: %s", DIFFICULTY_NAMES[difficulty]);
      }
    } else if (!strcmp(c, "bar")) {
      if (v < 0 || v >= NUM_BARS) err = "Unknown bar length";
      else if (bar != v) {
        bar = (uint8_t)v;
        settingsChanged = true;
        log(now, "Bar length: %s", BAR_NAMES[bar]);
      }
    } else if (!strcmp(c, "thr")) {  // centigrams
      if (v < 50 || v > 500) err = "Threshold must be 0.5 to 5 g";
      else {
        cfg.hitThresholdG = v / 100.0f;
        settingsChanged = true;
      }
    } else if (!strcmp(c, "zero")) {  // zero one cup
      if (!validSpot(s)) err = "Unknown cup";
      else if (!spots[s].present(now)) err = "That cup's sensor isn't connected";
      else {
        spots[s].tare();
        log(now, "Cup %d zeroed", s + 1);
      }
    } else if (!strcmp(c, "cal")) {  // v = known weight in centigrams
      if (!validSpot(s)) err = "Unknown cup";
      else if (!spots[s].present(now)) err = "That cup's sensor isn't connected";
      else if (!spots[s].calibrate(v / 100.0f)) err = "Zero the empty spot, then put the weight on";
      else {
        settingsChanged = true;
        log(now, "Cup %d calibrated (%ld counts/g)", s + 1, lroundf(spots[s].factor));
      }
    } else {
      err = "Unknown command";
    }

    Json j(out, cap);
    j.raw("{\"ok\":");
    j.raw(err ? "false" : "true");
    if (err) {
      j.raw(",\"err\":");
      j.str(err);
    }
    j.raw("}");
    return !err;
  }

  // ---- output ----

  size_t stateJson(char* out, size_t cap, uint32_t now) const {
    Json j(out, cap);
    j.raw("{\"up\":");
    j.num((long)now);
    j.raw(",\"ph\":");
    j.num(phase);
    j.raw(",\"win\":");
    char w[2] = {winner, 0};
    j.str(w);
    j.raw(",\"dif\":");
    j.num(difficulty);
    j.raw(",\"bar\":");
    j.num(bar);
    j.raw(",\"thr\":");
    j.num(lroundf(cfg.hitThresholdG * 100));
    j.raw(",\"rob\":");
    j.num(robot());
    j.raw(",\"you\":");
    j.num(you);
    j.raw(",\"tgt\":");
    j.num(target(now));
    j.raw(",\"el\":");
    uint32_t until = phase == OVER ? endMs : now;
    j.num(phase == READY ? 0 : (long)((until - startMs) / 1000));

    j.raw(",\"spots\":[");
    for (uint8_t i = 0; i < NUM_SPOTS; i++) {
      const Spot& s = spots[i];
      if (i) j.raw(",");
      j.raw("{\"p\":");
      j.num(s.present(now));
      j.raw(",\"z\":");
      j.num(s.zeroed());
      j.raw(",\"c\":");
      j.num(s.calibrated);
      j.raw(",\"h\":");
      j.num(s.hit);
      j.raw(",\"l\":");
      j.num(s.lifted);
      j.raw(",\"g\":");  // centigrams since zero
      j.num(lroundf(s.grams * 100));
      j.raw(",\"d\":");  // centigrams of change since the cup last settled
      j.num(lroundf(s.change() * 100));
      j.raw(",\"n\":");  // centigrams of noise / sloshing over the last second
      j.num(lroundf(s.spread * 100));
      j.raw(",\"hg\":");  // what the hit added, centigrams
      j.num(lroundf(s.hitGain * 100));
      j.raw("}");
    }

    j.raw("],\"ev\":[");
    for (uint8_t k = 0; k < eventCount; k++) {
      const Event& e = events[(eventHead + MAX_EVENTS - eventCount + k) % MAX_EVENTS];
      if (k) j.raw(",");
      j.raw("[");
      j.num(e.id);
      j.raw(",");
      j.num((long)e.t);
      j.raw(",");
      j.str(e.text);
      j.raw("]");
    }

    j.raw("],\"hist\":[");
    for (uint8_t k = 0; k < historyCount; k++) {
      const GameRecord& r = history[(historyHead + MAX_HISTORY - historyCount + k) % MAX_HISTORY];
      if (k) j.raw(",");
      j.raw("[");
      j.num(r.difficulty);
      j.raw(",");
      j.num(r.robot);
      j.raw(",");
      j.num(r.you);
      j.raw(",");
      char wr[2] = {r.winner, 0};
      j.str(wr);
      j.raw(",");
      j.num(r.seconds);
      j.raw("]");
    }
    j.raw("],\"robot\":{\"linked\":0}}");
    return j.len;
  }

  void log(uint32_t now, const char* fmt, ...) {
    Event& e = events[eventHead];
    e.id = ++lastEventId;
    e.t = now;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    eventHead = (eventHead + 1) % MAX_EVENTS;
    if (eventCount < MAX_EVENTS) eventCount++;
    newEvent = true;
  }

  // The newest event text, once: for Serial and the OLED.
  const char* takeNewEvent() {
    if (!newEvent) return nullptr;
    newEvent = false;
    return events[(eventHead + MAX_EVENTS - 1) % MAX_EVENTS].text;
  }

 private:
  Event events[MAX_EVENTS] = {};
  uint8_t eventHead = 0, eventCount = 0;
  uint16_t lastEventId = 0;
  bool newEvent = false;
  GameRecord history[MAX_HISTORY] = {};
  uint8_t historyHead = 0, historyCount = 0;
  bool wasPresent[NUM_SPOTS] = {};

  static bool validSpot(int s) { return s >= 0 && s < NUM_SPOTS; }

  static bool param(const char* q, const char* key, char* out, size_t cap) {
    size_t kl = strlen(key);
    const char* p = q;
    while (p && *p) {
      if (!strncmp(p, key, kl) && p[kl] == '=') {
        p += kl + 1;
        size_t i = 0;
        while (*p && *p != '&' && i + 1 < cap) out[i++] = *p++;
        out[i] = 0;
        return true;
      }
      p = strchr(p, '&');
      if (p) p++;
    }
    return false;
  }

  void beginPlay(uint32_t now, bool announce) {
    phase = PLAYING;
    winner = 0;
    startMs = now;
    endMs = 0;
    log(now, announce ? "Game on - %s" : "Game started (%s)", DIFFICULTY_NAMES[difficulty]);
  }

  const char* addYou(int delta, uint32_t now) {
    if (phase == OVER) return "Game over. Start a new one";
    if (delta < 0) {
      if (you == 0) return nullptr;
      you--;
      log(now, "You -1 (%d)", you);
      return nullptr;
    }
    if (phase == READY) beginPlay(now, false);
    you++;
    log(now, "You scored (%d)", you);
    checkOver(now);
    return nullptr;
  }

  const char* setHitByHand(int i, bool on, uint32_t now) {
    Spot& s = spots[i];
    if (on == s.hit) return nullptr;
    if (on) {
      if (phase == OVER) return "Game over. Start a new one";
      s.setHit(true);
      lastHitMs[i] = now;
      if (phase == READY) beginPlay(now, false);
      log(now, "Cup %d marked hit by hand", i + 1);
      checkOver(now);
    } else {
      s.setHit(false);
      log(now, "Cup %d hit removed", i + 1);
      // Undoing a false hit that ended the game puts the game back on.
      if (phase == OVER && winner == 'R' && robot() < target(now)) {
        phase = PLAYING;
        winner = 0;
        endMs = 0;
        if (historyCount) {
          historyHead = (historyHead + MAX_HISTORY - 1) % MAX_HISTORY;
          historyCount--;
        }
      }
    }
    return nullptr;
  }

  void checkOver(uint32_t now) {
    if (phase != PLAYING) return;
    uint8_t t = cupsInPlay(now);
    if (!t) return;
    uint8_t r = robot();
    if (r >= t) finish(now, 'R');
    else if (you >= t) finish(now, 'Y');
  }

  void finish(uint32_t now, char who) {
    phase = OVER;
    winner = who;
    endMs = now;
    GameRecord& r = history[historyHead];
    r.difficulty = difficulty;
    r.robot = robot();
    r.you = you;
    r.winner = who;
    r.seconds = (uint16_t)((now - startMs) / 1000);
    historyHead = (historyHead + 1) % MAX_HISTORY;
    if (historyCount < MAX_HISTORY) historyCount++;
    if (who == 'R') log(now, "Robot wins %d-%d", robot(), you);
    else log(now, "You win %d-%d", you, robot());
  }
};

}  // namespace splash
