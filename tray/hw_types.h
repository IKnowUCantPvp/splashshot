// hw_types.h - types used in tray.ino function signatures.
// They live here because the Arduino IDE writes function prototypes near the top of the
// .ino, above anything defined in it; a type defined in the .ino itself would come too late.
#pragma once
#include <stdint.h>

struct Button {
  uint8_t pin;
  bool reading, stable;
  uint32_t changedMs;
};

// What a cup's lights should show.
enum CupLook : uint8_t { OFF, STANDING, HIT, LIFTED };
