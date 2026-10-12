// moon_mode.cpp - Moon phase dashboard (offline).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <math.h>
#include "moon_mode.h"
#include "../net_yield.h"
#include "../theme_colors.h"
#include "../sysclock/sysclock.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

// Synodic month (new moon to new moon), days.
static const double SYNODIC = 29.530588853;
// A known new moon reference: 2000-01-06 18:14 UTC = Julian Date 2451550.1.
static const double KNOWN_NEW_JD = 2451550.1;

static int    moonPage = 0;       // 0 = dial, 1 = details
static double moonAge  = 0;       // days since last new moon
static double moonIllum = 0;      // 0..1 illuminated fraction
static double moonPhase = 0;      // 0..1 position in cycle (0=new,0.5=full)

// Convert a unix time to Julian Date.
static double unixToJD(time_t t) {
    return (double)t / 86400.0 + 2440587.5;
}

static const char* phaseName(double phase) {
    // phase 0..1 around the cycle (0/1 = new, 0.5 = full)
    if (phase < 0.04 || phase > 0.96) return "New Moon";
    if (phase < 0.21) return "Waxing Crescent";
    if (phase < 0.29) return "First Quarter";
    if (phase < 0.46) return "Waxing Gibbous";
    if (phase < 0.54) return "Full Moon";
    if (phase < 0.71) return "Waning Gibbous";
    if (phase < 0.79) return "Last Quarter";
    return "Waning Crescent";
}

static void computeMoon() {
    time_t now = sysclockNow();
    if (now < 1700000000) {
        // Clock not set; fall back to the reference epoch so we still show
        // something sensible rather than garbage.
        now = 1704067200; // 2024-01-01
    }
    double jd = unixToJD(now);
    double daysSince = jd - KNOWN_NEW_JD;
    double cycles = daysSince / SYNODIC;
    double frac = cycles - floor(cycles);
    if (frac < 0) frac += 1.0;
    moonPhase = frac;
    moonAge = frac * SYNODIC;
    // Illuminated fraction: (1 - cos(2*pi*phase)) / 2
    moonIllum = (1.0 - cos(2.0 * M_PI * frac)) / 2.0;
}

static double daysUntilPhase(double targetFrac) {
    // days forward until the cycle fraction next equals targetFrac
    double cur = moonPhase;
    double d = targetFrac - cur;
    if (d < 0) d += 1.0;
    return d * SYNODIC;
}

void moonEnter() {
    fb.setRotation(0);
    moonPage = 0;
    computeMoon();
}
void moonShutdown() {}

void moonTick() {
    bool lEdge = btnL && !btnL_prev;
    bool rEdge = btnR && !btnR_prev;
    if (lEdge) moonPage ^= 1;
    if (rEdge) computeMoon();   // refresh
}

// Draw a moon disc with the correct illuminated portion for the phase.
// Pixel-art moon: rendered on a coarse cell grid so it reads like a placed
// sprite. Each cell is shaded by whether it's in sunlight, in the terminator
// band (dithered), or in shadow (earthshine). Maria (dark seas) and a few
// craters give it surface character. The lit/shadow split tracks moonPhase.
static const int MOON_CELL = 4;   // pixel size of each "art" cell

// A few maria + craters as normalized disc coords (cx,cy in [-1,1], radius).
// Maria are darker patches; craters are bright rings. Hand-placed to look moonish.
struct MoonFeature { float fx, fy, fr; uint8_t kind; };  // kind 0=maria,1=crater
static const MoonFeature MOON_FEATURES[] = {
    { -0.30f, -0.35f, 0.26f, 0 },  // Mare Imbrium-ish
    {  0.05f, -0.15f, 0.20f, 0 },  // Mare Serenitatis-ish
    {  0.30f,  0.10f, 0.18f, 0 },  // Mare Tranquillitatis-ish
    { -0.10f,  0.40f, 0.16f, 0 },  // Mare Nubium-ish
    {  0.40f, -0.45f, 0.10f, 0 },  // small mare
    { -0.45f,  0.18f, 0.07f, 1 },  // crater (Copernicus-ish)
    {  0.22f,  0.48f, 0.06f, 1 },  // crater (Tycho-ish) with rays
    {  0.50f,  0.35f, 0.05f, 1 },  // crater
};
static const int MOON_FEATURE_COUNT = sizeof(MOON_FEATURES)/sizeof(MOON_FEATURES[0]);

