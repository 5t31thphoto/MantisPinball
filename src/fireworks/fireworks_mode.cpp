// FILE: src/fireworks/fireworks_mode.cpp
// FIREWORKS - pure stim eye candy. Nothing to win, nothing saved.
//
//   LEFT  tap        bottle rocket: whistling zig-zag climb, crackling pop
//   LEFT  hold       charge a rocket; bigger + stranger the longer you hold:
//                    peony -> chrysanthemum -> crossette -> golden willow
//   RIGHT tap        roman candle: a column of coloured stars, one by one
//   RIGHT hold       artillery shell grows in the mortar as you charge:
//                    ring -> double ring -> heart -> MANTIS (+ extra breaks)
// Both buttons launch on RELEASE. The two-button exit chord is guarded, so
// leaving the mode never fires anything.
//
// The particle pool (~9 KB) is malloc'd on Enter and freed on Shutdown, the
// house rule for big buffers. Every frame is redrawn from particle state like
// every other app - no direct sprite-buffer tricks.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fireworks_mode.h"
#include "../theme_colors.h"
#include "../chord_guard.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170, H = 320;
static constexpr int GROUND_Y = 296;            // top of the hill line
static constexpr int MAXP = 360;
static constexpr uint32_t TAP_MS = 220, FULL_MS = 2400, ENTER_GUARD = 250;

// ---- colour families: bright -> deep, fade by life ------------------------
enum Fam : uint8_t { F_RED = 0, F_GOLD, F_BLUE, F_SILVER, F_COUNT };
static uint16_t famCol(uint8_t fam, uint8_t lvl) {     // lvl 0 = brightest
    static const uint16_t T[F_COUNT][5] = {
        { PURPLE_LIGHT, PURPLE_BRIGHT, PURPLE, PURPLE_DARK, PURPLE_DEEP },
        { LIME_PALE,    LIME,          LIME_MED, LIME_DARK, LIME_DARK   },
        { TEAL_LIGHT,   TEAL_BRIGHT,   TEAL,   TEAL_DARK,   TEAL_DEEP   },
        { LIME_PALE,    TEAL_LIGHT,    TEAL_BRIGHT, TEAL,   TEAL_DARK   },
    };
    return T[fam % F_COUNT][lvl > 4 ? 4 : lvl];
}

// ---- particles ------------------------------------------------------------
enum Kind : uint8_t {
    K_FREE = 0,
    K_STAR,       // burst star: gravity + drag, fades
    K_SPARK,      // trail spark: short life, no gravity to speak of
    K_CHRYS,      // star that sheds sparks
    K_CROSS,      // crossette star: splits into four at mid-life
    K_WILLOW,     // gold, heavy drag, long droop, sheds faint sparks
    K_SHAPE,      // shape-holding star (ring/heart/mantis): high drag
    K_ROCKET,     // climbing shell; bursts when its fuse runs out
    K_CANDLE,     // roman-candle star climbing; pops small at apex
};
enum Pat : uint8_t { P_CRACKLE, P_PEONY, P_CHRYS, P_CROSSETTE, P_WILLOW,
                     P_RING, P_DRING, P_HEART, P_MANTIS };

struct Part {
    float x, y, vx, vy;
    uint8_t kind, fam, fam2, life, maxLife;
    uint8_t pat, size, breaks;     // rockets: what they burst into
    uint8_t wobble;                // bottle rockets zig-zag
};
static Part* P = nullptr;
static int   cursor = 0;

static float frand(float a, float b) { return a + (b - a) * (float)(esp_random() & 0xFFFF) / 65535.0f; }
static int   irand(int a, int b) { return a + (int)(esp_random() % (uint32_t)(b - a + 1)); }

// Free slot, or nullptr when full. Only launches pass force=true and may
// recycle a slot; sparks are simply dropped, so a full pool can never
// overwrite a particle while stepParticles() is updating it.
static Part* spawn(bool force = false) {
    for (int n = 0; n < MAXP; ++n) {
        int i = (cursor + n) % MAXP;
        if (P[i].kind == K_FREE) { cursor = (i + 1) % MAXP; return &P[i]; }
    }
    if (!force) return nullptr;
    Part* p = &P[cursor];
    cursor = (cursor + 1) % MAXP;
    return p;
}
static void emit(float x, float y, float vx, float vy, uint8_t kind, uint8_t fam, uint8_t life) {
    Part* p = spawn(); if (!p) return; memset(p, 0, sizeof(Part));
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->kind = kind; p->fam = fam; p->fam2 = fam; p->life = p->maxLife = life;
}

