// FILE: src/danceparty/danceparty_mode.cpp
// DANCE PARTY - the pet-mode mantis, turned dancer, grooving in time with the
// Mantis Synapse (Core2) over the MantisNow mesh.
//
// Synapse broadcasts MantisNow kind 0x30 (see MANTISNOW.md, decision 6):
//   BEAT  (sub 0): bpm, beatInBar, msSinceStart  - we lock our beat to it
//   MOVE  (sub 1): moveId, intensity, durationMs - we switch dance move
//   START (sub 2) / STOP (sub 3)
// No host heard for 3 s -> it free-runs its own groove at the last bpm, so the
// mode is fun on its own too. L = next move, R = hype burst. Exit chord guarded.
//
// The dancer is the procedural mantis from pet_mode.cpp (legs, abdomen,
// thorax, praying forelegs, neck, triangular head, eyes, antennae), copied
// here rather than shared so the working pet mode is untouched, and driven by
// explicit dance parameters instead of mood.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <string.h>
#include "danceparty_mode.h"
#include "../theme_colors.h"
#include "../chord_guard.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170, H = 320;

// ===========================================================================
//  MantisNow receive (kind 0x30 DANCE only) - same wire format as Mantis Chat
// ===========================================================================
static constexpr uint8_t MN_MAGIC = 0x4D, MN_VERSION = 0x01, MN_CHANNEL = 1, K_DANCE = 0x30;
static constexpr int HDR = 28;
enum : uint8_t { D_BEAT = 0, D_MOVE = 1, D_START = 2, D_STOP = 3 };
struct __attribute__((packed)) MnHeader {
    uint8_t magic, version, kind, flags;
    uint32_t srcNode, dstNode, pktId;
    uint16_t seq;
    uint8_t ttl, chanOrSub;
    char callsign[8];
};
static_assert(sizeof(MnHeader) == HDR, "MantisNow header must be 28 bytes");
struct RxSlot { volatile uint8_t full; uint8_t len; uint8_t data[64]; };
static RxSlot rx[8];
static volatile uint8_t rxW = 0;
static uint8_t rxR = 0;
static uint32_t dedupNode[16], dedupPkt[16];
static int dedupHead = 0;
static bool radioUp = false;
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void onRecv(const uint8_t* mac, const uint8_t* d, int len) {     // copy and return
    (void)mac;
    if (len < HDR || len > 64 || d[0] != MN_MAGIC || d[1] != MN_VERSION || d[2] != K_DANCE) return;
    RxSlot& s = rx[rxW % 8];
    if (s.full) return;
    memcpy(s.data, d, (size_t)len); s.len = (uint8_t)len; s.full = 1;
    rxW = (uint8_t)(rxW + 1);
}
static bool radioStart() {                     // Versus' bring-up + MantisNow channel lock
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(MN_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(onRecv));
    esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, BCAST, 6); p.channel = 0; p.encrypt = false;
    if (!esp_now_is_peer_exist(BCAST)) esp_now_add_peer(&p);
    radioUp = true;
    return true;
}
static void radioStop() {
    if (radioUp) { delay(20); esp_now_unregister_recv_cb(); esp_now_deinit(); radioUp = false; }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ===========================================================================
//  groove state
// ===========================================================================
enum Move : uint8_t { MV_BOB = 0, MV_SWAY, MV_ARMSUP, MV_SPIN, MV_JUMP, MV_WAVE,
                      MV_ROBOT, MV_SHUFFLE, MV_BANG, MV_POSE, MV_COUNT };
static const char* MOVE_NAME[MV_COUNT] = {"BOB","SWAY","ARMS UP","SPIN","JUMP",
                                          "WAVE","ROBOT","SHUFFLE","HEADBANG","POSE"};
static float    bpm = 0.0f;                  // ONLY ever what the Synapse measured; 0 = none yet
static uint32_t beatAnchor = 0;               // millis() of a known beat
static uint8_t  barAnchor = 0;                // beat-in-bar at beatAnchor
static uint8_t  move = MV_BOB;
static uint8_t  intensity = 180;
static uint32_t lastHostMs = 0, lastBeatMs = 0, enterAt = 0, frameNo = 0;
static char     hostCall[9] = "";
static bool     partyOn = true;
static int      hype = 0;
static uint32_t lastAutoBeat = 0;

enum DScreen : uint8_t { DS_WELCOME = 0, DS_LOBBY, DS_DANCE, DS_TAP };
static DScreen dscr = DS_WELCOME;
static uint8_t menuSel = 0;
// tap tempo: R taps the beat; tempo = average of the last few gaps
static uint32_t taps[6]; static int tapN = 0;
static bool lHeldDown = false; static uint32_t lDownAt = 0; static bool lLong = false;

static bool synced() { return lastHostMs && millis() - lastHostMs < 3000; }
// Beats are live while they keep arriving (2.5 beat periods of slack). The
// Synapse only sends beats it tracked from real music, so when the song stops
// the beats stop and the dancer eases into an idle sway - it never invents a
// tempo of its own.
static bool beatLive() {
    if (dscr == DS_TAP) return bpm >= 40;              // your taps set the beat until you leave
    if (!lastBeatMs || bpm < 40) return false;
    return millis() - lastBeatMs < (uint32_t)(2.5f * 60000.0f / bpm);
}

// beat position: integer beats since anchor + phase 0..1 within the beat
static void beatPos(uint32_t now, long& beatNo, float& phase) {
    float tb = beatLive() ? bpm : 40.0f;             // idle drift when no live beat
    float period = 60000.0f / (tb < 40 ? 40 : tb > 220 ? 220 : tb);
    float since = (float)(int32_t)(now - beatAnchor);
    float b = since / period;
    beatNo = (long)floorf(b);
    phase = b - (float)beatNo;
}

static void handle(const uint8_t* d, int len) {
    MnHeader h; memcpy(&h, d, HDR);
    for (int i = 0; i < 16; i++) if (dedupNode[i] == h.srcNode && dedupPkt[i] == h.pktId) return;
    dedupNode[dedupHead] = h.srcNode; dedupPkt[dedupHead] = h.pktId; dedupHead = (dedupHead + 1) % 16;
    memcpy(hostCall, h.callsign, 8); hostCall[8] = 0;
    lastHostMs = millis();
    const uint8_t* p = d + HDR; int pl = len - HDR;
    switch (h.chanOrSub) {
    case D_BEAT:
        if (pl >= 3) {
            uint16_t b; memcpy(&b, p, 2);
            if (b >= 40 && b <= 220) bpm = (float)b;
            beatAnchor = millis();                  // this frame IS a beat
            lastBeatMs = beatAnchor;
            barAnchor = p[2] & 3;
        }
        break;
    case D_MOVE:
        if (pl >= 2) { move = (uint8_t)(p[0] < MV_COUNT ? p[0] : (uint8_t)MV_BOB); intensity = p[1]; }
        break;
    case D_START: partyOn = true; if (pl >= 2) { uint16_t b; memcpy(&b, p, 2); if (b >= 40 && b <= 220) bpm = (float)b; } break;
    case D_STOP:  partyOn = false; move = MV_POSE; break;
    }
}

// ===========================================================================
//  the dancer - pet_mode's mantis, driven by dance parameters
// ===========================================================================
struct DancePose {
    float sway, bob, armL, armR;     // arms: 0 = folded prayer, 1 = straight up
    float headTilt, squashX, jump, legSpread;
};

static DancePose poseFor(uint8_t mv, float ph, long beatNo, float k) {
    // ph 0..1 within the beat, k = intensity 0..1. Hits land at ph = 0.
    DancePose P = {0, 0, 0.3f, 0.3f, 0, 1.0f, 0, 0};
    float hit = 1.0f - ph;                          // 1 on the beat, decays
    float s1 = sinf(ph * 6.2832f);
    bool odd = beatNo & 1;
    switch (mv) {
    case MV_BOB:     P.bob = -hit * 5 * k; P.armL = P.armR = 0.25f + 0.15f * hit; break;
    case MV_SWAY:    P.sway = (odd ? 1 : -1) * (1 - ph * 2) * 6 * k;
                     P.armL = 0.3f + 0.2f * s1; P.armR = 0.3f - 0.2f * s1; P.headTilt = P.sway * 0.4f; break;
    case MV_ARMSUP:  P.armL = P.armR = 0.6f + 0.4f * hit; P.bob = -hit * 3 * k; break;
    case MV_SPIN:    P.squashX = cosf((beatNo % 2 + ph) * 3.1416f);  // turns over two beats
                     P.armL = P.armR = 0.5f; break;
    case MV_JUMP:    P.jump = sinf(ph * 3.1416f) * 14 * k; P.armL = P.armR = 0.4f + 0.6f * sinf(ph * 3.1416f);
                     P.legSpread = sinf(ph * 3.1416f) * 3; break;
    case MV_WAVE:    if (odd) P.armR = 0.9f + 0.1f * s1; else P.armL = 0.9f + 0.1f * s1;
                     P.sway = s1 * 2; break;
    case MV_ROBOT: { int st = (int)(ph * 2) + (int)(beatNo % 2) * 2;      // snap to 4 shapes, hold each
                     static const float AL[4] = {0.0f, 1.0f, 0.5f, 0.0f}, AR[4] = {1.0f, 0.0f, 0.5f, 0.0f};
                     P.armL = AL[st & 3]; P.armR = AR[st & 3]; P.headTilt = (st & 1) ? 3 : -3; break; }
    case MV_SHUFFLE: P.sway = (odd ? 3 : -3) * k; P.legSpread = (ph < 0.5f ? 4 : -2);
                     P.bob = -fabsf(s1) * 3; P.armL = 0.2f; P.armR = 0.4f; break;
    case MV_BANG:    P.headTilt = 0; P.bob = (ph < 0.25f ? 4 : 0) * k; P.armL = P.armR = 0.15f;
                     P.squashX = 1.0f; break;
    default:         P.armL = 0.9f; P.armR = 0.05f; P.headTilt = 2; break;           // POSE
    }
    return P;
}

// thick line: the scaled-up mantis needs limbs with weight, not 1-px wires
static void thick(int x0, int y0, int x1, int y1, int w, uint16_t c) {
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0), L = sqrtf(dx*dx + dy*dy);
    if (L < 0.5f) { fb.fillCircle(x0, y0, w / 2, c); return; }
    float nx = -dy / L, ny = dx / L;
    for (int i = 0; i < w; i++) {
        float o = i - (w - 1) * 0.5f;
        fb.drawLine(x0 + (int)(nx*o), y0 + (int)(ny*o), x1 + (int)(nx*o), y1 + (int)(ny*o), c);
    }
    fb.fillCircle(x1, y1, w / 2, c);                     // rounded joints
}

