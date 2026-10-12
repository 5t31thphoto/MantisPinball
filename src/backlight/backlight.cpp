// backlight.cpp - LEDC PWM brightness for the T-Display-S3 backlight.
#include <Arduino.h>
#include <Preferences.h>
#include "backlight.h"

#ifndef TFT_BL
#define TFT_BL 38
#endif

// Arduino-ESP32 core 2.0.x LEDC API (ledcSetup/ledcAttachPin/ledcWrite).
static const int   BL_CH   = 7;        // LEDC channel (high channel, unlikely to clash)
static const int   BL_FREQ = 20000;    // 20kHz: above audible, no flicker
static const int   BL_RES  = 8;        // 8-bit duty (0..255)

// Duty for each level. "Off" is not truly off - it's ridiculously dim so the
// tank is still faintly visible (a true-off looks broken). The rest are gentle
// steps; the panel is bright, so even "mid" is usable indoors.
static const uint8_t BL_DUTY[4] = { 3, 40, 130, 255 };

static uint8_t blLevel = 3;            // default bright
static bool    blReady = false;

static void blApply() {
    if (!blReady) return;
    ledcWrite(BL_CH, BL_DUTY[blLevel & 3]);
}

void backlightBegin() {
    // Persisted level (so the device remembers your brightness across reboots).
    Preferences p;
    if (p.begin("backlight", true)) { blLevel = p.getUChar("lvl", 3); p.end(); }
    if (blLevel > 3) blLevel = 3;
    ledcSetup(BL_CH, BL_FREQ, BL_RES);
    ledcAttachPin(TFT_BL, BL_CH);
    blReady = true;
    blApply();
}

void backlightSetLevel(uint8_t level) {
    blLevel = level & 3;
    blApply();
    Preferences p;
    if (p.begin("backlight", false)) { p.putUChar("lvl", blLevel); p.end(); }
}

uint8_t backlightLevel() { return blLevel; }

void backlightCycle() {
    backlightSetLevel((blLevel + 1) & 3);
}
