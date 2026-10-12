// versus_mode.cpp - Two-player air-hockey pinball over ESP-NOW.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include "versus_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;
static constexpr float VS_BALL_R  = 4.5f;
static constexpr float VS_GRAVITY = 0.16f;
static constexpr float VS_MAXV    = 7.5f;   // tighter cap: fast balls tunnel through thin flippers

// ---- Flipper geometry (mirrors Corner Pocket's feel) -------------------------
// Paddles sit near the bottom, spaced wider apart than single-player to make
// it more challenging, and rotate/respond like the Corner Pocket flippers.
static constexpr float VSF_LEN        = 46.0f;    // longer paddles - smaller drain gap
static constexpr float VSF_THICK      = 3.0f;
static constexpr float VSF_REST_DEG   = 28.0f;
static constexpr float VSF_ACT_DEG    = -32.0f;   // swing up when pressed
static constexpr float VSF_LERP_UP    = 0.55f;
static constexpr float VSF_LERP_DOWN  = 0.30f;
static constexpr float VSF_Y          = (float)H - 30.0f;
static constexpr float VSF_GAP        = 20.0f;    // half-gap between paddles (tighter drain)
static constexpr float DEG2RAD        = 3.14159265f / 180.0f;

struct VsFlip {
    float pivotX, pivotY;
    float angle, prevAngle;
    float restAngle, activeAngle;
    int   sign;       // +1 right paddle, -1 left paddle (mirrors angle)
    bool  active;
};
static VsFlip vsFlipL, vsFlipR;

// ---- Powerup target box (one per screen) -------------------------------------
// A green target box holds a random powerup (same kinds as Pinball Blaster:
// 0 slow, 1 multi, 2 boost, 3 projectile, 4 powerflip). Hitting it collects it.
struct VsTarget { float x, y, w, h; uint8_t kind; bool active; };
static VsTarget vsTarget;
static const int VS_PWR_R = 6;      // powerup orb radius (matches Pinball Blaster)
static int      vsPowerHeld = -1;   // collected powerup kind (-1 none)
static uint32_t vsNoBallSince = 0;  // when the screen first went ball-less (grace timer)
static uint32_t vsPowerUntil = 0;   // when a timed powerup expires

// Forward declarations (these helpers are defined later in the file but are
// used by the lifecycle functions above them).
static void vsInitFlippers();
static void vsStepFlipper(VsFlip& f, bool pressed);
static void vsSpawnTarget();
static void vsStepBall();
static void vsServeOwnBall();
static void vsInitBumpers();
static void vsInitWalls();

// ---- ESP-NOW link state ------------------------------------------------------
static const uint8_t VS_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint8_t  vsPeer[6]    = {0};
static bool     vsPeerKnown  = false;
static bool     vsLinked     = false;
static bool     vsEspReady   = false;
static uint16_t vsMyId       = 0;     // last 2 bytes of our MAC, for display
static uint16_t vsPeerId     = 0;

// Match phases
enum VsPhase { VS_SEARCH, VS_CONFIRM, VS_PLAY, VS_OVER };
static VsPhase vsPhase = VS_SEARCH;

// ---- Message protocol --------------------------------------------------------
enum VsMsgType : uint8_t {
    VM_HELLO    = 1,   // discovery beacon (carries id)
    VM_CONFIRM  = 2,   // "I pressed confirm"
    VM_START    = 3,   // host says go (carries who serves first)
    VM_BALL     = 4,   // ball handoff: x, vx, vy (ball leaving my top -> your top)
    VM_SCORE    = 5,   // score update: myScore, yourScore (from sender's view)
    VM_BYE      = 6,   // leaving
};
struct __attribute__((packed)) VsMsg {
    uint8_t  type;
    uint16_t id;
    uint8_t  confirmed;     // for VM_CONFIRM/START
    float    x, vx, vy;     // for VM_BALL (x is 0..W in sender's frame)
    uint8_t  sMe, sYou;     // for VM_SCORE
};

// Incoming-event flags set by the (fast) recv callback, consumed in tick()
static volatile bool vsRxHello   = false;
static volatile bool vsRxConfirm = false;
static volatile bool vsRxStart   = false;
static volatile bool vsRxBall    = false;
static volatile uint16_t vsLastBallId = 0xFFFF;   // dedup for VM_BALL handoffs
static volatile bool vsRxScore   = false;
static volatile bool vsRxBye     = false;
static volatile float vsRxBallX = 0, vsRxBallVx = 0, vsRxBallVy = 0;
static volatile uint8_t vsRxSMe = 0, vsRxSYou = 0;
static volatile uint16_t vsRxId = 0;
static uint8_t vsRxMac[6] = {0};