// The pet-mode mantis, every dimension multiplied by k so it fills the
// screen. Same anatomy and proportions as pet_mode.cpp's drawMantisAnimated.
static void drawDancer(int cx, int feetY, float k, uint32_t t, const DancePose& P) {
    uint16_t body = LIME, dark = LIME_DARK, legC = LIME_MED, eyeC = PURPLE_DARK, accent = LIME_PALE, edge = TEAL_DEEP;
    float ts = t / 1000.0f;
    float breath = sinf(ts * 2.2f);
    // every limb gets a dark outline so the mantis pops on any background
    auto limb = [&](int x0, int y0, int x1, int y1, int w, uint16_t c) { thick(x0, y0, x1, y1, w + 2, edge); thick(x0, y0, x1, y1, w, c); };
    auto K = [&](float v) { return (int)lroundf(v * k); };
    int lw = K(1.3f); if (lw < 2) lw = 2;                 // limb thickness
    int jumpY = K(P.jump);
    int baseY = feetY + 1 - jumpY, hipY = feetY - K(2) - jumpY;
    int cxB = cx + K(P.sway);
    float bodyH = 46.0f * k;
    int abdY    = hipY - (int)(bodyH * 0.5f);
    int thoraxY = abdY - (int)(bodyH * 0.32f) + K(P.bob);
    int headY   = thoraxY - (int)(bodyH * 0.28f) + K(P.bob);
    float sx = fabsf(P.squashX) < 0.25f ? 0.25f : fabsf(P.squashX);   // spin narrows the body
    auto X = [&](float dx) { return cxB + (int)(dx * k * sx); };
    bool back = P.squashX < 0;
    float ls = P.legSpread;

    // six legs, two segments each, feet planted (pet model layout, scaled)
    auto leg = [&](float rdx, int ry, float kdx, float kdy, float fdx, float ph) {
        int rx = X(rdx);
        int kx = rx + (int)(kdx * k * sx), ky = ry + K(kdy);
        int fx = rx + (int)(fdx * k * sx) + K(sinf(ts * 2.0f + ph) * 1.5f);
        limb(rx, ry, kx, ky, lw, legC); limb(kx, ky, fx, baseY, lw, legC);
    };
    leg(-3, thoraxY + K(4), -7 - ls, 5, -11 - ls, 0.0f);
    leg( 3, thoraxY + K(4),  7 + ls, 5,  11 + ls, 0.6f);
    leg(-3, abdY - K(2),    -9 - ls, 4, -13 - ls, 1.2f);
    leg( 3, abdY - K(2),     9 + ls, 4,  13 + ls, 1.8f);
    leg(-3, abdY + K(3),    -8 - ls, 6, -10 - ls, 2.4f);
    leg( 3, abdY + K(3),     8 + ls, 6,  10 + ls, 3.0f);

    // abdomen with segment bands, thorax
    int abdW = (int)((9 + breath) * k * sx) + 2;           // fuller than the pet's - reads at size
    int abdTop = abdY - (int)(bodyH * 0.18f), abdH = (int)(bodyH * 0.5f);
    fb.fillRoundRect(cxB - abdW/2 - 1, abdTop - 1, abdW + 2, abdH + 2, K(3), edge);
    fb.fillRoundRect(cxB - abdW/2, abdTop, abdW, abdH, K(3), body);
    for (int i = 1; i < 4; i++) fb.drawFastHLine(cxB - abdW/2 + 2, abdTop + i * (int)(bodyH * 0.12f), abdW > 4 ? abdW - 4 : 1, dark);
    int thW = (int)(9 * k * sx) + 2, thH = (thoraxY < abdY ? abdY - thoraxY : K(6));
    fb.fillRoundRect(cxB - thW/2 - 1, thoraxY - 1, thW + 2, thH + 2, K(3), edge);
    fb.fillRoundRect(cxB - thW/2, thoraxY, thW, thH, K(3), dark);

    // raptorial forelegs: 0 = folded prayer, 1 = raised straight up
    auto arm = [&](int side, float a) {
        int rx = X(side * 3), ry = thoraxY + K(2);
        float ang = -0.35f - a * 1.15f;
        int ex = rx + side * (int)(cosf(ang) * 10 * k * sx), ey = ry + (int)(sinf(ang) * 10 * k);
        float fa = ang - 0.9f * (1 - a);
        int hx = ex + side * (int)(cosf(fa) * 8 * k * sx), hy = ey + (int)(sinf(fa) * 8 * k) - K(3);
        limb(rx, ry, ex, ey, lw + 1, body);
        limb(ex, ey, hx, hy, lw, accent);
        fb.fillCircle(hx, hy, lw, accent);                 // the claw tip
    };
    arm(-1, P.armL); arm(1, P.armR);

    // prothorax neck, tilting with the head
    int neckTopY = headY + K(5), neckBotY = thoraxY + 1;
    int neckLen = neckBotY - neckTopY; if (neckLen < K(6)) neckLen = K(6);
    int ntx = cxB + K(P.headTilt), nbx = cxB;
    for (int i = 0; i <= neckLen; i++) {
        float f = (float)i / neckLen; int y = neckBotY - i;
        int hw = (int)((3.0f - f) * k); if (hw < 2) hw = 2;
        fb.drawFastHLine(nbx + (int)((ntx - nbx) * f) - hw, y, hw * 2, body);
    }
    thick(nbx, neckBotY, ntx, neckTopY, 1, dark);

    // triangular head with big eyes (turned away mid-spin)
    int hx = cxB + K(P.headTilt);
    fb.fillTriangle(hx - (int)(6*k*sx) - 3, headY + K(6) + 1, hx + (int)(6*k*sx) + 3, headY + K(6) + 1, hx, headY - K(4) - 3, edge);
    fb.fillTriangle(hx - (int)(6*k*sx) - 1, headY + K(6), hx + (int)(6*k*sx) + 1, headY + K(6), hx, headY - K(4), body);
    if (!back) {
        bool blink = (t % 2600) < 120;
        int ey = headY + K(2), ex = (int)(3 * k * sx);
        if (blink) { thick(hx - ex - K(2), ey, hx - ex + K(1), ey, 2, eyeC); thick(hx + ex - K(1), ey, hx + ex + K(2), ey, 2, eyeC); }
        else {
            fb.fillCircle(hx - ex, ey, K(2), accent); fb.fillCircle(hx + ex, ey, K(2), accent);
            fb.fillCircle(hx - ex, ey, K(1), eyeC);   fb.fillCircle(hx + ex, ey, K(1), eyeC);
            fb.drawPixel(hx - ex - 1, ey - K(1), LIME_PALE); fb.drawPixel(hx + ex - 1, ey - K(1), LIME_PALE);
        }
        thick(hx - K(2), headY + K(6), hx + K(2), headY + K(6), 2, accent);   // a happy dancer
    }
    float aw = sinf(ts * 5.0f) * 3.0f;
    limb(hx - K(3), headY - K(3), hx - K(6) + K(aw), headY - K(12), 2, body);
    limb(hx + K(3), headY - K(3), hx + K(6) - K(aw), headY - K(12), 2, body);
    fb.fillCircle(hx - K(6) + K(aw), headY - K(12), 2, accent);
    fb.fillCircle(hx + K(6) - K(aw), headY - K(12), 2, accent);
}