// ---- show state -----------------------------------------------------------
static uint32_t enterAt = 0, frame = 0;
static bool     lDown = false, rDown = false;
static uint32_t lAt = 0, rAt = 0;
static int      flash = 0, cheer = 0, hintT = 0;
static int      candleLeft = 0, candleTimer = 0, candleFam = 0;
static uint8_t  starTw[40];

// ---- bursts ---------------------------------------------------------------
static void burst(float x, float y, uint8_t pat, uint8_t size, uint8_t fam, uint8_t fam2) {
    float s = 0.6f + size * 0.012f;            // size 0..100 -> speed
    switch (pat) {
    case P_CRACKLE:
        for (int i = 0; i < 16; ++i) { float a = frand(0, 6.283f), v = frand(0.4f, 1.4f);
            emit(x, y, cosf(a) * v, sinf(a) * v, K_SPARK, (uint8_t)((i & 3) ? fam : (uint8_t)F_SILVER), irand(8, 16)); }
        break;
    case P_PEONY: {
        int n = 40 + size / 2;
        for (int i = 0; i < n; ++i) { float a = 6.283f * i / n + frand(-0.05f, 0.05f), v = s * frand(0.85f, 1.0f);
            emit(x, y, cosf(a) * v, sinf(a) * v, K_STAR, fam, irand(40, 58)); }
        break; }
    case P_CHRYS: {
        int n = 46 + size / 2;
        for (int i = 0; i < n; ++i) { float a = 6.283f * i / n, v = s * frand(0.9f, 1.05f);
            emit(x, y, cosf(a) * v, sinf(a) * v, K_CHRYS, fam, irand(50, 66)); }
        break; }
    case P_CROSSETTE:
        for (int i = 0; i < 10; ++i) { float a = 6.283f * i / 10, v = s * 1.05f;
            emit(x, y, cosf(a) * v, sinf(a) * v, K_CROSS, fam, 64);
            Part* last = &P[(cursor + MAXP - 1) % MAXP]; if (last->kind == K_CROSS) last->fam2 = fam2; }
        break;
    case P_WILLOW:
        for (int i = 0; i < 64; ++i) { float a = frand(0, 6.283f), v = s * frand(0.6f, 1.15f);
            emit(x, y, cosf(a) * v, sinf(a) * v - 0.3f, K_WILLOW, F_GOLD, irand(110, 150)); }
        flash = 3;
        break;
    case P_RING: case P_DRING: {
        float tilt = frand(0.45f, 0.75f);           // ellipse = a ring seen at an angle
        for (int i = 0; i < 44; ++i) { float a = 6.283f * i / 44;
            emit(x, y, cosf(a) * s * 1.1f, sinf(a) * s * 1.1f * tilt, K_SHAPE, fam, 62); }
        if (pat == P_DRING)
            for (int i = 0; i < 30; ++i) { float a = 6.283f * i / 30 + 0.1f;
                emit(x, y, cosf(a) * s * 0.6f, sinf(a) * s * 0.6f * tilt, K_SHAPE, fam2, 56); }
        break; }
    case P_HEART:
        for (int i = 0; i < 56; ++i) {
            float t = 6.283f * i / 56;                 // the classic heart curve
            float hx = 16 * powf(sinf(t), 3);
            float hy = -(13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t));
            emit(x, y, hx * s * 0.075f, hy * s * 0.075f, K_SHAPE, F_RED, 70);
        }
        break;
    case P_MANTIS: {                                    // a mantis face in the sky
        auto line = [&](float x0, float y0, float x1, float y1, int n, uint8_t f) {
            for (int i = 0; i <= n; ++i) { float t = (float)i / n;
                emit(x, y, (x0 + (x1 - x0) * t) * s * 0.07f, (y0 + (y1 - y0) * t) * s * 0.07f, K_SHAPE, f, 78); }
        };
        line(-14, -6, 14, -6, 14, F_GOLD);     // triangular head
        line(-14, -6, 0, 14, 12, F_GOLD);
        line(14, -6, 0, 14, 12, F_GOLD);
        line(-6, -6, -14, -24, 8, F_GOLD);     // antennae
        line(6, -6, 14, -24, 8, F_GOLD);
        for (int i = 0; i < 10; ++i) { float a = 6.283f * i / 10;   // big eyes
            emit(x, y, (-7 + cosf(a) * 3.5f) * s * 0.07f, (1 + sinf(a) * 3.5f) * s * 0.07f, K_SHAPE, F_RED, 82);
            emit(x, y, (7 + cosf(a) * 3.5f) * s * 0.07f, (1 + sinf(a) * 3.5f) * s * 0.07f, K_SHAPE, F_RED, 82); }
        flash = 4; cheer = 90;
        break; }
    }
    if (pat >= P_WILLOW) cheer = cheer > 50 ? cheer : 50;
}

