// fruit_mode.cpp - FRUIT FRENZY (gesture fruit slicing, PAJ7620U2).
//
// Non-blocking port of the original landscape sketch to the Pinball-S3 app
// framework: portrait 170x320, shared fb sprite, enter/tick/draw/shutdown
// lifecycle, no delay() calls (flashes/slashes/particles are all timers).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <math.h>
#include "fruit_mode.h"
#include "paj7620_drv.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

// ---------------------------------------------------------------------------
// Sensor detection (menu gating). main.cpp probes at boot + every menu open.
// ---------------------------------------------------------------------------
static bool sensorPresent = false;
static bool sensorInited  = false;

void fruitProbeSensor() {
    bool now = paj7620Probe();
    if (now && !sensorInited) {
        // First sighting (or re-plug after an unplug): run the full init.
        sensorInited = paj7620InitGesture();
        now = sensorInited;
    }
    if (!now) sensorInited = false;   // unplugged: force re-init on return
    sensorPresent = now;
}
bool fruitSensorPresent() { return sensorPresent; }

// ---------------------------------------------------------------------------
// Gesture orientation remap. If the sensor is mounted rotated on the JST
// pigtail, swap these so on-screen slashes match physical swipes.
// ---------------------------------------------------------------------------
static constexpr uint8_t G_LEFT  = PAJ_GES_LEFT;
static constexpr uint8_t G_RIGHT = PAJ_GES_RIGHT;
static constexpr uint8_t G_UP    = PAJ_GES_UP;
static constexpr uint8_t G_DOWN  = PAJ_GES_DOWN;

// ---------------------------------------------------------------------------
// Game constants
// ---------------------------------------------------------------------------
static constexpr int   PLAY_TOP = 22;          // below HUD bar
static constexpr int   PLAY_BOT = H - 16;      // above footer bar
static constexpr int   RET_X = W / 2;          // reticle center
static constexpr int   RET_Y = (PLAY_TOP + PLAY_BOT) / 2;
static constexpr float TOL   = 26.0f;          // slash hit tolerance (px)
static float tolNow = TOL;                     // per-game (Wide Blade widens it)
static constexpr float ANGLE_STEP_DEG = 15.0f; // reticle snap step
static constexpr uint32_t FRENZY_MS   = 5000;
static constexpr uint32_t ROT_COOLDOWN_MS = 220;  // between reticle snaps

enum FruitScreen { FR_TITLE, FR_PLAY, FR_OVER };
enum FruitMode   { FR_ARCADE, FR_ZEN };
enum ItemType    { IT_ORANGE, IT_BANANA, IT_MELON, IT_SLIME, IT_FRENZY };

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static FruitScreen screen = FR_TITLE;
static FruitMode   mode   = FR_ARCADE;
static int   titleSel = 0;                 // 0 = arcade, 1 = zen

static int      score = 0;
static int      lives = 3;
static int      combo = 0;
static uint32_t hiArcade = 0, hiZen = 0;
static bool     hiLoaded = false;
static bool     newHigh = false;

static int   angleIdx = 0;                 // -1 / 0 / +1  ->  -15 / 0 / +15 deg
static uint32_t lastRotAt = 0;

static ItemType itemType = IT_ORANGE;
static float itemX = RET_X, itemY = PLAY_TOP;
static float itemSpeed = 1.6f;
static bool  itemActive = false;

static bool     frenzy = false;
static uint32_t frenzyEnd = 0;

// Transient FX (all timer-driven, no delay())
static uint32_t slashUntil = 0;            // slash streak visible until
static int      slashAxis  = 0;            // 0 = horizontal axis, 1 = vertical
static uint16_t slashColor = TFT_WHITE;
static uint32_t flashUntil = 0;            // border flash
static uint16_t flashColor = 0;

struct Particle { float x, y, vx, vy; uint32_t die; uint16_t col; };
static Particle parts[14];

struct Popup { float x, y; uint32_t die; char txt[12]; uint16_t col; };
static Popup popup = { 0, 0, 0, "", TFT_WHITE };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline float degRad(float d) { return d * 0.017453293f; }