// ---- Game state --------------------------------------------------------------
struct VsBall { float x, y, vx, vy; bool on; };
static constexpr int VS_MAX_BALLS = 2;   // your own ball + one crossed over
static VsBall vsBalls[VS_MAX_BALLS];     // balls currently on MY screen
static int    vsMyDrains  = 0;           // balls I've lost out my bottom
static bool   vsRewarded  = false;       // win perk granted this match?
static int    vsOppDrains = 0;           // balls the opponent has lost
static const int VS_LOSE_AT = 3;         // first to lose 3 of their own balls loses
static bool   vsIsHost = false;          // higher id hosts (tie-breaks only)
static bool   vsConfirmedLocal = false;
static bool   vsConfirmedPeer  = false;
static uint32_t vsLastHello = 0;

// Two mirrored round bumpers per screen (Pinball Blaster style), identical on
// both devices so the field is symmetric.
struct VsBumper { float x, y, r; int8_t flash; };
static VsBumper vsBumpers[2];

// Helpers
static int  vsBallCount();
static bool vsSpawnBall(float x, float y, float vx, float vy);

// ---- ESP-NOW plumbing --------------------------------------------------------
static void vsOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < (int)sizeof(VsMsg)) {
        // tolerate short HELLO etc — only act on full struct
    }
    if ((size_t)len < sizeof(VsMsg)) return;
    const VsMsg* m = (const VsMsg*)data;
    memcpy(vsRxMac, mac, 6);
    vsRxId = m->id;
    switch (m->type) {
        case VM_HELLO:   vsRxHello = true;   break;
        case VM_CONFIRM: vsRxConfirm = true; break;
        case VM_START:   vsRxStart = true;   break;
        case VM_BALL:
            // ESP-NOW can deliver the same frame more than once; ignore a repeat
            // of the same ball-handoff id so we never spawn two balls from one
            // handoff (the "blinking in two places" glitch).
            if (m->id != vsLastBallId) {
                vsLastBallId = m->id;
                vsRxBallX = m->x; vsRxBallVx = m->vx; vsRxBallVy = m->vy;
                vsRxBall = true;
            }
            break;
        case VM_SCORE:
            vsRxSMe = m->sMe; vsRxSYou = m->sYou; vsRxScore = true; break;
        case VM_BYE:     vsRxBye = true;     break;
    }
}

static void vsSend(VsMsgType t, const uint8_t* to) {
    VsMsg m; memset(&m, 0, sizeof(m));
    m.type = t; m.id = vsMyId;
    if (t == VM_CONFIRM || t == VM_START) m.confirmed = 1;
    if (t == VM_SCORE) { m.sMe = vsMyDrains; m.sYou = vsOppDrains; }
    esp_now_send(to, (uint8_t*)&m, sizeof(m));
}

// Hand a ball off to the opponent: send its position/velocity already mirrored
// into their coordinate frame.
static void vsSendBall(const uint8_t* to, float x, float vx, float vy) {
    VsMsg m; memset(&m, 0, sizeof(m));
    m.type = VM_BALL; m.id = vsMyId;
    m.x = x; m.vx = vx; m.vy = vy;
    esp_now_send(to, (uint8_t*)&m, sizeof(m));
}

static bool vsAddPeer(const uint8_t* mac) {
    esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.encrypt = false;
    if (esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p) == ESP_OK;
}

static bool vsInitEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Derive a short id from our MAC
    uint8_t mac[6]; WiFi.macAddress(mac);
    vsMyId = (mac[4] << 8) | mac[5];
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(vsOnRecv));
    vsAddPeer(VS_BCAST);
    return true;
}

// ---- Lifecycle ---------------------------------------------------------------
void versusEnter() {
    fb.setRotation(0);
    vsPhase = VS_SEARCH;
    vsPeerKnown = vsLinked = false;
    vsConfirmedLocal = vsConfirmedPeer = false;
    vsMyDrains = vsOppDrains = 0;
    for (int i=0;i<VS_MAX_BALLS;i++) vsBalls[i].on=false;
    vsInitBumpers();
    vsInitWalls();
    vsRxHello = vsRxConfirm = vsRxStart = vsRxBall = vsRxScore = vsRxBye = false;
    vsEspReady = vsInitEspNow();
    vsLastHello = 0;
    vsInitFlippers();
    vsTarget.active = false;
    vsPowerHeld = -1; vsPowerUntil = 0;
    vsNoBallSince = 0;

    // Starting advantage: consume any pinball perks the player has banked, and
    // apply the most useful one as a held versus power for the first stretch.
    // (Versus powers: 0=slow, 2=boost, 4=powerflip — mirror the pinball perks.)
    vsRewarded = false;
    uint32_t perks = qpConsumePerks();
    if (perks & PERK_POWERFLIP)      { vsPowerHeld = 4; vsPowerUntil = millis() + 12000; }
    else if (perks & PERK_SLOWBALL)  { vsPowerHeld = 0; vsPowerUntil = millis() + 12000; }
    else if (perks & PERK_BOOST)     { vsPowerHeld = 2; vsPowerUntil = millis() + 12000; }
}