// ---- beat-driven backgrounds (theme colours only), a new style every bar ----
static void bgSunburst(float ph, long beatNo, float hit) {
    fb.fillSprite(TEAL_DEEP);
    int cx = W / 2, cy = 150; float rot = (beatNo + ph) * 0.26f;
    for (int i = 0; i < 12; i++) {
        float a0 = rot + i * 0.5236f, a1 = a0 + 0.2618f;
        uint16_t c = (i & 1) ? (hit > 0.6f ? PURPLE : PURPLE_DARK) : TEAL_DARK;
        fb.fillTriangle(cx, cy, cx + (int)(cosf(a0) * 260), cy + (int)(sinf(a0) * 260),
                        cx + (int)(cosf(a1) * 260), cy + (int)(sinf(a1) * 260), c);
    }
    fb.fillCircle(cx, cy, 18 + (int)(hit * 8), hit > 0.6f ? LIME_DARK : PURPLE_DEEP);
}
static void bgRings(float ph, long beatNo, float hit) {
    fb.fillSprite(PURPLE_DEEP);
    static const uint16_t RC[4] = { TEAL_DARK, PURPLE_DARK, LIME_DARK, TEAL };
    for (int i = 6; i >= 0; i--) {                       // rings ride outward each beat
        int r = (int)((i + ph) * 34);
        fb.drawCircle(W / 2, 150, r, RC[(i + beatNo) & 3]);
        fb.drawCircle(W / 2, 150, r + 1, RC[(i + beatNo) & 3]);
        if (i == 0 && hit > 0.7f) fb.drawCircle(W / 2, 150, r + 3, LIME);
    }
}
static void bgEqualizer(float ph, long beatNo, float hit) {
    fb.fillSprite(TEAL_DEEP);
    for (int i = 0; i < 10; i++) {                       // bars jump on the beat, decay between
        uint32_t h = (uint32_t)(beatNo * 2654435761u) ^ (uint32_t)(i * 40503u);
        int top = 40 + (int)((h >> 8) % 120);
        int len = (int)((300 - top) * (0.35f + 0.65f * hit));
        uint16_t c = (i % 3 == 0) ? PURPLE_DARK : (i % 3 == 1) ? TEAL_DARK : LIME_DARK;
        fb.fillRect(i * 17 + 1, 300 - len, 15, len, c);
        fb.fillRect(i * 17 + 1, 300 - len, 15, 3, hit > 0.6f ? LIME_PALE : TEAL_LIGHT);
    }
}
static void bgDisco(float ph, long beatNo, float hit) {
    fb.fillSprite(TEAL_DEEP);
    for (int r = 0; r < 6; r++)                          // perspective dance floor
        for (int c = 0; c < 6; c++) {
            int y0 = 220 + r * r * 3 + r * 4, y1 = 220 + (r+1) * (r+1) * 3 + (r+1) * 4;
            bool on = ((r + c + beatNo) & 1);
            fb.fillRect(c * 29, y0, 29, y1 - y0, on ? (hit > 0.5f ? PURPLE : PURPLE_DARK) : TEAL_DARK);
        }
    fb.fillCircle(W / 2, 30, 16, TEAL);                  // the mirror ball
    for (int i = 0; i < 18; i++) {
        float a = i * 0.349f + (beatNo + ph) * 0.4f;
        int x = W / 2 + (int)(cosf(a) * (40 + (i % 5) * 18)), y = 90 + (int)(sinf(a) * (30 + (i % 4) * 20));
        if (((i + beatNo) % 3) != 0) fb.fillCircle(x, y, hit > 0.6f ? 2 : 1, (i & 1) ? LIME_PALE : PURPLE_LIGHT);
    }
}
static void bgIdle(uint32_t now) {                       // no live beat: calm, slow, waiting
    fb.fillSprite(TEAL_DEEP);
    for (int i = 0; i < 20; i++) {
        int x = (i * 53) % W, y = (i * 97 + (int)(now / 80)) % 280;
        fb.drawPixel(x, y, (i % 3) ? TEAL_DARK : PURPLE_DARK);
    }
}

