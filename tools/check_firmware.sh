#!/bin/bash
# Type-checks tray/tray.ino against the real UNO R4 WiFi core headers using the Mac's own
# clang (no Rosetta needed). It does not produce a binary; upload with the Arduino IDE.
# Usage: tools/check_firmware.sh [extra -D flags, e.g. -DUSE_LED_RINGS=1]
set -e
cd "$(dirname "$0")/.."
P=~/Library/Arduino15/packages/arduino/hardware/renesas_uno/1.6.0
T=~/Library/Arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4
V=$P/variants/UNOWIFIR4
L=$P/libraries
mkdir -p build
{ echo '#include <Arduino.h>'; echo '#line 1 "tray.ino"'; cat tray/tray.ino; } > build/tray_check.cpp
EXTRA_INC=()
for d in "$HOME/Documents/Arduino/libraries"/*/src "$HOME/Documents/Arduino/libraries"/*; do EXTRA_INC+=("-I$d"); done
xcrun clang++ -fsyntax-only --target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -std=gnu++17 -fno-rtti -fno-exceptions -fsigned-char -nostdinc -nostdinc++ \
  -Wall -Wno-main -Wno-unknown-pragmas -Wno-unknown-attributes -Wno-unused-command-line-argument \
  -isystem "$T/arm-none-eabi/include/c++/7.2.1" \
  -isystem "$T/arm-none-eabi/include/c++/7.2.1/arm-none-eabi/thumb/v7e-m/fpv4-sp/hard" \
  -isystem "$T/arm-none-eabi/include" \
  -isystem "$T/lib/gcc/arm-none-eabi/7.2.1/include" \
  -isystem "$T/lib/gcc/arm-none-eabi/7.2.1/include-fixed" \
  -DF_CPU=48000000 -DNO_USB -DBACKTRACE_SUPPORT -DARDUINO_UNOR4_WIFI -DARDUINO=10607 \
  -DARDUINO_UNOWIFIR4 -DARDUINO_ARCH_RENESAS_UNO -DARDUINO_ARCH_RENESAS -DARDUINO_FSP -D_XOPEN_SOURCE=700 \
  $(cat "$V/defines.txt") \
  -I"$P/cores/arduino" -I"$P/cores/arduino/api/deprecated" -I"$P/cores/arduino/api/deprecated-avr-comp" \
  -I"$V" -iprefix"$P" @"$V/includes.txt" \
  -I"$L/WiFiS3/src" -I"$L/EEPROM/src" -I"$L/BlockDevices" -I"$L/Arduino_LED_Matrix/src" -I"$L/Wire" -I"$L/SPI" \
  "${EXTRA_INC[@]}" -Itray "$@" build/tray_check.cpp
echo "tray.ino type-checks OK $*"