static void launch(float x, float vy, uint8_t pat, uint8_t size, uint8_t fam, uint8_t fam2,
                   uint8_t fuse, uint8_t breaks, bool whistle) {
    Part* p = spawn(true); memset(p, 0, sizeof(Part));
    p->x = x; p->y = GROUND_Y - 6; p->vx = frand(-0.15f, 0.15f); p->vy = vy;
    p->kind = K_ROCKET; p->fam = fam; p->fam2 = fam2; p->life = p->maxLife = fuse;
    p->pat = pat; p->size = size; p->breaks = breaks; p->wobble = whistle ? 1 : 0;
}

// charge 0..1 for a held button
static float charge(uint32_t since) {
    uint32_t d = millis() - since;
    if (d <= TAP_MS) return 0;
    float c = (float)(d - TAP_MS) / (float)(FULL_MS - TAP_MS);
    return c > 1 ? 1 : c;
}

static void releaseLeft(uint32_t startedAt) {
    uint32_t held = millis() - startedAt;
    float x = frand(30, 90);
    if (held < TAP_MS) {                                       // bottle rocket
        launch(x, frand(-6.2f, -5.2f), P_CRACKLE, 10, irand(0, F_COUNT - 1), F_SILVER, irand(30, 40), 1, true);
        return;
    }
    float c = charge(startedAt);
    uint8_t pat = c < 0.33f ? P_PEONY : c < 0.66f ? P_CHRYS : c < 0.9f ? P_CROSSETTE : P_WILLOW;
    launch(x, -5.6f - c * 1.4f, pat, (uint8_t)(30 + c * 70), irand(0, F_COUNT - 1),
           irand(0, F_COUNT - 1), (uint8_t)(44 + c * 14), 1, false);
}

static void releaseRight(uint32_t startedAt) {
    uint32_t held = millis() - startedAt;
    if (held < TAP_MS) {                                       // roman candle
        candleLeft = 7; candleTimer = 0; candleFam = irand(0, F_COUNT - 1);
        return;
    }
    float c = charge(startedAt);
    uint8_t pat = c < 0.3f ? P_RING : c < 0.55f ? P_DRING : c < 0.8f ? P_HEART : P_MANTIS;
    uint8_t breaks = c < 0.6f ? 1 : c < 0.9f ? 2 : 3;          // follow-up bursts
    launch(frand(105, 140), -5.0f - c * 1.8f, pat, (uint8_t)(40 + c * 60),
           (uint8_t)((millis() / 300) % F_COUNT), irand(0, F_COUNT - 1),
           (uint8_t)(52 + c * 14), breaks, false);
}