// ===========================================================================
//  tick / draw / lifecycle
// ===========================================================================
void dancepartyTick() {
    chordGuardTick();
    ++frameNo;
    for (int g = 0; g < 8; g++) {                        // drain received frames
        RxSlot& s = rx[rxR % 8];
        if (!s.full) break;
        handle(s.data, s.len);
        s.full = 0; rxR = (uint8_t)(rxR + 1);
    }
    uint32_t now = millis();
    (void)lastAutoBeat;                               // no invented groove any more
    if (hype) --hype;
    // a Core2 that starts a party pulls us straight onto the floor
    bool hostBeating = synced() && lastBeatMs && now - lastBeatMs < 3000;
    if ((dscr == DS_WELCOME || dscr == DS_LOBBY) && hostBeating) { dscr = DS_DANCE; partyOn = true; }
    if (dscr == DS_DANCE && !synced()) dscr = DS_LOBBY;   // host gone: wait for it again
    if (now - enterAt < 250) return;
    bool lE = chordLEdge(), rE = chordREdge();
    // L: tap = next / nudge move, hold = back to the welcome screen
    if (lE) { lHeldDown = true; lDownAt = now; lLong = false; }
    if (lHeldDown && btnL && !lLong && now - lDownAt > 420) {
        lLong = true;
        if (dscr != DS_WELCOME) { dscr = DS_WELCOME; bpm = 0; lastBeatMs = 0; tapN = 0; }
    }
    bool lTap = lHeldDown && !btnL && !lLong;
    if (!btnL) lHeldDown = false;
    switch (dscr) {
    case DS_WELCOME:
        if (lTap) menuSel ^= 1;
        if (rE) {
            if (menuSel == 0) dscr = DS_LOBBY;
            else { dscr = DS_TAP; bpm = 0; tapN = 0; move = MV_BOB; }
        }
        break;
    case DS_LOBBY:
        break;                                        // waits for the Synapse (or hold L)
    case DS_DANCE:
        if (lTap) move = (uint8_t)((move + 1) % MV_COUNT);
        if (rE) hype = 60;
        break;
    case DS_TAP:
        if (lTap) move = (uint8_t)((move + 1) % MV_COUNT);
        if (rE) {
            if (tapN && now - taps[tapN - 1] > 2000) tapN = 0;      // long pause: start a new count
            if (tapN == 6) { memmove(taps, taps + 1, sizeof(uint32_t) * 5); tapN = 5; }
            taps[tapN++] = now;
            if (tapN >= 2) {
                float gap = (float)(taps[tapN - 1] - taps[0]) / (float)(tapN - 1);
                float b = 60000.0f / gap;
                if (b >= 40 && b <= 220) { bpm = b; beatAnchor = now; barAnchor = 0; lastBeatMs = now; }
            }
            hype = 20;
        }
        break;
    }
}

