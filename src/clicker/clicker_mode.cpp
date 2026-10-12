// clicker_mode.cpp - Dual click counter with persistence.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include "clicker_mode.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

static const char* NVS_CLICK = "clicker";
static const uint32_t HOLD_RESET_MS = 700;   // long-press to reset a side

static uint32_t clkLeft  = 0;
static uint32_t clkRight = 0;

// Press tracking for long-press detection (per side, independent of the global
// both-buttons gestures which are handled elsewhere).
static uint32_t lDownAt = 0; static bool lWasDown = false; static bool lResetFired = false;
static uint32_t rDownAt = 0; static bool rWasDown = false; static bool rResetFired = false;

// Brief visual feedback when a side changes.
static uint32_t lFlash = 0, rFlash = 0;
static uint32_t lResetFlash = 0, rResetFlash = 0;

static void clkLoad() {
    Preferences p;
    if (p.begin(NVS_CLICK, true)) {
        clkLeft  = p.getULong("left", 0);
        clkRight = p.getULong("right", 0);
        p.end();
    }
}
static void clkSaveLeft() {
    Preferences p; if (p.begin(NVS_CLICK, false)) { p.putULong("left", clkLeft); p.end(); }
}
static void clkSaveRight() {
    Preferences p; if (p.begin(NVS_CLICK, false)) { p.putULong("right", clkRight); p.end(); }
}

void clickerEnter() {
    fb.setRotation(0);
    clkLoad();
    lWasDown = rWasDown = false;
    lResetFired = rResetFired = false;
}
void clickerShutdown() {
    clkSaveLeft();
    clkSaveRight();
}

void clickerTick() {
    uint32_t now = millis();

    // ---- LEFT button ----
    if (btnL && !lWasDown) {            // press begins
        lWasDown = true; lDownAt = now; lResetFired = false;
    }
    if (btnL && lWasDown && !lResetFired && (now - lDownAt >= HOLD_RESET_MS)) {
        // long-press: reset this side
        clkLeft = 0; clkSaveLeft();
        lResetFired = true;
        lResetFlash = now;
    }
    if (!btnL && lWasDown) {            // release
        lWasDown = false;
        if (!lResetFired) {            // short tap -> increment
            clkLeft++;
            clkSaveLeft();
            lFlash = now;
        }
    }

    // ---- RIGHT button ----
    if (btnR && !rWasDown) {
        rWasDown = true; rDownAt = now; rResetFired = false;
    }
    if (btnR && rWasDown && !rResetFired && (now - rDownAt >= HOLD_RESET_MS)) {
        clkRight = 0; clkSaveRight();
        rResetFired = true;
        rResetFlash = now;
    }
    if (!btnR && rWasDown) {
        rWasDown = false;
        if (!rResetFired) {
            clkRight++;
            clkSaveRight();
            rFlash = now;
        }
    }
}

// Draw one counter panel (a vertical half). accent = its color.
static void drawPanel(int x0, int w, uint32_t value, uint16_t accent, uint16_t accentDark,
                      const char* label, bool resetFlash, bool held) {
    // panel background subtly tinted toward its accent when pressed
    uint16_t bg = held ? accentDark : TEAL_DEEP;
    fb.fillRect(x0, 18, w, H - 18, bg);

    // big number, centered in the panel
    char num[12];
    snprintf(num, sizeof(num), "%lu", (unsigned long)value);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(accent);
    // choose font size by digit count so big numbers still fit
    int len = strlen(num);
    int font = (len <= 3) ? 7 : (len <= 5) ? 6 : 4;
    fb.drawString(num, x0 + w / 2, 150, font);

    // label under the number
    fb.setTextColor(accent);
    fb.setTextDatum(MC_DATUM);
    fb.drawString(label, x0 + w / 2, 210, 2);

    // reset hint
    fb.setTextColor(accentDark);
    fb.drawString("hold = reset", x0 + w / 2, 250, 1);

    // reset confirmation flash
    if (resetFlash) {
        fb.setTextColor(LIME_PALE);
        fb.drawString("reset!", x0 + w / 2, 280, 2);
    }
}

void clickerDraw() {
    uint32_t now = millis();
    fb.fillSprite(TEAL_DEEP);

    int halfW = W / 2;

    bool lFlashOn = (now - lFlash) < 120;
    bool rFlashOn = (now - rFlash) < 120;
    bool lResetOn = (now - lResetFlash) < 600;
    bool rResetOn = (now - rResetFlash) < 600;

    // LEFT panel (teal), RIGHT panel (purple)
    drawPanel(0,      halfW,        clkLeft,  TEAL_LIGHT,   TEAL_DARK,
              "LEFT",  lResetOn, btnL);
    drawPanel(halfW,  W - halfW,    clkRight, PURPLE_LIGHT, PURPLE_DARK,
              "RIGHT", rResetOn, btnR);

    // brief brighten overlay on tap (drawn as a thin accent bar at the top of the panel)
    if (lFlashOn) fb.fillRect(0,     18, halfW,     4, LIME_PALE);
    if (rFlashOn) fb.fillRect(halfW, 18, W - halfW, 4, LIME_PALE);

    // center divider
    fb.drawFastVLine(halfW, 18, H - 18, PURPLE_DARK);

    // header on top
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("CLICK COUNTER", 4, 9, 1);
    // total on the right of the header
    char tot[20];
    snprintf(tot, sizeof(tot), "sum %lu", (unsigned long)(clkLeft + clkRight));
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(tot, W - 4, 9, 1);

    fb.pushSprite(0, 0);
}
