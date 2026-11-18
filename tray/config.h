// config.h - everything you might need to change on the tray, in one place.
#pragma once
#include <Arduino.h>

// ---------------- Wi-Fi ----------------
// By default the tray makes its own Wi-Fi network. Join it from your phone and open
// http://192.168.4.1 (no internet needed).
#define WIFI_SSID "SplashShot"
#define WIFI_PASS "waterpong"  // 8+ characters

// Set to 1 to join an existing network instead (e.g. a phone hotspot). The tray's
// address is then printed in the Serial Monitor at 115200 baud.
#define JOIN_EXISTING_WIFI 0

// ---------------- Load cells (pin map from the SplashShot Electrical Plan) ----------------
// Cup n uses HX711 #n. Cup 1 is the tip of the triangle (nearest the robot),
// cups 2-3 the middle row, 4-6 the back row, numbered left to right from the player's side.
// For the one-cell prototype, wire it as cup 1 (DT -> D2, SCK -> D8); the rest show as
// "no sensor" and the game plays to 1 cup.
const uint8_t HX_DT_PINS[6] = {2, 3, 4, 5, 6, 7};
const uint8_t HX_SCK_PINS[6] = {8, 12, 13, A0, A1, A2};

// Raw HX711 counts per gram before a cup is calibrated (a 1 kg bar cell is roughly 2100).
// Calibrate each cup from the app's Sensors tab; the result is saved on the board.
#define DEFAULT_COUNTS_PER_GRAM 2100.0f

// ---------------- Buttons ----------------
// One side to the pin, the other to GND. Unwired buttons are fine.
#define START_BUTTON_PIN 10
#define TARE_BUTTON_PIN 11

// ---------------- Lights ----------------
// A plain LED (with a 220-330 ohm resistor to GND) on A3 lights while any cup is hit.
// A3 is a spare in the pin map; set to 255 to turn this off.
#define HIT_LED_PIN A3

// The UNO R4 WiFi's own 12x8 LED matrix shows the cups: lit = still standing.
#define USE_LED_MATRIX 1

// Set to 1 once the WS2812B ring PCBs are wired (install "Adafruit NeoPixel" first).
#define USE_LED_RINGS 0
#define LED_RING_PIN 9
#define LEDS_PER_RING 8
#define RING_BRIGHTNESS 80  // out of 255; keeps the tray under ~1 A

// Set to 1 once the 2.42" SSD1309 OLED is wired on I2C (install "U8g2" first).
#define USE_OLED 0

// Prints each cup's weight to the Serial Monitor once a second (115200 baud).
#define SERIAL_DEBUG 1