void versusShutdown() {
    if (vsEspReady) {
        if (vsPeerKnown) vsSend(VM_BYE, vsPeer);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        vsEspReady = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ---- Flipper helpers (segment-based, like Corner Pocket) --------------------
static void vsInitFlippers() {
    // EXACTLY Corner Pocket's model (see initFlippers in main.cpp):
    //   left  flipper: restAngle = +REST, activeAngle = +ACT, sign +1
    //   right flipper: restAngle = pi-REST, activeAngle = pi-ACT, sign -1
    // Only the pivot X differs; no custom sign-mirroring math.
    float restRad = VSF_REST_DEG * DEG2RAD;
    float actRad  = VSF_ACT_DEG  * DEG2RAD;
    float restCos = cosf(restRad);

    vsFlipL.pivotX = W * 0.5f - VSF_GAP - VSF_LEN * restCos;
    vsFlipL.pivotY = VSF_Y;
    vsFlipL.restAngle   = restRad;
    vsFlipL.activeAngle = actRad;
    vsFlipL.angle = vsFlipL.prevAngle = vsFlipL.restAngle;
    vsFlipL.sign = +1; vsFlipL.active = false;

    vsFlipR.pivotX = W * 0.5f + VSF_GAP + VSF_LEN * restCos;
    vsFlipR.pivotY = VSF_Y;
    vsFlipR.restAngle   = (float)M_PI - restRad;
    vsFlipR.activeAngle = (float)M_PI - actRad;
    vsFlipR.angle = vsFlipR.prevAngle = vsFlipR.restAngle;
    vsFlipR.sign = -1; vsFlipR.active = false;
}

static void vsStepFlipper(VsFlip& f, bool pressed) {
    f.prevAngle = f.angle;
    f.active = pressed;
    float target = pressed ? f.activeAngle : f.restAngle;
    float amt = pressed ? VSF_LERP_UP : VSF_LERP_DOWN;
    f.angle += (target - f.angle) * amt;
}

static void vsFlipEnds(const VsFlip& f, float& tipX, float& tipY) {
    tipX = f.pivotX + cosf(f.angle) * VSF_LEN;
    tipY = f.pivotY + sinf(f.angle) * VSF_LEN;
}

static void vsClosestOnSeg(float x1, float y1, float x2, float y2,
                           float px, float py, float& cx, float& cy) {
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx*dx + dy*dy;
    if (len2 < 0.0001f) { cx = x1; cy = y1; return; }
    float t = ((px - x1)*dx + (py - y1)*dy) / len2;
    if (t < 0) t = 0; if (t > 1) t = 1;
    cx = x1 + t*dx; cy = y1 + t*dy;
}

// Collide the ball with a flipper — EXACTLY Corner Pocket's collideFlipper
// (the boosted branch), which is what gives the ball enough power to climb the
// screen. The previous kick of 9.0 was ~14x too weak.
static void vsCollideFlipper(VsFlip& f, VsBall& b) {
    float tipX, tipY; vsFlipEnds(f, tipX, tipY);
    float cx, cy;
    vsClosestOnSeg(f.pivotX, f.pivotY, tipX, tipY, b.x, b.y, cx, cy);

    float pdx = b.x - cx, pdy = b.y - cy;
    float minDist = VS_BALL_R + VSF_THICK;
    float d2 = pdx*pdx + pdy*pdy;
    if (d2 >= minDist * minDist) return;

    float d = sqrtf(d2);
    float nx, ny;
    if (d > 0.001f) { nx = pdx / d; ny = pdy / d; }
    else {
        float dx = tipX - f.pivotX, dy = tipY - f.pivotY;
        float len = sqrtf(dx*dx + dy*dy);
        nx = -dy / len; ny = dx / len;
    }

    // Anti-trap: don't let an active/extended flipper trap the ball under it.
    float midAngle = (f.restAngle + f.activeAngle) * 0.5f;
    bool extended = (f.sign > 0) ? (f.angle < midAngle) : (f.angle > midAngle);
    if (ny > 0 && (f.active || extended)) {
        nx = -nx; ny = -ny;
        b.x = cx + nx * (minDist + 0.5f);
        b.y = cy + ny * (minDist + 0.5f);
    }

    float pen = minDist - d;
    b.x += nx * pen;
    b.y += ny * pen;

    // Reflect velocity (Corner Pocket uses 0.92 normal / 0.97 tangential).
    float vn = b.vx * nx + b.vy * ny;
    if (vn < 0) {
        float tx = -ny, ty = nx;
        float vt = b.vx * tx + b.vy * ty;
        vn = -vn * 0.92f;
        vt =  vt * 0.97f;
        b.vx = nx * vn + tx * vt;
        b.vy = ny * vn + ty * vt;
    }

    // Tangential kick at contact radius. Versus needs MORE than Corner Pocket
    // (a perfect CP swing barely reaches the top; here the ball must cross the
    // whole screen), so the gain and cap are higher.
    float omega = f.angle - f.prevAngle;
    float rx = cx - f.pivotX;
    float ry = cy - f.pivotY;
    float tkx = -ry;
    float tky =  rx;
    float baseGain = 200.0f;   // > Corner Pocket's 130 for full-screen travel
    float kickGain = (vsPowerHeld == 4) ? baseGain * 1.5f : baseGain;
    b.vx += tkx * omega * kickGain;
    b.vy += tky * omega * kickGain;

    // Also guarantee a minimum upward launch on a clean active strike so timing
    // a swing reliably sends the ball up and over, not just a weak tap.
    if (f.active && omega != 0.0f && b.vy > -7.0f) b.vy = -9.0f;

    // Speed cap (higher than Corner Pocket so the launch survives).
    float sp2 = b.vx * b.vx + b.vy * b.vy;
    float capSp = VS_MAXV * (vsPowerHeld == 4 ? 2.2f : 18.0f);
    if (sp2 > capSp * capSp) {
        float s = capSp / sqrtf(sp2);
        b.vx *= s; b.vy *= s;
    }
}

static void vsSpawnTarget() {
    vsTarget.w = VS_PWR_R * 2; vsTarget.h = VS_PWR_R * 2;
    vsTarget.x = 20 + (esp_random() % (int)(W - 40));
    vsTarget.y = 55 + (esp_random() % 90);     // upper-middle area
    vsTarget.kind = esp_random() % 5;
    vsTarget.active = true;
}

// ---- Ball helpers ------------------------------------------------------------
static int vsBallCount() {
    int n = 0;
    for (int i = 0; i < VS_MAX_BALLS; i++) if (vsBalls[i].on) n++;
    return n;
}
static bool vsSpawnBall(float x, float y, float vx, float vy) {
    for (int i = 0; i < VS_MAX_BALLS; i++) {
        if (!vsBalls[i].on) {
            vsBalls[i].x = x; vsBalls[i].y = y;
            vsBalls[i].vx = vx; vsBalls[i].vy = vy;
            vsBalls[i].on = true;
            return true;
        }
    }
    return false;   // screen already full (2 balls)
}

// Serve YOUR own ball from the bottom-center, launched gently upward. Both
// players do this at the start; you also get a fresh ball after one drains.
static void vsServeOwnBall() {
    float vx = (esp_random() % 200 - 100) / 60.0f;
    vsSpawnBall(W / 2.0f, H - 60.0f, vx, -2.0f);
}

static void vsInitBumpers() {
    // Two mirrored round bumpers, symmetric across the vertical centerline,
    // in the upper-middle of the playfield (same on both devices).
    vsBumpers[0] = { W * 0.30f, 120.0f, 12.0f, 0 };
    vsBumpers[1] = { W * 0.70f, 120.0f, 12.0f, 0 };
}

// Angled inlane walls that funnel a ball coming down the sides onto the
// flippers (instead of letting it drain straight past them). Each is a line
// segment from a side wall down to just outside a flipper pivot.
struct VsWall { float ax, ay, bx, by; };
static VsWall vsWalls[2];
static void vsInitWalls() {
    float restCos = cosf(VSF_REST_DEG * DEG2RAD);
    float flL_x = W * 0.5f - VSF_GAP - VSF_LEN * restCos;
    float flR_x = W * 0.5f + VSF_GAP + VSF_LEN * restCos;
    float topY  = H * 0.62f;
    vsWalls[0] = { 0.0f,      topY, flL_x - 2.0f, VSF_Y };   // left funnel
    vsWalls[1] = { (float)W,  topY, flR_x + 2.0f, VSF_Y };   // right funnel
}

static void vsCollideWalls(VsBall& b) {
    for (int i = 0; i < 2; i++) {
        VsWall& w = vsWalls[i];
        float cx, cy;
        vsClosestOnSeg(w.ax, w.ay, w.bx, w.by, b.x, b.y, cx, cy);
        float dx = b.x - cx, dy = b.y - cy;
        float d2 = dx*dx + dy*dy;
        float minD = VS_BALL_R + 2.0f;
        if (d2 < minD*minD && d2 > 0.0001f) {
            float d = sqrtf(d2);
            float nx = dx/d, ny = dy/d;
            b.x = cx + nx*minD; b.y = cy + ny*minD;
            float vn = b.vx*nx + b.vy*ny;
            if (vn < 0) { b.vx -= 1.9f*vn*nx; b.vy -= 1.9f*vn*ny; }
        }
    }
}

// Step one ball; returns: 0 stay, 1 crossed top (handed off), 2 drained bottom.
static int vsStepOneBall(VsBall& b) {
    float grav = (vsPowerHeld == 0 && millis() < vsPowerUntil) ? VS_GRAVITY * 0.5f
                                                               : VS_GRAVITY;
    b.vy += grav;

    // Hard velocity clamp BEFORE moving, so a single frame can never move the
    // ball more than a fraction of a flipper length (prevents tunneling).
    float sp = sqrtf(b.vx*b.vx + b.vy*b.vy);
    if (sp > VS_MAXV) { float s = VS_MAXV / sp; b.vx *= s; b.vy *= s; }

    // Substep the motion: never advance more than VS_BALL_R per sub-step, so the
    // ball always overlaps thin obstacles (flippers, walls) instead of skipping
    // past them between frames. This is the core fix for "clips through
    // collisions" and balls punching through the flippers.
    int steps = (int)(sp / VS_BALL_R) + 1;
    if (steps > 6) steps = 6;
    float inv = 1.0f / steps;

    for (int s = 0; s < steps; s++) {
        b.x += b.vx * inv;
        b.y += b.vy * inv;

        // Side walls
        if (b.x < VS_BALL_R) { b.x = VS_BALL_R; b.vx = -b.vx * 0.92f; }
        if (b.x > W - VS_BALL_R) { b.x = W - VS_BALL_R; b.vx = -b.vx * 0.92f; }

        // Round bumpers (Pinball Blaster style): reflect + add a little energy.
        for (int i = 0; i < 2; i++) {
            VsBumper& bm = vsBumpers[i];
            float dx = b.x - bm.x, dy = b.y - bm.y;
            float rr = VS_BALL_R + bm.r;
            float dd = dx*dx + dy*dy;
            if (dd < rr*rr && dd > 0.0001f) {
                float d = sqrtf(dd);
                float nx = dx/d, ny = dy/d;
                b.x = bm.x + nx * rr;
                b.y = bm.y + ny * rr;
                float vn = b.vx*nx + b.vy*ny;
                if (vn < 0) { b.vx -= 2.0f*vn*nx; b.vy -= 2.0f*vn*ny; }
                b.vx += nx * 1.6f; b.vy += ny * 1.6f;   // pop
                bm.flash = 6;
                if (!vsTarget.active && (esp_random() % 100) < 15) vsSpawnTarget();
            }
        }

        // Angled inlane walls funnel the ball toward the flippers.
        vsCollideWalls(b);

        // Flippers
        vsCollideFlipper(vsFlipL, b);
        vsCollideFlipper(vsFlipR, b);

        // Powerup orb pickup (circular)
        if (vsTarget.active) {
            float dx = b.x - vsTarget.x, dy = b.y - vsTarget.y;
            if (dx*dx + dy*dy < (VS_BALL_R + VS_PWR_R)*(VS_BALL_R + VS_PWR_R)) {
                vsPowerHeld = vsTarget.kind;
                vsTarget.active = false;
                vsPowerUntil = millis() + 8000;
                if (vsTarget.kind == 2) { b.vx *= 1.4f; b.vy *= 1.4f; }
                b.vy = -fabsf(b.vy);
            }
        }

        // early-out if the ball left the playfield mid-substep
        if (b.y < -VS_BALL_R) return 1;
        if (b.y > H + VS_BALL_R) return 2;
    }

    if (b.y < -VS_BALL_R) return 1;   // crossed top
    if (b.y > H + VS_BALL_R) return 2; // drained bottom
    return 0;
}

static void vsStepBall() {
    for (int i = 0; i < VS_MAX_BALLS; i++) {
        VsBall& b = vsBalls[i];
        if (!b.on) continue;
        int r = vsStepOneBall(b);
        if (r == 1) {
            // Crossed the top -> hand off to opponent, mirrored into their frame.
            if (vsLinked) {
                float hx  = W - b.x;
                float hvx = -b.vx;
                float hvy = fabsf(b.vy);
                const float ENTRY_MAX = 6.0f;
                if (hvx >  ENTRY_MAX) hvx =  ENTRY_MAX;
                if (hvx < -ENTRY_MAX) hvx = -ENTRY_MAX;
                if (hvy >  ENTRY_MAX) hvy =  ENTRY_MAX;
                if (hvy < 1.5f)       hvy = 1.5f;
                vsSendBall(vsPeer, hx, hvx, hvy);
            }
            b.on = false;   // leaves my screen
        } else if (r == 2) {
            // Drained my bottom -> I lost a ball. Re-serve and count the drain.
            b.on = false;
            vsMyDrains++;
            if (vsLinked) vsSend(VM_SCORE, vsPeer);
            if (vsMyDrains >= VS_LOSE_AT) { vsPhase = VS_OVER; }
            else { vsServeOwnBall(); }
        }
    }
    // Decay bumper flashes
    for (int i = 0; i < 2; i++) if (vsBumpers[i].flash > 0) vsBumpers[i].flash--;
}

// ---- Tick --------------------------------------------------------------------
void versusTick() {
    if (!vsEspReady) return;

    // --- Consume async RX events ---
    if (vsRxBye) {
        vsRxBye = false;
        if (vsPhase == VS_PLAY) vsPhase = VS_OVER;
    }
    if (vsRxHello) {
        vsRxHello = false;
        if (!vsPeerKnown) {
            memcpy(vsPeer, vsRxMac, 6);
            vsPeerId = vsRxId;
            vsAddPeer(vsPeer);
            vsPeerKnown = true;
            vsIsHost = (vsMyId > vsPeerId);   // deterministic host
            if (vsPhase == VS_SEARCH) vsPhase = VS_CONFIRM;
        }
    }
    if (vsRxConfirm) { vsRxConfirm = false; vsConfirmedPeer = true; }
    if (vsRxStart) {
        vsRxStart = false;
        if (vsPhase == VS_CONFIRM) {
            vsLinked = true; vsPhase = VS_PLAY;
            // Both players serve their own ball; serve mine now if I haven't.
            if (vsBallCount() == 0 && vsMyDrains == 0) {
                for (int i=0;i<VS_MAX_BALLS;i++) vsBalls[i].on=false;
                vsServeOwnBall();
            }
        }
    }
    if (vsRxBall) {
        vsRxBall = false;
        // Only accept a handed-off ball while actually playing. A stray or late
        // VM_BALL outside PLAY would otherwise spawn a frozen ball that just
        // hovers (it never gets stepped), which looked like the ball being in
        // two places at once.
        if (vsPhase == VS_PLAY) {
            // Clamp the entry X to the playfield so a mirrored handoff can't
            // spawn the ball inside/under a side wall.
            float ex = vsRxBallX;
            if (ex < VS_BALL_R)       ex = VS_BALL_R;
            if (ex > W - VS_BALL_R)   ex = W - VS_BALL_R;
            float evy = fabsf(vsRxBallVy);
            if (evy < 1.5f) evy = 1.5f;   // ensure it actually enters downward
            vsSpawnBall(ex, 6.0f, vsRxBallVx, evy);
        }
    }
    if (vsRxScore) {
        vsRxScore = false;
        // Each device OWNS its own drain count. The peer's message carries its
        // own drains in sMe; we only adopt that as the opponent's count and
        // never overwrite our own locally-counted drains.
        vsOppDrains = vsRxSMe;
        if (vsMyDrains >= VS_LOSE_AT || vsOppDrains >= VS_LOSE_AT) vsPhase = VS_OVER;
    }

    // --- Phase logic ---
    switch (vsPhase) {
        case VS_SEARCH:
            // Broadcast HELLO ~3x/sec
            if (millis() - vsLastHello > 300) {
                vsSend(VM_HELLO, VS_BCAST);
                vsLastHello = millis();
            }
            break;

        case VS_CONFIRM:
            // Keep broadcasting so the peer finds us too
            if (millis() - vsLastHello > 300) {
                vsSend(VM_HELLO, VS_BCAST);
                vsLastHello = millis();
            }
            // Either button confirms
            if ((btnL && !btnL_prev) || (btnR && !btnR_prev)) {
                if (!vsConfirmedLocal) {
                    vsConfirmedLocal = true;
                    vsSend(VM_CONFIRM, vsPeer);
                }
            }
            // Both confirmed -> match starts; BOTH players serve their own ball.
            if (vsConfirmedLocal && vsConfirmedPeer) {
                vsLinked = true;
                vsPhase = VS_PLAY;
                if (vsIsHost) vsSend(VM_START, vsPeer);
                for (int i=0;i<VS_MAX_BALLS;i++) vsBalls[i].on=false;
                vsServeOwnBall();          // each device serves its own ball
            }
            break;

        case VS_PLAY:
            // Drive the rotating flippers from the buttons every frame
            vsStepFlipper(vsFlipL, btnL);
            vsStepFlipper(vsFlipR, btnR);
            // Powerup drops are spawned by gameplay events (bumper hits), not on
            // a timer — see vsStepOneBall. Nothing to spawn here.
            // Safety net: if you've been completely ball-less for a short grace
            // period (e.g. your ball crossed over and none came back), serve a
            // fresh one so you can keep playing. The grace timer prevents a
            // same-frame double-serve right after a drain re-serve.
            if (vsBallCount() == 0 && vsMyDrains < VS_LOSE_AT) {
                if (vsNoBallSince == 0) vsNoBallSince = millis();
                else if (millis() - vsNoBallSince > 900) { vsServeOwnBall(); vsNoBallSince = 0; }
            } else {
                vsNoBallSince = 0;
            }
            vsStepBall();
            break;

        case VS_OVER:
            // On a win (you didn't hit the drain limit, opponent did), bank a
            // pinball perk as the prize — granted once per match.
            if (!vsRewarded) {
                vsRewarded = true;
                if (vsMyDrains < VS_LOSE_AT && vsOppDrains >= VS_LOSE_AT) {
                    static const uint32_t POOL[] = {
                        PERK_MULTIBALL, PERK_SLOWBALL, PERK_POWERFLIP, PERK_BOOST, PERK_PROJECTILE
                    };
                    qpGrantPerk(POOL[esp_random() % 5]);
                }
            }
            // Press either button to return to search for a rematch
            if ((btnL && !btnL_prev) || (btnR && !btnR_prev)) {
                vsMyDrains = vsOppDrains = 0;
                vsConfirmedLocal = vsConfirmedPeer = false;
                for (int i=0;i<VS_MAX_BALLS;i++) vsBalls[i].on=false;
                vsPhase = vsPeerKnown ? VS_CONFIRM : VS_SEARCH;
            }
            break;
    }
}

// ---- Draw --------------------------------------------------------------------
static void vsDrawOneFlipper(const VsFlip& f) {
    float tipX, tipY; vsFlipEnds(f, tipX, tipY);
    float dx = tipX - f.pivotX, dy = tipY - f.pivotY;
    float len = sqrtf(dx*dx + dy*dy); if (len < 0.001f) return;
    float nx = -dy/len, ny = dx/len;
    float t = VSF_THICK + 1.0f;
    int x0 = (int)(f.pivotX + nx*t), y0 = (int)(f.pivotY + ny*t);
    int x1 = (int)(f.pivotX - nx*t), y1 = (int)(f.pivotY - ny*t);
    int x2 = (int)(tipX - nx*t),     y2 = (int)(tipY - ny*t);
    int x3 = (int)(tipX + nx*t),     y3 = (int)(tipY + ny*t);
    uint16_t col = (vsPowerHeld == 4) ? LIME_PALE : (f.active ? LIME : LIME_DARK);
    fb.fillTriangle(x0, y0, x1, y1, x2, y2, col);
    fb.fillTriangle(x0, y0, x2, y2, x3, y3, col);
    fb.drawLine(x0, y0, x3, y3, LIME_PALE);
    fb.drawLine(x1, y1, x2, y2, LIME_PALE);
    fb.fillCircle((int)f.pivotX, (int)f.pivotY, (int)VSF_THICK + 1, col);
    fb.fillCircle((int)tipX, (int)tipY, (int)VSF_THICK + 1, col);
    fb.fillCircle((int)f.pivotX, (int)f.pivotY, 2, TEAL_BRIGHT);
}

static void vsDrawFlippers() {
    vsDrawOneFlipper(vsFlipL);
    vsDrawOneFlipper(vsFlipR);
}

static const char* vsPowerName(int kind) {
    switch (kind) {
        case 0: return "SLOW";
        case 1: return "MULTI";
        case 2: return "BOOST";
        case 3: return "PROJ";
        case 4: return "PWRFLIP";
    }
    return "";
}

// Per-kind powerup color + letter, matching Pinball Blaster's look.
static void vsPowerStyle(int kind, uint16_t& col, char& ch) {
    switch (kind) {
        case 0: col = TEAL_LIGHT;   ch = 'S'; break;  // slow
        case 1: col = PURPLE_LIGHT; ch = 'M'; break;  // multi
        case 2: col = LIME;         ch = 'B'; break;  // boost
        case 3: col = LIME_PALE;    ch = 'P'; break;  // projectile
        default:col = LIME_PALE;    ch = 'F'; break;  // powerflip
    }
}

static void vsDrawTarget() {
    if (!vsTarget.active) return;
    uint16_t col; char ch; vsPowerStyle(vsTarget.kind, col, ch);
    int cx = (int)vsTarget.x, cy = (int)vsTarget.y;
    fb.fillCircle(cx, cy, VS_PWR_R - 1, col);
    fb.drawCircle(cx, cy, VS_PWR_R, LIME_PALE);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TEAL_DEEP);
    fb.drawChar(ch, cx - 3, cy - 3, 1);
}

static void vsDrawScore() {
    char s[20];
    // Show balls LOST (drains) out of VS_LOSE_AT. Mine bottom-left, opp top-right.
    snprintf(s, sizeof(s), "%d", vsMyDrains);
    fb.setTextDatum(BL_DATUM); fb.setTextColor(LIME);
    fb.drawString(s, 6, H - 4, 4);
    snprintf(s, sizeof(s), "%d", vsOppDrains);
    fb.setTextDatum(TR_DATUM); fb.setTextColor(PURPLE_LIGHT);
    fb.drawString(s, W - 6, 20, 4);
    // Lives dots: filled = balls lost.
    for (int i = 0; i < VS_LOSE_AT; i++) {
        fb.fillCircle(W - 12 - i * 10, 6, 3, i < vsMyDrains ? LIME : TEAL_DARK);
        fb.fillCircle(12 + i * 10, H - 8, 3, i < vsOppDrains ? PURPLE_LIGHT : TEAL_DARK);
    }
}

static void vsDrawWalls() {
    for (int i = 0; i < 2; i++) {
        VsWall& w = vsWalls[i];
        fb.drawLine((int)w.ax, (int)w.ay, (int)w.bx, (int)w.by, PURPLE_LIGHT);
        fb.drawLine((int)w.ax+1, (int)w.ay, (int)w.bx+1, (int)w.by, PURPLE_DARK);
    }
}

static void vsDrawBumpers() {
    for (int i = 0; i < 2; i++) {
        VsBumper& b = vsBumpers[i];
        uint16_t outer = b.flash > 0 ? LIME_PALE : PURPLE;
        fb.fillCircle((int)b.x, (int)b.y, (int)b.r, outer);
        fb.fillCircle((int)b.x, (int)b.y, (int)b.r - 4, TEAL_BRIGHT);
        fb.drawCircle((int)b.x, (int)b.y, (int)b.r, LIME);
    }
}

void versusDraw() {
    fb.fillSprite(TEAL_DEEP);

    if (!vsEspReady) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("link radio", W/2, H/2 - 10, 2);
        fb.drawString("failed", W/2, H/2 + 10, 2);
        fb.pushSprite(0,0); return;
    }

    // Header
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("VERSUS PINBALL", 4, 8, 1);

    if (vsPhase == VS_SEARCH) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString("searching for", W/2, H/2 - 30, 2);
        fb.drawString("opponent...", W/2, H/2 - 10, 2);
        char id[20]; snprintf(id, sizeof(id), "your id: %04X", vsMyId);
        fb.setTextColor(TEAL_DARK); fb.drawString(id, W/2, H/2 + 20, 1);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("both enter this mode", W/2, H - 40, 1);
        // animated dots
        int dots = (millis() / 400) % 4;
        char d[5] = "    ";
        for (int i = 0; i < dots; i++) d[i] = '.';
        fb.setTextColor(LIME); fb.drawString(d, W/2, H/2 + 4, 2);
        fb.pushSprite(0,0); return;
    }

    if (vsPhase == VS_CONFIRM) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString("OPPONENT FOUND", W/2, 50, 2);
        char id[20]; snprintf(id, sizeof(id), "peer: %04X", vsPeerId);
        fb.setTextColor(TEAL_LIGHT); fb.drawString(id, W/2, 74, 1);

        // Confirm boxes
        fb.setTextColor(vsConfirmedLocal ? LIME : TEAL_BRIGHT);
        fb.drawRect(20, 110, W-40, 36, vsConfirmedLocal ? LIME : TEAL_DARK);
        fb.drawString(vsConfirmedLocal ? "YOU: READY" : "press to confirm",
                      W/2, 128, 2);
        fb.setTextColor(vsConfirmedPeer ? PURPLE_LIGHT : TEAL_DARK);
        fb.drawRect(20, 156, W-40, 36, vsConfirmedPeer ? PURPLE_LIGHT : TEAL_DARK);
        fb.drawString(vsConfirmedPeer ? "THEM: READY" : "waiting on them",
                      W/2, 174, 2);

        fb.setTextColor(TEAL_DARK);
        fb.drawString("both start with a ball", W/2, H - 30, 1);
        fb.pushSprite(0,0); return;
    }

    if (vsPhase == VS_OVER) {
        // You lose if YOU reached the drain limit; otherwise you win.
        bool win = vsMyDrains < VS_LOSE_AT;
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(win ? LIME : PURPLE_LIGHT);
        fb.drawString(win ? "YOU WIN!" : "YOU LOSE", W/2, H/2 - 30, 4);
        if (win) {
            fb.setTextColor(LIME_PALE);
            fb.drawString("+1 pinball perk earned!", W/2, H/2, 1);
        }
        char s[24]; snprintf(s, sizeof(s), "lost %d  -  %d lost", vsMyDrains, vsOppDrains);
        fb.setTextColor(TEAL_LIGHT); fb.drawString(s, W/2, H/2 + 4, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("press for rematch", W/2, H - 30, 1);
        fb.pushSprite(0,0); return;
    }

    // --- VS_PLAY ---
    // Center line (the "net")
    for (int x = 4; x < W - 4; x += 10)
        fb.drawFastHLine(x, H/2, 5, TEAL_DARK);

    vsDrawTarget();
    vsDrawWalls();
    vsDrawBumpers();
    vsDrawFlippers();
    vsDrawScore();

    // Held powerup indicator
    if (vsPowerHeld >= 0) {
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
        char pb[20]; snprintf(pb, sizeof(pb), "PWR: %s", vsPowerName(vsPowerHeld));
        fb.drawString(pb, W/2, 20, 1);
    }

    // Balls (up to VS_MAX_BALLS on this screen)
    int shown = 0;
    for (int i = 0; i < VS_MAX_BALLS; i++) {
        if (!vsBalls[i].on) continue;
        shown++;
        fb.fillCircle((int)vsBalls[i].x, (int)vsBalls[i].y, (int)VS_BALL_R + 1, TEAL_BRIGHT);
        fb.fillCircle((int)vsBalls[i].x, (int)vsBalls[i].y, (int)VS_BALL_R, LIME);
    }
    if (shown == 0) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("...", W/2, H/2 - 30, 2);
    }

    fb.pushSprite(0,0);
}