// ---- simulation -----------------------------------------------------------
static void stepParticles() {
    for (int i = 0; i < MAXP; ++i) {
        Part& p = P[i];
        if (p.kind == K_FREE) continue;
        switch (p.kind) {
        case K_ROCKET:
            p.vy += 0.05f;
            if (p.wobble) p.vx = sinf((float)(frame + i) * 0.6f) * 0.9f;
            emit(p.x, p.y + 2, frand(-0.2f, 0.2f), frand(0.2f, 0.6f), K_SPARK,
                 p.wobble ? F_SILVER : F_GOLD, irand(6, 12));
            if (--p.life == 0 || p.vy > -0.4f) {
                burst(p.x, p.y, p.pat, p.size, p.fam, p.fam2);
                if (p.breaks > 1) {                     // multi-break: another shell rides on
                    float x = p.x, y = p.y; uint8_t b = p.breaks - 1, f2 = p.fam2, sz = p.size;
                    p.kind = K_FREE;
                    Part* q = spawn(true); memset(q, 0, sizeof(Part));
                    q->x = x + frand(-12, 12); q->y = y; q->vx = 0; q->vy = -1.2f;
                    q->kind = K_ROCKET; q->life = q->maxLife = 22; q->fam = f2; q->fam2 = p.fam;
                    q->pat = (b == 2) ? P_PEONY : P_CRACKLE; q->size = (uint8_t)(sz * 0.7f); q->breaks = b;
                    continue;
                }
                p.kind = K_FREE;
                continue;
            }
            break;
        case K_CANDLE:
            p.vy += 0.07f;
            emit(p.x, p.y + 1, frand(-0.15f, 0.15f), 0.3f, K_SPARK, p.fam, 7);
            if (p.vy > -0.3f) { burst(p.x, p.y, P_CRACKLE, 4, p.fam, p.fam); p.kind = K_FREE; continue; }
            break;
        case K_SPARK:  p.vy += 0.01f; p.vx *= 0.94f; p.vy *= 0.94f; break;
        case K_STAR:   p.vy += 0.035f; p.vx *= 0.975f; p.vy *= 0.975f; break;
        case K_SHAPE:  p.vy += 0.012f; p.vx *= 0.93f; p.vy *= 0.93f; break;
        case K_WILLOW:
            p.vy += 0.018f; p.vx *= 0.955f; p.vy *= 0.955f;
            if ((frame & 3) == 0) emit(p.x, p.y, 0, 0.05f, K_SPARK, F_GOLD, 14);
            break;
        case K_CHRYS:
            p.vy += 0.03f; p.vx *= 0.97f; p.vy *= 0.97f;
            if ((frame & 1) == 0) emit(p.x, p.y, 0, 0.1f, K_SPARK, p.fam, 10);
            break;
        case K_CROSS:  p.vy += 0.03f; p.vx *= 0.97f; p.vy *= 0.97f;
                       if (p.life == p.maxLife / 2) {      // split into four
                           for (int k = 0; k < 4; ++k) { float a = 0.785f + 1.571f * k;
                               emit(p.x, p.y, cosf(a) * 0.9f, sinf(a) * 0.9f, K_STAR, p.fam2, 30); }
                           p.kind = K_FREE; continue;
                       }
                       break;
        }
        p.x += p.vx; p.y += p.vy;
        if (p.kind != K_ROCKET && p.kind != K_CANDLE) {
            if (p.life) --p.life;
            if (!p.life || p.y > GROUND_Y + 2 || p.x < -4 || p.x > W + 4) p.kind = K_FREE;
        }
    }
}

// ---- input + tick ---------------------------------------------------------
void fireworksTick() {
    chordGuardTick();
    if (!P) return;
    ++frame;
    uint32_t now = millis();
    bool live = (now - enterAt) >= ENTER_GUARD;

    // Left: press starts a charge, release launches. A chord poisons it.
    if (live && chordLEdge()) { lDown = true; lAt = now; }
    if (lDown && btnL && !chordLHeld()) lDown = false;          // part of the exit chord
    if (lDown && !btnL) { lDown = false; releaseLeft(lAt); }
    if (live && chordREdge()) { rDown = true; rAt = now; }
    if (rDown && btnR && !chordRHeld()) rDown = false;
    if (rDown && !btnR) { rDown = false; releaseRight(rAt); }

    // fuse sparks while a charge is building
    if (lDown && charge(lAt) > 0 && (frame & 1))
        emit(frand(40, 80), GROUND_Y - 4, frand(-0.6f, 0.6f), frand(-1.4f, -0.6f), K_SPARK, F_GOLD, irand(6, 12));

    // roman candle: one star every few frames
    if (candleLeft > 0 && ++candleTimer >= 9) {
        candleTimer = 0; --candleLeft;
        Part* p = spawn(true); memset(p, 0, sizeof(Part));
        p->x = 150; p->y = GROUND_Y - 10; p->vx = frand(-0.35f, 0.15f); p->vy = frand(-5.6f, -4.6f);
        p->kind = K_CANDLE; p->fam = (uint8_t)((candleFam + candleLeft) % F_COUNT);
        p->life = p->maxLife = 255;
    }
    stepParticles();
    if (flash) --flash;
    if (cheer) --cheer;
    if (hintT) --hintT;
}

// ---- drawing --------------------------------------------------------------
static void drawSpectator(int cx, int cy, bool arms) {      // the little mantis
    uint16_t g = LIME, d = LIME_DARK;
    fb.fillRoundRect(cx - 3, cy - 6, 6, 12, 3, d);
    fb.fillTriangle(cx - 5, cy - 8, cx + 5, cy - 8, cx, cy - 2, g);
    fb.drawPixel(cx - 2, cy - 7, PURPLE_LIGHT); fb.drawPixel(cx + 2, cy - 7, PURPLE_LIGHT);
    fb.drawLine(cx - 2, cy - 9, cx - 5, cy - 14, d); fb.drawLine(cx + 2, cy - 9, cx + 5, cy - 14, d);
    int a = arms ? -9 + (int)(sinf(frame * 0.4f) * 2) : -2;
    fb.drawLine(cx - 3, cy - 3, cx - 8, cy + a, g); fb.drawLine(cx + 3, cy - 3, cx + 8, cy + a, g);
}

