// Laptop tests for the tray's hit detection and game logic (tray/detect.h, tray/game.h).
// Simulates HX711 readings at 10 per second with noise, sloshing, lifts and refills.
//   c++ -std=c++17 -I tray tests/test_tray.cpp -o build/test_tray && build/test_tray
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "game.h"

using namespace splash;

static int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("  FAIL line %d: %s  ", __LINE__, #cond);    \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

const long OFFSET0 = 84213;    // what an empty spot reads
const float COUNTS = 2100.0f;  // counts per gram for the simulated cell

struct Sim {
  Game game;
  std::mt19937 rng{42};
  float noiseG = 0.25f;
  uint32_t now = 0;
  float weight[NUM_SPOTS] = {};  // grams actually on each spot
  bool plugged[NUM_SPOTS] = {true, true, true, true, true, true};
  // slosh: decaying wobble added on top of the true weight
  float sloshAmp[NUM_SPOTS] = {};
  uint32_t sloshStart[NUM_SPOTS] = {};
  std::vector<std::string> events;

  Sim() {
    game.begin();
    for (auto& s : game.spots) s.factor = COUNTS;  // pretend calibrated
    for (auto& w : weight) w = 180.0f;              // cup + water
  }
  long raw(int i) {
    std::normal_distribution<float> n(0, noiseG);
    float t = (now - sloshStart[i]) / 1000.0f;
    float slosh = sloshAmp[i] * expf(-t / 0.35f) * sinf(2 * 3.14159f * 4.0f * t);
    return OFFSET0 + lroundf((weight[i] + slosh + n(rng)) * COUNTS);
  }
  // Advance time in 100 ms steps, feeding every plugged spot.
  void run(float seconds) {
    int steps = (int)lroundf(seconds * 10);
    for (int k = 0; k < steps; k++) {
      now += 100;
      for (int i = 0; i < NUM_SPOTS; i++)
        if (plugged[i]) game.onSample(i, raw(i), now);
      game.tick(now);
      while (const char* e = game.takeNewEvent()) events.push_back(e);
    }
  }
  void dropBall(int i, float gain = 2.7f) {
    weight[i] += gain;
    sloshAmp[i] = 3.0f;
    sloshStart[i] = now;
  }
  int count(const std::string& needle) const {
    int c = 0;
    for (auto& e : events) c += e.find(needle) != std::string::npos;
    return c;
  }
  std::string state() {
    static char buf[4096];
    game.stateJson(buf, sizeof buf, now);
    return buf;
  }
};

void test(const char* name, std::function<void()> body) {
  int before = failures;
  body();
  printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}

int main() {
  test("startup zeroes every spot and all 6 sensors show up", [] {
    Sim s;
    s.run(2);
    CHECK(s.game.cupsInPlay(s.now) == 6, "in play %d", s.game.cupsInPlay(s.now));
    for (auto& sp : s.game.spots) CHECK(fabsf(sp.grams) < 0.5f, "grams %.2f", sp.grams);
  });

  test("10 minutes of noise gives no hits", [] {
    Sim s;
    s.noiseG = 0.4f;
    s.run(600);
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
    CHECK(s.count("hit") == 0, "events: %d", s.count("hit"));
  });

  test("a ball (2.7 g with sloshing) is one hit within 1-3 s", [] {
    Sim s;
    s.run(2);
    s.dropBall(3);
    uint32_t t0 = s.now;
    uint32_t hitAt = 0;
    for (int k = 0; k < 50 && !hitAt; k++) {
      s.run(0.1f);
      if (s.game.spots[3].hit) hitAt = s.now;
    }
    CHECK(hitAt, "never hit");
    float secs = (hitAt - t0) / 1000.0f;
    CHECK(secs >= 1.0f && secs <= 3.0f, "hit after %.1f s", secs);
    s.run(10);
    CHECK(s.game.robot() == 1, "robot %d", s.game.robot());
    CHECK(s.count("Cup 4 hit") == 1, "hit events %d", s.count("Cup 4 hit"));
    CHECK(s.game.phase == PLAYING, "phase %d", s.game.phase);
  });

  test("a ball that splashes some water out (+1.8 g) still counts", [] {
    Sim s;
    s.run(2);
    s.dropBall(0, 1.8f);
    s.run(4);
    CHECK(s.game.spots[0].hit, "no hit");
  });

  test("a second ball in a cup that's already hit adds nothing", [] {
    Sim s;
    s.run(2);
    s.dropBall(1);
    s.run(4);
    s.dropBall(1);
    s.run(4);
    CHECK(s.game.robot() == 1, "robot %d", s.game.robot());
    CHECK(s.count("Cup 2 hit") == 1, "events %d", s.count("Cup 2 hit"));
  });

  test("picking a cup up and putting it back is not a hit", [] {
    Sim s;
    s.run(2);
    s.weight[2] = 0;
    s.run(3);
    CHECK(s.game.spots[2].lifted, "not lifted");
    s.weight[2] = 120;  // they drank some
    s.run(4);
    CHECK(!s.game.spots[2].lifted, "still lifted");
    s.run(5);
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
    CHECK(s.count("picked up") == 1 && s.count("put back") == 1, "events");
  });

  test("a hand pressing on a cup for a second is not a hit", [] {
    Sim s;
    s.run(2);
    s.weight[4] += 300;
    s.run(1);
    s.weight[4] -= 300;
    s.run(5);
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
  });

  test("a refill (+40 g) re-zeroes that cup instead of counting", [] {
    Sim s;
    s.run(2);
    s.weight[5] += 40;
    s.run(6);
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
    CHECK(s.count("re-zeroed") == 1, "rezero events %d", s.count("re-zeroed"));
    s.dropBall(5);
    s.run(4);
    CHECK(s.game.spots[5].hit, "ball after refill not counted");
  });

  test("slow drift (1 g over 10 minutes) gives no hits", [] {
    Sim s;
    s.run(2);
    for (int k = 0; k < 600; k++) {
      for (auto& w : s.weight) w += 1.0f / 600;
      s.run(1);
    }
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
  });

  test("all 6 hit = robot wins, then hits stop counting", [] {
    Sim s;
    s.run(2);
    char out[64];
    s.game.command("c=start", out, sizeof out, s.now);
    s.run(1);
    for (int i = 0; i < 6; i++) {
      s.dropBall(i);
      s.run(3);
    }
    CHECK(s.game.phase == OVER && s.game.winner == 'R', "phase %d winner %c", s.game.phase,
          s.game.winner);
    CHECK(s.count("Robot wins 6-0") == 1, "win event");
    std::string st = s.state();
    CHECK(st.find("\"hist\":[[1,6,0,\"R\",") != std::string::npos, "history: %s", st.c_str());
  });

  test("you +1 six times = you win", [] {
    Sim s;
    s.run(2);
    char out[128];
    for (int k = 0; k < 6; k++) s.game.command("c=you&v=1", out, sizeof out, s.now);
    CHECK(s.game.winner == 'Y', "winner %c", s.game.winner);
    CHECK(!s.game.command("c=you&v=1", out, sizeof out, s.now), "should refuse after game over");
  });

  test("un-marking a false hit that ended the game puts it back on", [] {
    Sim s;
    s.plugged[1] = s.plugged[2] = s.plugged[3] = s.plugged[4] = s.plugged[5] = false;  // 1-cell MVP
    s.run(2);
    CHECK(s.game.target(s.now) == 1, "target %d", s.game.target(s.now));
    s.dropBall(0);
    s.run(4);
    CHECK(s.game.phase == OVER, "phase %d", s.game.phase);
    char out[128];
    s.game.command("c=hit&s=1&v=0", out, sizeof out, s.now);
    CHECK(s.game.phase == PLAYING && s.game.robot() == 0, "phase %d", s.game.phase);
    CHECK(s.state().find("\"hist\":[]") != std::string::npos, "history kept");
  });

  test("start zeroes the scales and clears hits", [] {
    Sim s;
    s.run(2);
    s.dropBall(0);
    s.run(4);
    char out[64];
    s.game.command("c=start", out, sizeof out, s.now);
    s.run(1);
    CHECK(s.game.robot() == 0, "robot %d", s.game.robot());
    CHECK(fabsf(s.game.spots[0].grams) < 0.5f, "grams %.2f", s.game.spots[0].grams);
    CHECK(s.game.phase == PLAYING, "phase");
  });

  test("calibration finds counts per gram, even with the wires swapped", [] {
    Sim s;
    for (auto& sp : s.game.spots) sp.factor = 1000;  // wrong guess
    for (auto& w : s.weight) w = 0;                  // empty spots
    s.run(2);
    char out[128];
    CHECK(s.game.command("c=zero&s=2", out, sizeof out, s.now), "%s", out);
    s.run(1);
    s.weight[1] = 200;  // known 200 g weight
    s.run(2);
    CHECK(s.game.command("c=cal&s=2&v=20000", out, sizeof out, s.now), "%s", out);
    CHECK(fabsf(s.game.spots[1].factor - COUNTS) < 10, "factor %.1f", s.game.spots[1].factor);
    CHECK(s.game.settingsChanged, "settings not flagged");
    // reversed wiring: negative counts per gram
    Sim r;
    for (auto& w : r.weight) w = 0;
    r.run(2);
    for (int k = 0; k < 20; k++) {  // swap the sign by hand: weight goes down when loaded
      r.now += 100;
      r.game.onSample(0, OFFSET0 - lroundf(150 * COUNTS), r.now);
    }
    CHECK(r.game.command("c=cal&s=1&v=15000", out, sizeof out, r.now), "%s", out);
    CHECK(r.game.spots[0].factor < 0, "factor %.1f", r.game.spots[0].factor);
    CHECK(fabsf(r.game.spots[0].grams - 150) < 1, "grams %.1f", r.game.spots[0].grams);
  });

  test("calibrating with nothing on the spot is refused", [] {
    Sim s;
    for (auto& w : s.weight) w = 0;
    s.run(2);
    char out[128];
    CHECK(!s.game.command("c=cal&s=1&v=20000", out, sizeof out, s.now), "%s", out);
    CHECK(std::string(out).find("\"err\"") != std::string::npos, "%s", out);
  });

  test("an unplugged sensor drops out of play", [] {
    Sim s;
    s.run(2);
    s.plugged[5] = false;
    s.run(2);
    CHECK(s.game.cupsInPlay(s.now) == 5, "in play %d", s.game.cupsInPlay(s.now));
    CHECK(s.count("Cup 6 sensor lost") == 1, "lost event");
    CHECK(s.state().find("{\"p\":0") != std::string::npos, "state shows it missing");
  });

  test("threshold, difficulty and bar settings validate", [] {
    Sim s;
    char out[128];
    CHECK(s.game.command("c=thr&v=200", out, sizeof out, 0), "%s", out);
    CHECK(fabsf(s.game.cfg.hitThresholdG - 2.0f) < 1e-4, "thr");
    CHECK(!s.game.command("c=thr&v=9000", out, sizeof out, 0), "accepted 90 g");
    CHECK(s.game.command("c=dif&v=4", out, sizeof out, 0) && s.game.difficulty == 4, "dif");
    CHECK(!s.game.command("c=dif&v=7", out, sizeof out, 0), "accepted dif 7");
    CHECK(s.game.command("c=bar&v=2", out, sizeof out, 0) && s.game.bar == 2, "bar");
    CHECK(!s.game.command("c=bogus", out, sizeof out, 0), "accepted bogus");
  });

  test("state JSON stays inside the 3000-byte buffer with full logs", [] {
    Sim s;
    s.run(2);
    for (int g = 0; g < 10; g++) {  // fill history and events
      char out[64];
      s.game.command("c=start", out, sizeof out, s.now);
      s.run(1);
      for (int k = 0; k < 6; k++) s.game.command("c=you&v=1", out, sizeof out, s.now);
    }
    std::string st = s.state();
    CHECK(st.size() < 3000, "size %zu", st.size());
    printf("  (state JSON is %zu bytes)\n", st.size());
    FILE* f = fopen("build/state_sample.json", "w");
    if (f) {
      fputs(st.c_str(), f);
      fclose(f);
    }
  });

  printf(failures ? "\n%d check(s) failed\n" : "\nall tests passed\n", failures);
  return failures ? 1 : 0;
}