// Current axis direction vectors from the reticle angle.
static void axisDirs(float& hx, float& hy, float& vx, float& vy) {
    float th = degRad(angleIdx * ANGLE_STEP_DEG);
    hx = cosf(th); hy = sinf(th);      // "horizontal" slash axis
    vx = -sinf(th); vy = cosf(th);     // "vertical" slash axis
}

static int multNow() { return 1 + min(combo, 15) / 5; }   // x1..x4

static void spawnParticles(float x, float y, uint16_t col) {
    uint32_t now = millis();
    for (int i = 0; i < 14; i++) {
        float a = (float)(esp_random() % 628) / 100.0f;
        float sp = 0.8f + (float)(esp_random() % 100) / 45.0f;
        parts[i] = { x, y, cosf(a) * sp, sinf(a) * sp - 0.6f,
                     now + 300 + (esp_random() % 250), col };
    }
}

static void showPopup(float x, float y, const char* txt, uint16_t col) {
    popup.x = x; popup.y = y;
    popup.die = millis() + 700;
    popup.col = col;
    strncpy(popup.txt, txt, sizeof(popup.txt) - 1);
    popup.txt[sizeof(popup.txt) - 1] = 0;
}

static void borderFlash(uint16_t col, uint32_t ms) {
    flashColor = col; flashUntil = millis() + ms;
}

static void loadHighs() {
    if (hiLoaded) return;
    Preferences p;
    if (p.begin("fruit", true)) {                 // own namespace, read-only
        hiArcade = p.getULong("hi_arc", 0);
        hiZen    = p.getULong("hi_zen", 0);
        p.end();
    }
    hiLoaded = true;
}

static void saveHigh() {
    uint32_t& hi = (mode == FR_ARCADE) ? hiArcade : hiZen;
    if ((uint32_t)score <= hi) return;
    hi = (uint32_t)score;
    newHigh = true;
    Preferences p;
    if (p.begin("fruit", false)) {
        p.putULong((mode == FR_ARCADE) ? "hi_arc" : "hi_zen", hi);
        p.end();
    }
}

// ---------------------------------------------------------------------------
// Spawning / round flow
// ---------------------------------------------------------------------------
static void spawnItem() {
    itemX = (float)(25 + (int)(esp_random() % (W - 50)));
    itemY = PLAY_TOP + 4;
    itemActive = true;

    float diff = 1.0f + ((float)score / 100.0f) * 0.04f;   // sketch's ramp
    int roll = (int)(esp_random() % 100);

    if (mode == FR_ZEN) {
        if (roll < 45)      { itemType = IT_ORANGE; itemSpeed = 1.4f * diff; }
        else if (roll < 75) { itemType = IT_BANANA; itemSpeed = 2.4f * diff; }
        else                { itemType = IT_MELON;  itemSpeed = 4.2f * diff;
                              borderFlash(TFT_GREEN, 120); }
    } else {
        if (roll < 35)      { itemType = IT_ORANGE; itemSpeed = 1.4f * diff; }
        else if (roll < 60) { itemType = IT_BANANA; itemSpeed = 2.4f * diff; }
        else if (roll < 75) { itemType = IT_MELON;  itemSpeed = 4.2f * diff;
                              borderFlash(TFT_DARKGREEN, 120); }
        else if (roll < 92) { itemType = IT_SLIME;  itemSpeed = 2.9f * diff; }
        else                { itemType = IT_FRENZY; itemSpeed = 3.4f * diff; }
    }
}

static bool perkWide = false, perkLife = false, perkFrenzy = false;

static void startGame(FruitMode m) {
    mode = m;
    score = 0; lives = 3; combo = 0;
    angleIdx = 0;
    frenzy = false; newHigh = false;
    // Items ARMED in the Quest inventory land at the start of this game.
    perkWide   = qpTakePending(ITEM_FR_WIDE)   > 0;
    perkLife   = qpTakePending(ITEM_FR_LIFE)   > 0;
    perkFrenzy = qpTakePending(ITEM_FR_FRENZY) > 0;
    tolNow = perkWide ? TOL * 1.6f : TOL;
    if (perkLife && m == FR_ARCADE) lives = 4;
    if (perkFrenzy) { frenzy = true; frenzyEnd = millis() + 8000; }
    itemActive = false;
    slashUntil = flashUntil = 0;
    popup.die = 0;
    for (auto& p : parts) p.die = 0;
    screen = FR_PLAY;
}

