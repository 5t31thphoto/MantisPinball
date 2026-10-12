// theme_colors.h - GENERATED. Edit via tools/intro_designer.py
#pragma once
#include <stdint.h>

// rgb565 color packing helper - must appear before all color constants
#ifndef RGB565_DEFINED
#define RGB565_DEFINED
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
#endif

static constexpr uint16_t TEAL_DEEP     = rgb565(  0,  34,  34);
static constexpr uint16_t TEAL_DARK     = rgb565(  0,  60,  60);
static constexpr uint16_t TEAL          = rgb565(  0, 115, 115);
static constexpr uint16_t TEAL_BRIGHT   = rgb565(  0, 179, 179);
static constexpr uint16_t TEAL_LIGHT    = rgb565( 51, 227, 227);
static constexpr uint16_t PURPLE_DEEP   = rgb565( 25,   0,  25);
static constexpr uint16_t PURPLE_DARK   = rgb565( 50,   0,  50);
static constexpr uint16_t PURPLE        = rgb565( 93,   0,  93);
static constexpr uint16_t PURPLE_BRIGHT = rgb565(150,   0, 150);
static constexpr uint16_t PURPLE_LIGHT  = rgb565(226,  51, 226);
static constexpr uint16_t LIME_DARK     = rgb565(  0,  79,   0);
static constexpr uint16_t LIME_MED      = rgb565(  0, 161,   0);
static constexpr uint16_t LIME          = rgb565(  0, 255,   0);
static constexpr uint16_t LIME_PALE     = rgb565(178, 255, 178);

#define INTRO_URL "kylermichaelson.carrd.co"
