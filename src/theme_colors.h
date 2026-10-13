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

// Runtime palette: 14 shades derived from 3 base colours. themeInit() fills
// it at boot - from this build's colours or the board's saved ones.
struct MantisPalette { uint16_t s[14]; };
extern MantisPalette g_theme;
void themeInit();

#define TEAL_DEEP      (g_theme.s[0])
#define TEAL_DARK      (g_theme.s[1])
#define TEAL           (g_theme.s[2])
#define TEAL_BRIGHT    (g_theme.s[3])
#define TEAL_LIGHT     (g_theme.s[4])
#define PURPLE_DEEP    (g_theme.s[5])
#define PURPLE_DARK    (g_theme.s[6])
#define PURPLE         (g_theme.s[7])
#define PURPLE_BRIGHT  (g_theme.s[8])
#define PURPLE_LIGHT   (g_theme.s[9])
#define LIME_DARK      (g_theme.s[10])
#define LIME_MED       (g_theme.s[11])
#define LIME           (g_theme.s[12])
#define LIME_PALE      (g_theme.s[13])

// Tokens for const tables (a table can't hold a runtime value without
// moving into RAM and freezing the boot-time colour): store TH_x, read via thc().
enum : uint16_t {
    TH_TEAL_DEEP = 1,
    TH_TEAL_DARK = 2,
    TH_TEAL = 3,
    TH_TEAL_BRIGHT = 4,
    TH_TEAL_LIGHT = 5,
    TH_PURPLE_DEEP = 6,
    TH_PURPLE_DARK = 7,
    TH_PURPLE = 8,
    TH_PURPLE_BRIGHT = 9,
    TH_PURPLE_LIGHT = 10,
    TH_LIME_DARK = 11,
    TH_LIME_MED = 12,
    TH_LIME = 13,
    TH_LIME_PALE = 14,
};
static inline uint16_t thc(uint16_t c) { return (c >= 1 && c <= 14) ? g_theme.s[c - 1] : c; }

#define THEME_EXPLICIT 0
#define THEME_NONCE    0xD501B38Du
#define THEME_DEFAULTS { \
    rgb565(  0,  34,  34), /* TEAL_DEEP     = rgb565(  0,  34,  34) */ \
    rgb565(  0,  60,  60), /* TEAL_DARK     = rgb565(  0,  60,  60) */ \
    rgb565(  0, 115, 115), /* TEAL          = rgb565(  0, 115, 115) */ \
    rgb565(  0, 179, 179), /* TEAL_BRIGHT   = rgb565(  0, 179, 179) */ \
    rgb565( 51, 227, 227), /* TEAL_LIGHT    = rgb565( 51, 227, 227) */ \
    rgb565( 25,   0,  25), /* PURPLE_DEEP   = rgb565( 25,   0,  25) */ \
    rgb565( 50,   0,  50), /* PURPLE_DARK   = rgb565( 50,   0,  50) */ \
    rgb565( 93,   0,  93), /* PURPLE        = rgb565( 93,   0,  93) */ \
    rgb565(150,   0, 150), /* PURPLE_BRIGHT = rgb565(150,   0, 150) */ \
    rgb565(226,  51, 226), /* PURPLE_LIGHT  = rgb565(226,  51, 226) */ \
    rgb565(  0,  79,   0), /* LIME_DARK     = rgb565(  0,  79,   0) */ \
    rgb565(  0, 161,   0), /* LIME_MED      = rgb565(  0, 161,   0) */ \
    rgb565(  0, 255,   0), /* LIME          = rgb565(  0, 255,   0) */ \
    rgb565(178, 255, 178), /* LIME_PALE     = rgb565(178, 255, 178) */ \
}
#define INTRO_URL "kylermichaelson.carrd.co"
