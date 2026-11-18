// detect.h - weight tracking and hit detection for one cup spot on the SplashShot tray.
//
// Plain C++ with no Arduino calls, so the same code runs on the tray and in the
// laptop tests (tests/test_tray.cpp). Every reading comes in through addSample().
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

namespace splash {

struct DetectConfig {
  float hitThresholdG = 1.5f;  // a settled gain above this counts as a ball (a ball is 2.7 g)
  float maxBallG = 15.0f;      // bigger gains are a hand or a refill, never a ball
  float liftG = 40.0f;         // a drop this big means the cup was picked up
  float backG = 15.0f;         // weight rising this far above the lifted level = cup put back
  float maxSpreadG = 5.0f;     // the last second must be at least this steady to count a hit
  uint32_t hitHoldMs = 1000;   // the gain has to last this long (lets the water settle)
  uint32_t liftHoldMs = 300;
  uint32_t backHoldMs = 1500;
  uint32_t bigHoldMs = 3000;
  uint32_t lostMs = 1000;      // no reading for this long = sensor unplugged
};

enum class SpotEvent : uint8_t { None, Hit, Lifted, Back, Rezeroed };

class Spot {
 public:
  static const uint8_t WIN = 10;  // the last second of readings (an HX711 gives 10 per second)
  static const uint8_t FILT = 5;  // grams = average of the last half second

  const DetectConfig* cfg = nullptr;
  long offset = 0;         // raw reading that means 0 g
  float factor = 2100.0f;  // raw counts per gram; replaced by calibration
  bool calibrated = false;

  bool hit = false;     // a ball landed in this cup this game
  bool lifted = false;  // the cup is off the scale
  float grams = 0;      // weight change since the last zero, averaged
  float spread = 0;     // max - min over the last second: noise or sloshing
  float baseline = 0;   // grams with no new ball in the cup
  float hitGain = 0;    // how much the hit added

  bool present(uint32_t now) const { return n > 0 && (uint32_t)(now - lastMs) < cfg->lostMs; }
  bool zeroed() const { return !tarePending; }
  float change() const { return grams - baseline; }

  // Zero this spot using the next half second of readings.
  void tare() {
    tarePending = true;
    fresh = 0;
  }

  // Mark or clear a hit by hand. Either way, the current weight becomes the new normal.
  void setHit(bool on) {
    hit = on;
    if (!on) hitGain = 0;
    baseline = grams;
    aboveSince = 0;
  }

  // Call with a known weight sitting on a spot that was zeroed empty.
  bool calibrate(float knownGrams) {
    if (tarePending || n < FILT || knownGrams < 5.0f) return false;
    long diff = rawMean(n) - offset;
    if (labs(diff) < 500) return false;  // nothing on it
    factor = (float)diff / knownGrams;
    calibrated = true;
    recompute();
    baseline = grams;
    clearTimers();
    return true;
  }

  // Feed one HX711 reading. allowHit = false while the game is over.
  SpotEvent addSample(long raw, uint32_t now, bool allowHit) {
    buf[head] = raw;
    head = (head + 1) % WIN;
    if (n < WIN) n++;
    lastMs = now;

    if (tarePending) {
      if (++fresh < FILT) return SpotEvent::None;
      offset = rawMean(FILT);
      tarePending = false;
      recompute();
      baseline = grams;
      lifted = false;
      clearTimers();
      return SpotEvent::None;
    }

    recompute();
    float d = grams - baseline;

    if (lifted) {
      if (grams < liftedLevel) liftedLevel = grams;
      bool settledBack = grams > liftedLevel + cfg->backG && spread < cfg->maxSpreadG;
      if (held(backSince, settledBack, now, cfg->backHoldMs)) {
        lifted = false;
        baseline = grams;
        clearTimers();
        return SpotEvent::Back;
      }
      return SpotEvent::None;
    }

    if (held(liftSince, d < -cfg->liftG, now, cfg->liftHoldMs)) {
      lifted = true;
      liftedLevel = grams;
      clearTimers();
      return SpotEvent::Lifted;
    }

    bool ballSized = d > cfg->hitThresholdG && d < cfg->maxBallG;
    if (held(aboveSince, ballSized, now, cfg->hitHoldMs) && !hit && allowHit &&
        spread < cfg->maxSpreadG) {
      hit = true;
      hitGain = d;
      baseline = grams;
      aboveSince = 0;
      return SpotEvent::Hit;
    }

    bool bigSteady = d >= cfg->maxBallG && spread < cfg->maxSpreadG;
    if (held(bigSince, bigSteady, now, cfg->bigHoldMs)) {
      baseline = grams;
      bigSince = 0;
      return SpotEvent::Rezeroed;
    }

    // Follow slow drift (load-cell creep, evaporation) but never a ball-sized step.
    if (fabsf(d) < cfg->hitThresholdG * 0.3f) baseline += 0.02f * d;
    return SpotEvent::None;
  }

 private:
  long buf[WIN] = {};
  uint8_t head = 0, n = 0, fresh = 0;
  bool tarePending = true;
  uint32_t lastMs = 0;
  uint32_t aboveSince = 0, liftSince = 0, backSince = 0, bigSince = 0;
  float liftedLevel = 0;

  long rawMean(uint8_t k) const {
    if (n == 0) return offset;
    if (k > n) k = n;
    int64_t sum = 0;
    for (uint8_t i = 0; i < k; i++) sum += buf[(head + WIN - 1 - i) % WIN];
    return (long)(sum / k);
  }

  void recompute() {
    float f = factor == 0 ? 1.0f : factor;
    grams = (float)(rawMean(FILT) - offset) / f;
    long lo = buf[0], hi = buf[0];
    for (uint8_t i = 0; i < n; i++) {
      long v = buf[(head + WIN - 1 - i) % WIN];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    spread = fabsf((float)(hi - lo) / f);
  }

  void clearTimers() { aboveSince = liftSince = backSince = bigSince = 0; }

  // True once `cond` has held continuously for holdMs. Stores start time + 1 so 0 means "not started".
  static bool held(uint32_t& since, bool cond, uint32_t now, uint32_t holdMs) {
    if (!cond) {
      since = 0;
      return false;
    }
    if (!since) since = now + 1;
    return (uint32_t)(now + 1 - since) >= holdMs;
  }
};

}  // namespace splash