static void endGame() {
    saveHigh();
    screen = FR_OVER;
}

// ---------------------------------------------------------------------------
// Slice resolution
// ---------------------------------------------------------------------------
static void resolveHit() {
    int base = 0;
    uint16_t col = TFT_WHITE;
    bool comboUp = true;

    switch (itemType) {
        case IT_ORANGE: base = 10; col = TFT_ORANGE; break;
        case IT_BANANA: base = 25; col = TFT_YELLOW; break;
        case IT_MELON:  base = 50; col = TFT_GREEN;  break;
        case IT_FRENZY:
            frenzy = true; frenzyEnd = millis() + FRENZY_MS;
            col = TFT_CYAN; comboUp = true; base = 0;
            showPopup(itemX, itemY, "FRENZY!", TFT_CYAN);
            break;
        case IT_SLIME: {
            // Slicing the hazard hurts: points lost, big flash, combo gone.
            score = max(0, score - 30);
            if (mode == FR_ARCADE) lives -= 2;
            combo = 0; comboUp = false;
            col = TFT_MAGENTA;
            borderFlash(TFT_MAGENTA, 160);
            showPopup(itemX, itemY, "-30", TFT_MAGENTA);
            break;
        }
    }

    if (base > 0) {
        int m = multNow();
        int pts = base * m * (frenzy ? 2 : 1);
        score += pts;
        char t[12];
        if (m > 1 || frenzy) snprintf(t, sizeof(t), "+%d x%d", base, m * (frenzy ? 2 : 1));
        else                 snprintf(t, sizeof(t), "+%d", pts);
        showPopup(itemX, itemY, t, col);
    }
    if (comboUp) combo++;

    spawnParticles(itemX, itemY, col);
    itemActive = false;

    if (mode == FR_ARCADE && lives <= 0) endGame();
}

