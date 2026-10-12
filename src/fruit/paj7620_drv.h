// paj7620_drv.h - Minimal self-contained PAJ7620U2 gesture sensor driver.
//
// The sensor rides the 4-pin JST on the bottom of the T-Display-S3
// (3V3 / GND / SDA / SCL). No external library needed: the full Seeed
// init register sequence (PAJ7620U2_20140305.asc) is embedded in the .cpp,
// so this module owns I2C init, hardware probing, and gesture polling.
//
// Design notes:
//  - probe() is cheap (~1 ms) and safe to call any time, so the firmware can
//    re-check for the sensor whenever the menu opens (hot-plug support).
//  - Reading the gesture flag register CLEARS it in hardware, so exactly one
//    consumer (fruit_mode) should poll paj7620ReadGesture().
#pragma once
#include <stdint.h>

// I2C pins for the bottom JST connector (matches the working sketch:
// Wire.begin(18, 17)). Override with build flags if the harness changes.
#ifndef PAJ7620_I2C_SDA
#define PAJ7620_I2C_SDA 18
#endif
#ifndef PAJ7620_I2C_SCL
#define PAJ7620_I2C_SCL 17
#endif

// 7-bit I2C address of the PAJ7620U2.
#define PAJ7620_I2C_ADDR 0x73

// Gesture flag bits as reported in bank-0 register 0x43 (reg 0x44 bit0 = wave).
enum : uint8_t {
    PAJ_GES_RIGHT    = 1 << 0,
    PAJ_GES_LEFT     = 1 << 1,
    PAJ_GES_UP       = 1 << 2,
    PAJ_GES_DOWN     = 1 << 3,
    PAJ_GES_FORWARD  = 1 << 4,   // push toward sensor
    PAJ_GES_BACKWARD = 1 << 5,   // pull away from sensor
    PAJ_GES_CW       = 1 << 6,   // clockwise circle
    PAJ_GES_CCW      = 1 << 7,   // counter-clockwise circle
};

// Begin I2C (idempotent) and check the part ID (0x20 0x76). Returns true if a
// PAJ7620U2 is present and answering. Does NOT run the init sequence.
bool paj7620Probe();

// Full gesture-mode initialization (wake + init register array). Returns true
// on success. Call once after a successful probe; safe to call again after a
// re-plug.
bool paj7620InitGesture();

// Poll and clear the gesture flags. Returns the PAJ_GES_* byte from reg 0x43
// (0 = no gesture since last read). wave (optional out) reports reg 0x44 bit0.
uint8_t paj7620ReadGesture(bool* wave = nullptr);