static void drawMoonDisc(int cx, int cy, int r) {
    // Terminator x-position as a fraction across the disc, from the phase.
    // cosTerm: +1 at new (shadow covers all), -1 at full (all lit).
    double cosTerm = cos(2.0 * M_PI * moonPhase);
    bool waxing = (moonPhase <= 0.5);

    for (int py = -r; py <= r; py += MOON_CELL) {
        for (int px = -r; px <= r; px += MOON_CELL) {
            // cell center in disc coords
            float ux = (px + MOON_CELL * 0.5f) / (float)r;
            float uy = (py + MOON_CELL * 0.5f) / (float)r;
            float d2 = ux * ux + uy * uy;
            if (d2 > 1.0f) continue;              // outside the disc
            int half = (int)(sqrt((double)(r * r - py * py)) + 0.5);
            int termX = (int)(cosTerm * half);

            // Is this cell lit, in shadow, or near the terminator?
            // Lit region: waxing -> x > termX ; waning -> x < -termX
            int xpix = px + MOON_CELL / 2;
            float litEdge = waxing ? (float)(xpix - termX) : (float)(-termX - xpix);
            bool lit    = litEdge > MOON_CELL;
            bool shadow = litEdge < -MOON_CELL;
            bool termband = !lit && !shadow;      // straddles the terminator

            // Base tone
            uint16_t col;
            if (shadow) {
                // earthshine: very dim, just a hint of the disc
                col = TEAL_DARK;
            } else if (termband) {
                // dithered edge: checkerboard between pale and dark
                bool on = (((px / MOON_CELL) + (py / MOON_CELL)) & 1) == 0;
                col = on ? LIME_PALE : TEAL_DARK;
            } else {
                col = LIME_PALE;                  // fully lit highland
            }

            // Surface features only show on the lit side (and faintly at term).
            if (lit || termband) {
                for (int f = 0; f < MOON_FEATURE_COUNT; f++) {
                    const MoonFeature& mf = MOON_FEATURES[f];
                    float ddx = ux - mf.fx, ddy = uy - mf.fy;
                    float fd = sqrtf(ddx * ddx + ddy * ddy);
                    if (mf.kind == 0) {
                        // maria: darker fill
                        if (fd < mf.fr) col = lit ? LIME : TEAL_DARK;
                    } else {
                        // crater: bright rim, darker floor
                        if (fd < mf.fr * 0.55f)      col = lit ? LIME : TEAL_DARK;
                        else if (fd < mf.fr)          col = LIME_PALE;
                    }
                }
            }

            // Draw the cell.
            fb.fillRect(cx + px, cy + py, MOON_CELL, MOON_CELL, col);
        }
    }

    // Crisp rim
    fb.drawCircle(cx, cy, r, TEAL_LIGHT);
    fb.drawCircle(cx, cy, r - 1, TEAL_DARK);
}

static void drawDial() {
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MOON", 4, 9, 1);

    drawMoonDisc(W / 2, 110, 56);

    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString(phaseName(moonPhase), W / 2, 190, 2);

    fb.setTextColor(TEAL_LIGHT);
    char b[24];
    snprintf(b, sizeof(b), "%d%% lit", (int)(moonIllum * 100 + 0.5));
    fb.drawString(b, W / 2, 214, 2);

    snprintf(b, sizeof(b), "age %.1f days", moonAge);
    fb.setTextColor(TEAL_DARK);
    fb.drawString(b, W / 2, 236, 1);

    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString("L: details", 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: refresh", W - 4, H - 7, 1);
}

static void drawDetails() {
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MOON DETAILS", 4, 9, 1);

    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
    char b[32]; int y = 32;
    snprintf(b, sizeof(b), "Phase: %s", phaseName(moonPhase));
    fb.drawString(b, 6, y, 1); y += 20;
    snprintf(b, sizeof(b), "Illumination: %d%%", (int)(moonIllum * 100 + 0.5));
    fb.drawString(b, 6, y, 1); y += 20;
    snprintf(b, sizeof(b), "Age: %.1f days", moonAge);
    fb.drawString(b, 6, y, 1); y += 20;
    snprintf(b, sizeof(b), "Cycle: %d%%", (int)(moonPhase * 100 + 0.5));
    fb.drawString(b, 6, y, 1); y += 28;

    fb.setTextColor(LIME);
    double toFull = daysUntilPhase(0.5);
    double toNew  = daysUntilPhase(0.0);
    snprintf(b, sizeof(b), "Next full: %.1f d", toFull);
    fb.drawString(b, 6, y, 1); y += 20;
    snprintf(b, sizeof(b), "Next new:  %.1f d", toNew);
    fb.drawString(b, 6, y, 1); y += 28;

    fb.setTextColor(TEAL_DARK);
    if (!sysclockValid())
        fb.drawString("clock not set; set time", 6, y, 1);
    else
        fb.drawString("computed from device clock", 6, y, 1);

    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L: back to dial", W / 2, H - 7, 1);
}

void moonDraw() {
    fb.fillSprite(TEAL_DEEP);
    if (moonPage == 0) drawDial();
    else               drawDetails();
    fb.pushSprite(0, 0);
}