// Push-away: hazards only. Points + combo kept; the risky-but-rewarding play.
static void resolvePush() {
    if (!itemActive || itemType != IT_SLIME) return;
    int pts = 15 * multNow() * (frenzy ? 2 : 1);
    score += pts;
    combo++;
    char t[12]; snprintf(t, sizeof(t), "+%d", pts);
    showPopup(itemX, itemY, t, TEAL_LIGHT);
    spawnParticles(itemX, itemY, PURPLE_LIGHT);
    itemActive = false;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
static void tickTitle(uint8_t g, bool lEdge, bool rEdge) {
    bool moved = false;
    if ((g & G_UP) || (g & G_DOWN) || lEdge) { titleSel ^= 1; moved = true; }
    (void)moved;
    if ((g & PAJ_GES_FORWARD) || rEdge) {
        startGame(titleSel == 0 ? FR_ARCADE : FR_ZEN);
    }
}

static void tickPlay(uint8_t g, bool lEdge, bool rEdge) {
    uint32_t now = millis();

    if (frenzy && now > frenzyEnd) frenzy = false;

    // ---- reticle rotation: CW/CCW gestures snap +/-15deg; buttons mirror it
    bool wantCW  = (g & PAJ_GES_CW)  || rEdge;
    bool wantCCW = (g & PAJ_GES_CCW) || lEdge;
    if ((wantCW || wantCCW) && now - lastRotAt > ROT_COOLDOWN_MS) {
        if (wantCW  && angleIdx < 1)  { angleIdx++; lastRotAt = now; }
        if (wantCCW && angleIdx > -1) { angleIdx--; lastRotAt = now; }
    }

    // ---- push-away (hazard defense)
    if (g & PAJ_GES_FORWARD) resolvePush();

    // ---- slashes
    if (itemActive) {
        float hx, hy, vx, vy;
        axisDirs(hx, hy, vx, vy);
        float dx = itemX - RET_X, dy = itemY - RET_Y;

        if ((g & G_LEFT) || (g & G_RIGHT)) {
            // distance from item to the horizontal axis line = |cross(d, h)|
            float dist = fabsf(dx * hy - dy * hx);
            slashAxis = 0; slashUntil = now + 130;
            slashColor = frenzy ? TFT_CYAN : TFT_RED;
            if (dist <= tolNow) resolveHit();
        } else if ((g & G_UP) || (g & G_DOWN)) {
            float dist = fabsf(dx * vy - dy * vx);
            slashAxis = 1; slashUntil = now + 130;
            slashColor = frenzy ? TFT_CYAN : TFT_RED;
            if (dist <= tolNow) resolveHit();
        }
    }

    // ---- fall / spawn
    if (screen != FR_PLAY) return;   // resolveHit may have ended the game
    if (itemActive) {
        itemY += itemSpeed;
        if (itemY > PLAY_BOT + 20) {
            itemActive = false;
            if (itemType == IT_SLIME || itemType == IT_FRENZY) {
                combo = 0;               // safe, but the streak is gone
            } else {
                combo = 0;
                if (mode == FR_ARCADE) {
                    lives--;
                    borderFlash(TFT_RED, 140);
                    if (lives <= 0) { endGame(); return; }
                }
            }
        }
    } else {
        spawnItem();
    }
}

static void tickOver(uint8_t g, bool lEdge, bool rEdge) {
    if ((g & PAJ_GES_FORWARD) || rEdge || lEdge) screen = FR_TITLE;
}

void fruitTick() {
    bool lEdge = btnL && !btnL_prev;
    bool rEdge = btnR && !btnR_prev;

    // One poll per frame; reading clears the sensor's flags.
    uint8_t g = sensorInited ? paj7620ReadGesture() : 0;

    switch (screen) {
        case FR_TITLE: tickTitle(g, lEdge, rEdge); break;
        case FR_PLAY:  tickPlay(g, lEdge, rEdge);  break;
        case FR_OVER:  tickOver(g, lEdge, rEdge);  break;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawDashed(float x0, float y0, float x1, float y1,
                       int dash, uint16_t col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    for (float i = 0; i < len; i += dash * 2) {
        float t1 = i / len, t2 = min(i + (float)dash, len) / len;
        fb.drawLine((int)(x0 + dx * t1), (int)(y0 + dy * t1),
                    (int)(x0 + dx * t2), (int)(y0 + dy * t2), col);
    }
}

static void drawReticle() {
    float hx, hy, vx, vy;
    axisDirs(hx, hy, vx, vy);
    const float L = 200.0f;   // long enough to span the screen; clipped free
    drawDashed(RET_X - hx * L, RET_Y - hy * L, RET_X + hx * L, RET_Y + hy * L,
               4, TEAL_DARK);
    drawDashed(RET_X - vx * L, RET_Y - vy * L, RET_X + vx * L, RET_Y + vy * L,
               4, TEAL_DARK);
    // tiny angle marker at the crosshair
    fb.drawCircle(RET_X, RET_Y, 3, angleIdx == 0 ? TEAL : LIME_DARK);
}

static void drawSlash() {
    if (millis() >= slashUntil) return;
    float hx, hy, vx, vy;
    axisDirs(hx, hy, vx, vy);
    float dxx = slashAxis == 0 ? hx : vx;
    float dyy = slashAxis == 0 ? hy : vy;
    const float L = 200.0f;
    int ax = RET_X - (int)(dxx * L), ay = RET_Y - (int)(dyy * L);
    int bx = RET_X + (int)(dxx * L), by = RET_Y + (int)(dyy * L);
    fb.drawLine(ax, ay - 1, bx, by - 1, slashColor);
    fb.drawLine(ax, ay,     bx, by,     TFT_WHITE);
    fb.drawLine(ax, ay + 1, bx, by + 1, slashColor);
}

static void drawItem() {
    if (!itemActive) return;
    int x = (int)itemX, y = (int)itemY;
    switch (itemType) {
        case IT_ORANGE:
            fb.fillCircle(x, y, 9, TFT_ORANGE);
            fb.fillCircle(x - 3, y - 3, 2, rgb565(255, 200, 120));
            break;
        case IT_BANANA:
            fb.fillEllipse(x, y, 15, 6, TFT_YELLOW);
            fb.drawEllipse(x, y, 15, 6, rgb565(160, 130, 0));
            break;
        case IT_MELON:
            fb.fillCircle(x, y, 20, TFT_GREEN);
            fb.drawCircle(x, y, 20, TFT_DARKGREEN);
            fb.drawCircle(x, y, 14, TFT_DARKGREEN);
            break;
        case IT_SLIME:
            fb.fillCircle(x, y, 12, PURPLE_BRIGHT);
            fb.fillCircle(x - 4, y - 3, 2, PURPLE_LIGHT);
            fb.fillCircle(x + 4, y - 3, 2, PURPLE_LIGHT);
            // little "danger" spikes
            for (int i = 0; i < 6; i++) {
                float a = degRad(60.0f * i + (millis() / 40) % 60);
                fb.drawLine(x + (int)(cosf(a) * 12), y + (int)(sinf(a) * 12),
                            x + (int)(cosf(a) * 16), y + (int)(sinf(a) * 16),
                            PURPLE);
            }
            break;
        case IT_FRENZY:
            fb.fillCircle(x, y, 8, TFT_CYAN);
            fb.drawCircle(x, y, 11 + (int)((millis() / 90) % 3), TEAL_LIGHT);
            break;
    }
}

static void drawParticlesAndPopup() {
    uint32_t now = millis();
    for (auto& p : parts) {
        if (now >= p.die) continue;
        p.x += p.vx; p.y += p.vy; p.vy += 0.08f;
        fb.fillCircle((int)p.x, (int)p.y, 2, p.col);
    }
    if (now < popup.die) {
        popup.y -= 0.5f;
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(popup.col);
        fb.drawString(popup.txt, (int)popup.x, (int)popup.y, 2);
    }
}

static void drawHud() {
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(frenzy ? TFT_CYAN : LIME_PALE);
    char b[20];
    snprintf(b, sizeof(b), "%d", score);
    fb.drawString(b, 4, 9, 2);
    fb.setTextDatum(MR_DATUM);
    if (mode == FR_ARCADE) {
        // lives as pips (a Spare Life perk can push this to 4)
        int pips = perkLife ? 4 : 3;
        for (int i = 0; i < pips; i++)
            fb.fillCircle(W - 8 - i * 10, 9, 3, i < lives ? TFT_RED : TEAL_DARK);
    } else {
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("ZEN", W - 4, 9, 2);
    }
    // combo readout, center
    if (combo >= 2) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(multNow() > 1 ? TFT_GOLD : TEAL_LIGHT);
        snprintf(b, sizeof(b), "COMBO %d x%d", combo, multNow() * (frenzy ? 2 : 1));
        fb.drawString(b, W / 2, 9, 1);
    }

    // footer
    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString("swipe=cut push=shove", 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM);
    if (angleIdx == 0) {
        fb.setTextColor(TEAL_DARK);
        fb.drawString("level", W - 4, H - 7, 1);
    } else {
        snprintf(b, sizeof(b), "aim %+d", angleIdx * 15);
        fb.setTextColor(LIME);
        fb.drawString(b, W - 4, H - 7, 1);
    }
}

static void drawFlashAndFrenzy() {
    uint32_t now = millis();
    if (now < flashUntil) {
        fb.drawRect(0, 0, W, H, flashColor);
        fb.drawRect(1, 1, W - 2, H - 2, flashColor);
        fb.drawRect(2, 2, W - 4, H - 4, flashColor);
    }
    if (frenzy && (now % 200 < 100)) {
        fb.drawRect(0, 0, W, H, TFT_CYAN);
        fb.drawRect(1, 1, W - 2, H - 2, TFT_WHITE);
    }
}

static void drawTitle() {
    fb.fillSprite(TEAL_DEEP);
    fb.drawRect(0, 0, W, H, TEAL_DARK);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME);
    fb.drawString("FRUIT", W / 2, 44, 4);
    fb.drawString("FRENZY", W / 2, 72, 4);

    // decorative fruit
    fb.fillCircle(30, 50, 8, TFT_ORANGE);
    fb.fillEllipse(140, 60, 12, 5, TFT_YELLOW);
    fb.fillCircle(38, 88, 12, TFT_GREEN);
    fb.drawCircle(38, 88, 12, TFT_DARKGREEN);

    const char* items[2] = { "ARCADE", "ZEN" };
    for (int i = 0; i < 2; i++) {
        int y = 130 + i * 34;
        bool sel = (titleSel == i);
        if (sel) {
            fb.fillRect(24, y - 13, W - 48, 26, LIME_DARK);
            fb.drawRect(24, y - 13, W - 48, 26, LIME);
        }
        fb.setTextColor(sel ? LIME_PALE : TEAL_LIGHT);
        fb.drawString(items[i], W / 2, y, 2);
    }

    char b[24];
    fb.setTextColor(TFT_GOLD);
    snprintf(b, sizeof(b), "HI %lu", (unsigned long)hiArcade);
    fb.drawString(b, W / 2 - 38, 208, 2);
    snprintf(b, sizeof(b), "HI %lu", (unsigned long)hiZen);
    fb.drawString(b, W / 2 + 38, 208, 2);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("arcade", W / 2 - 38, 224, 1);
    fb.drawString("zen", W / 2 + 38, 224, 1);

    // Armed Quest-inventory perks waiting to land next game
    {
        char armed[40] = ""; int ap = 0;
        if (qpPendingCount(ITEM_FR_WIDE))   ap += snprintf(armed+ap,sizeof(armed)-ap,"WIDE ");
        if (qpPendingCount(ITEM_FR_LIFE))   ap += snprintf(armed+ap,sizeof(armed)-ap,"+LIFE ");
        if (qpPendingCount(ITEM_FR_FRENZY)) ap += snprintf(armed+ap,sizeof(armed)-ap,"FRENZY");
        if (ap > 0) {
            fb.setTextColor(TFT_GOLD);
            char line[52]; snprintf(line,sizeof(line),"ARMED: %s", armed);
            fb.drawString(line, W / 2, 240, 1);
        }
    }
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("swipe up/down: choose", W / 2, 254, 1);
    fb.drawString("push forward: start", W / 2, 268, 1);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("circle cw/ccw aims the cut", W / 2, 284, 1);

    if (!sensorPresent) {
        fb.setTextColor(TFT_RED);
        fb.drawString("SENSOR UNPLUGGED", W / 2, 302, 1);
    }
    fb.pushSprite(0, 0);
}

static void drawPlay() {
    fb.fillSprite(TEAL_DEEP);
    drawReticle();
    drawSlash();
    drawItem();
    drawParticlesAndPopup();
    drawHud();
    drawFlashAndFrenzy();
    fb.pushSprite(0, 0);
}

static void drawOver() {
    fb.fillSprite(PURPLE_DEEP);
    fb.drawRect(0, 0, W, H, PURPLE);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_RED);
    fb.drawString("GAME OVER", W / 2, 90, 4);

    char b[24];
    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "%d", score);
    fb.drawString(b, W / 2, 150, 6);

    fb.setTextColor(TFT_GOLD);
    snprintf(b, sizeof(b), "BEST %lu",
             (unsigned long)(mode == FR_ARCADE ? hiArcade : hiZen));
    fb.drawString(b, W / 2, 196, 2);
    if (newHigh) {
        fb.setTextColor((millis() % 400 < 200) ? LIME : LIME_PALE);
        fb.drawString("NEW HIGH SCORE!", W / 2, 222, 2);
    }
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("push forward to continue", W / 2, 272, 1);
    fb.pushSprite(0, 0);
}

void fruitDraw() {
    switch (screen) {
        case FR_TITLE: drawTitle(); break;
        case FR_PLAY:  drawPlay();  break;
        case FR_OVER:  drawOver();  break;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void fruitEnter() {
    fb.setRotation(0);
    loadHighs();
    // Belt & suspenders: if the sensor appeared since the last probe (or the
    // app was launched from the studio), make sure it's initialized.
    if (!sensorInited) fruitProbeSensor();
    screen = FR_TITLE;
    titleSel = 0;
    // Drain any stale gesture flag so an old swipe doesn't instantly select.
    if (sensorInited) paj7620ReadGesture();
}

void fruitShutdown() {
    // Sensor keeps running; probing/reads are cheap and re-entry is instant.
}