static void dpFooter(const char* l, const char* r) {
    fb.fillRect(0, H - 14, W, 14, TEAL_DARK);
    fb.setTextColor(TEAL_LIGHT);
    fb.setTextDatum(ML_DATUM); fb.drawString(l, 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString(r, W - 4, H - 7, 1);
}

static void drawWelcome(uint32_t now) {
    fb.fillSprite(TEAL_DEEP);
    for (int i = 0; i < 24; i++) {                    // slow drifting dots, like the other menus
        int x = (i * 53 + (int)(now / 60)) % W, y = 60 + (i * 37) % 70;
        fb.drawPixel(x, y, (i % 3) ? TEAL_DARK : PURPLE_DARK);
    }
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("DANCE", W / 2, 26, 4);
    fb.drawString("PARTY", W / 2, 48, 4);
    // hero: the dancer, small, bobbing to a lazy beat
    float ph = (float)(now % 700) / 700.0f;
    DancePose P = poseFor(MV_ARMSUP, ph, (long)(now / 700), 0.6f);
    drawDancer(W / 2, 128, 0.9f, now, P);
    static const char* ITEMS[2] = { "DANCE WITH CORE2", "TAP TEMPO" };
    const char* sub[2] = { synced() ? "SYNAPSE is here!" : (radioUp ? "listening for a Synapse" : "radio off"),
                           "tap R to the beat" };
    for (int i = 0; i < 2; i++) {
        int y = 150 + i * 56;
        bool sel = (i == menuSel);
        fb.fillRoundRect(10, y, W - 20, 46, 5, sel ? TEAL_DARK : TEAL_DEEP);
        fb.drawRoundRect(10, y, W - 20, 46, 5, sel ? LIME : TEAL_DARK);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(sel ? LIME_PALE : TEAL_BRIGHT);
        fb.drawString(ITEMS[i], W / 2, y + 16, 2);
        fb.setTextColor(sel ? TEAL_LIGHT : TEAL_DARK);
        fb.drawString(sub[i], W / 2, y + 33, 1);
    }
    dpFooter("L: next", "R: select");
}

static void drawLobby(uint32_t now) {
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("FINDING THE", W / 2, 50, 2);
    fb.drawString("SYNAPSE", W / 2, 72, 4);
    float a = (now % 2000) * 0.00314f;               // radar sweep, same as the versus lobbies
    fb.drawCircle(W / 2, 160, 44, TEAL_DARK);
    fb.drawCircle(W / 2, 160, 28, TEAL_DARK);
    fb.drawCircle(W / 2, 160, 12, TEAL_DARK);
    fb.drawLine(W / 2, 160, W / 2 + (int)(cosf(a) * 44), 160 + (int)(sinf(a) * 44), LIME);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("on the Core2: mantis mode,", W / 2, 228, 1);
    fb.drawString("then HOLD C for party mode", W / 2, 242, 1);
    fb.setTextColor(TEAL); fb.drawString(radioUp ? "listening on the mesh" : "radio off", W / 2, 266, 1);
    dpFooter("hold L: back", "");
}

void dancepartyDraw() {
    if (dscr == DS_WELCOME) { drawWelcome(millis()); fb.pushSprite(0, 0); return; }
    if (dscr == DS_LOBBY)   { drawLobby(millis());   fb.pushSprite(0, 0); return; }
    uint32_t now = millis();
    long beatNo; float ph; beatPos(now, beatNo, ph);
    bool live = beatLive();
    float hit = live ? 1.0f - ph : 0.0f;
    if (live) {
        switch ((beatNo / 4) & 3) {                      // new background every bar
        case 0: bgSunburst(ph, beatNo, hit); break;
        case 1: bgRings(ph, beatNo, hit); break;
        case 2: bgEqualizer(ph, beatNo, hit); break;
        default: bgDisco(ph, beatNo, hit); break;
        }
    } else bgIdle(now);

    uint8_t mv = live ? move : (uint8_t)MV_SWAY;          // no beat: an easy idle sway
    float inten = live ? intensity / 255.0f : 0.35f;
    DancePose P = poseFor(mv, ph, beatNo, inten);
    drawDancer(W / 2, 306, 3.0f, now, P);                 // big: fills most of the screen

    if (hype) for (int i = 0; i < 12; i++) {              // R: hype sparkles
        float a = i * 0.5236f + frameNo * 0.1f; int r = 90 - hype;
        fb.fillCircle(W/2 + (int)(cosf(a) * r), 170 + (int)(sinf(a) * r * 0.8f), 3, (i & 1) ? LIME : PURPLE_LIGHT);
    }
    // a slim status strip - no meters, just who we're dancing with
    fb.setTextDatum(TC_DATUM);
    if (dscr == DS_TAP) {
        char b[40];
        if (bpm >= 40) snprintf(b, sizeof(b), "TAP TEMPO  %d bpm", (int)(bpm + 0.5f));
        else snprintf(b, sizeof(b), "tap R to the beat");
        fb.setTextColor(LIME_PALE); fb.drawString(b, W/2, 3, 1);
        if (live) { int inBar = (int)((barAnchor + beatNo) & 3);
            for (int i = 0; i < 4; i++) fb.fillCircle(W/2 - 18 + i * 12, 16, 3, i == inBar ? LIME : TEAL_DARK); }
    } else if (live) {
        char b[40]; snprintf(b, sizeof(b), "%s  %d bpm", hostCall[0] ? hostCall : "SYNAPSE", (int)bpm);
        fb.setTextColor(LIME_PALE); fb.drawString(b, W/2, 3, 1);
        int inBar = (int)((barAnchor + beatNo) & 3);
        for (int i = 0; i < 4; i++) fb.fillCircle(W/2 - 18 + i * 12, 16, 3, i == inBar ? LIME : TEAL_DARK);
    } else {
        fb.setTextColor(synced() ? TEAL_LIGHT : PURPLE_LIGHT);
        fb.drawString(synced() ? "waiting for the music..." : (radioUp ? "waiting for the Synapse" : "radio off"), W/2, 3, 1);
        fb.setTextColor(TEAL); fb.drawString("put on a song by the Core2", W/2, 15, 1);
    }
    fb.pushSprite(0, 0);
}

void dancepartyEnter() {
    fb.setRotation(0);
    chordGuardReset();
    memset((void*)rx, 0, sizeof(rx)); rxW = 0; rxR = 0;
    memset(dedupNode, 0, sizeof(dedupNode)); memset(dedupPkt, 0, sizeof(dedupPkt));
    bpm = 0.0f; beatAnchor = millis(); lastBeatMs = 0; barAnchor = 0; move = MV_BOB; intensity = 180;
    lastHostMs = 0; hostCall[0] = 0; partyOn = true; hype = 0; frameNo = 0; lastAutoBeat = 0;
    dscr = DS_WELCOME; menuSel = 0; tapN = 0; lHeldDown = false;
    radioStart();
    enterAt = millis();
}

void dancepartyShutdown() {
    radioStop();
}