void fireworksDraw() {
    if (!P) {
        fb.fillSprite(TEAL_DEEP); fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("not enough memory", W / 2, H / 2, 2); fb.pushSprite(0, 0); return;
    }
    fb.fillSprite(flash ? TEAL_DARK : TEAL_DEEP);
    for (int i = 0; i < 40; ++i) {                             // twinkling sky
        int x = (i * 53 + 11) % W, y = (i * 97 + 7) % (GROUND_Y - 20);
        uint8_t ph = (uint8_t)(starTw[i] + frame);
        if ((ph & 63) < 50) fb.drawPixel(x, y, (ph & 64) ? TEAL_LIGHT : TEAL);
    }
    for (int i = 0; i < MAXP; ++i) {
        const Part& p = P[i];
        if (p.kind == K_FREE) continue;
        int x = (int)p.x, y = (int)p.y;
        if (p.kind == K_ROCKET || p.kind == K_CANDLE) {
            uint16_t c = famCol(p.fam, 0);
            fb.fillCircle(x, y, p.kind == K_CANDLE ? 2 : 1, c);
            continue;
        }
        uint8_t lvl = (uint8_t)(4 - (4 * p.life) / (p.maxLife ? p.maxLife : 1));
        uint16_t c = famCol(p.fam, lvl);
        fb.drawPixel(x, y, c);
        if (p.kind != K_SPARK && lvl <= 1) {                    // bright stars get a small cross glow
            uint16_t g = famCol(p.fam, lvl + 2);
            fb.drawPixel(x - 1, y, g); fb.drawPixel(x + 1, y, g);
            fb.drawPixel(x, y - 1, g); fb.drawPixel(x, y + 1, g);
        }
    }
    // ground: rolling hill + the two launchers
    for (int x = 0; x < W; ++x) {
        int hy = GROUND_Y + (int)(sinf(x * 0.05f) * 3 + sinf(x * 0.13f) * 2);
        fb.drawFastVLine(x, hy, H - hy, TEAL_DARK);
        if ((x % 7) == 0) fb.drawPixel(x, hy - 1, LIME_DARK);
    }
    // left launch pad - glows as it charges
    float lc = lDown ? charge(lAt) : 0;
    fb.fillRect(52, GROUND_Y - 6, 16, 6, PURPLE_DARK);
    if (lc > 0) fb.fillRect(52, GROUND_Y - 6, (int)(16 * lc), 2, famCol(F_GOLD, lc > 0.9f ? 0 : 1));
    // right mortar - the shell grows and cycles colour while charging
    fb.fillRect(140, GROUND_Y - 12, 14, 12, PURPLE_DEEP);
    fb.drawRect(140, GROUND_Y - 12, 14, 12, PURPLE_DARK);
    float rc = rDown ? charge(rAt) : 0;
    if (rc > 0) {
        int r = 2 + (int)(rc * 6);
        uint8_t fam = (uint8_t)((millis() / 300) % F_COUNT);
        fb.fillCircle(147, GROUND_Y - 13 - r / 2, r, famCol(fam, 1));
        fb.drawCircle(147, GROUND_Y - 13 - r / 2, r + 1, famCol(fam, 0));
    }
    drawSpectator(W / 2 + 8, GROUND_Y + 8, cheer > 0);
    if (hintT > 0) {
        fb.setTextDatum(TC_DATUM); fb.setTextColor(hintT > 40 ? TEAL_LIGHT : TEAL);
        fb.drawString("L rockets   R shells", W / 2, 8, 1);
        fb.drawString("hold to charge", W / 2, 20, 1);
    }
    fb.pushSprite(0, 0);
}

// ---- lifecycle ------------------------------------------------------------
void fireworksEnter() {
    fb.setRotation(0);
    chordGuardReset();
    if (!P) P = (Part*)malloc(sizeof(Part) * MAXP);
    if (P) memset(P, 0, sizeof(Part) * MAXP);
    cursor = 0; frame = 0; flash = cheer = 0; hintT = 240;
    lDown = rDown = false; candleLeft = 0;
    for (int i = 0; i < 40; ++i) starTw[i] = (uint8_t)irand(0, 255);
    enterAt = millis();
}

void fireworksShutdown() {
    if (P) { free(P); P = nullptr; }
}
