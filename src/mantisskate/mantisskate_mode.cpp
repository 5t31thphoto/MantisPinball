// FILE: src/mantisskate/mantisskate_mode.cpp
// MANTIS SKATE - side-view flow skater (the OlliOlli school, not the Paperboy
// school). The draft's pseudo-3D 3-lane runner was rebuilt as a 2D line
// skater because that's the genre that actually FEELS like skateboarding on
// two buttons: everything is timing, and speed is earned.
//
// THE FLOW LOOP (why this feels like skating):
//   * R hold = crouch, R release = pop. Bigger crouch, bigger ollie.
//   * In the air: L tap = KICKFLIP, R tap = SHOVE-IT (each spins the board
//     for real frames - land mid-spin and you eat pavement), R held = GRAB
//     (pays the longer you hold, land still holding = bail).
//   * Hold L descending onto a rail = 50-50 GRIND. Sparks. Pays with speed.
//   * THE LANDING IS THE GAME: press R just before touchdown = PERFECT -
//     banks your combo x1.5 and pays +speed. No input = SLOPPY - half bank,
//     speed bleeds. Hold L through touchdown = MANUAL - the pot stays open
//     across the flat, wobble meter ticking, release to bank clean.
//   * Speed is the multiplier and the danger: reaction distance shrinks as
//     flow builds. Perfect landings are how you go fast. That's skateboarding.
//
// LEVEL FLOW: procedural street from pattern recipes (cones, hydrants, rails,
// gaps, stair sets, boxes, kickers, rail-over-gap), difficulty-tiered by
// distance, padding scaled by CURRENT speed so the course always arrives at
// the edge of readable - never empty (the draft spawned its first obstacle at
// the finish line), never unfair.
//
// VERSUS: 90-second same-seed score attack over ESP-NOW, house convention
// (HELLO 300ms / CONFIRM / roles by chip id / copy-only recv callback /
// seq-dedup double-sends / BYE + full teardown / 15s search timeout). Live
// rival score beats every second; both lines are identical because both run
// the host's seed. Radio never comes up in solo.
//
// ART: generated pixel sprite set (mantisskate_sprites.h) - 9 skater poses +
// tumble frames + flying board + cone/hydrant props. Parallax skyline, ground
// cracks, speed lines, landing shadow (the landing-timing affordance), dust,
// grind sparks, screen shake.
//
// ECONOMY: score pays no XP (pinball rule). Versus wins pay +30 XP / +8 pet.
// Eight one-time DECK MEDALS pay 15-50 XP from a persisted bitmask. NVS
// "mskate" writes at run end only + dirty-flag shutdown flush.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "mantisskate_mode.h"
#include "mantisskate_sprites.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ---- tuning -----------------------------------------------------------------
static constexpr int   SK_X        = 42;     // skater's fixed screen column
static constexpr int   GY          = 250;    // base ground line
static constexpr float SK_GRAV     = 0.22f;
static constexpr float SPD_MIN     = 2.0f;
static constexpr float SPD_START   = 2.3f;
static constexpr float SPD_MAX     = 3.8f;
static constexpr float SPD_PERFECT = 0.25f;
static constexpr float SPD_SLOPPY  = 0.30f;
static constexpr int   MAX_BAILS   = 3;
static constexpr int   MANUAL_MAX  = 96;     // frames before the wobble-out
static constexpr int   BOX_H       = 18;
static constexpr int   RAIL_H      = 16;     // rail top sits GY-RAIL_H
static constexpr uint32_t SK_LONG_MS    = 420;
static constexpr uint32_t SK_ENTER_GUARD= 250;
static constexpr uint32_t SK_HELLO_MS   = 300;
static constexpr uint32_t SK_SEARCH_MS  = 15000;
static constexpr uint32_t SK_STATUS_MS  = 1000;
static constexpr uint32_t SK_PEER_LOST  = 8000;
static constexpr int   VS_RUN_FRAMES = 90 * 62;   // 90 s at the 16 ms gate

// ---- persistence ------------------------------------------------------------
static const char* NVS_SK = "mskate";
struct __attribute__((packed)) SkSave {
    uint16_t magic;      // 0x4B53 'SK'
    uint8_t  version;
    uint8_t  ach;
    uint32_t bestScore;
    uint16_t bestDist;   // meters
    uint16_t runs;
    uint16_t vsWins;
};
static SkSave skSv;
static bool skDirty = false;

static void skDefaults() { memset(&skSv, 0, sizeof(skSv)); skSv.magic = 0x4B53; skSv.version = 1; }
static void skLoad() {
    skDefaults();
    Preferences p;
    if (!p.begin(NVS_SK, true)) return;
    SkSave t;
    size_t n = p.getBytes("blob", &t, sizeof(t));
    p.end();
    if (n == sizeof(t) && t.magic == 0x4B53 && t.version == 1) skSv = t;
}
static void skFlush() {
    if (!skDirty) return;
    Preferences p;
    if (!p.begin(NVS_SK, false)) return;
    p.putBytes("blob", &skSv, sizeof(skSv));
    p.end();
    skDirty = false;
}

// ---- medals -----------------------------------------------------------------
enum SkAch : uint8_t {
    ACH_FIRSTPOP = 0,   // first banked trick
    ACH_COMBO500,
    ACH_COMBO2000,
    ACH_PERFECT5,       // 5 perfect landings in one run
    ACH_RAILWIZ,        // 3 grinds inside one open combo
    ACH_MARATHON,       // 1500 m in one run
    ACH_DUELDECK,       // win a versus run
    ACH_LEGEND,         // 10 runs finished
};
struct SkAchDef { const char* name; const char* how; uint8_t xp; };
static const SkAchDef ACHS[8] = {
    { "FIRST POP",    "Bank your first trick",      15 },
    { "COMBO 500",    "Bank a 500+ combo",          25 },
    { "COMBO 2000",   "Bank a 2000+ combo",         50 },
    { "PERFECT FIVE", "5 perfect landings, 1 run",  30 },
    { "RAIL WIZARD",  "3 grinds in one combo",      40 },
    { "MARATHON",     "Skate 1500m in one run",     50 },
    { "DUEL DECK",    "Win a versus run",           50 },
    { "LOCAL LEGEND", "Finish 10 runs",             40 },
};
static uint8_t achQueue[4];
static int     achQueueN = 0;
static void skAward(uint8_t a) {
    if ((skSv.ach >> a) & 1) return;
    skSv.ach |= (uint8_t)(1 << a);
    qpAddXp(ACHS[a].xp);
    skDirty = true;
    if (achQueueN < 4) achQueue[achQueueN++] = a;
}

// ---- course features --------------------------------------------------------
enum FeatType : uint8_t { F_NONE = 0, F_CONE, F_HYDRANT, F_RAIL, F_GAP, F_BOX, F_KICKER, F_STAIRS };
struct Feat { uint8_t type; int32_t x; int16_t w; };
static constexpr int MAX_FEATS = 20;
static Feat* feats = nullptr;   // [MAX_FEATS] on the heap while the app is open
static int32_t genX = 0;                     // next world-x to generate at
static uint32_t runSeed = 1, rngS = 1;
static uint32_t skRng() { rngS = rngS * 1664525u + 1013904223u; return rngS >> 8; }

static void featClear() { for (int i = 0; i < MAX_FEATS; i++) feats[i].type = F_NONE; }
static void featAdd(uint8_t t, int32_t x, int16_t w) {
    // reuse the slot furthest behind the camera
    int best = 0; int32_t bx = feats[0].x;
    for (int i = 0; i < MAX_FEATS; i++) {
        if (feats[i].type == F_NONE) { best = i; break; }
        if (feats[i].x < bx) { bx = feats[i].x; best = i; }
    }
    feats[best] = { t, x, w };
}

// Ground level at world x (base line or a box top); <0 means open pit.
static int skGroundAt(int32_t wx) {
    for (int i = 0; i < MAX_FEATS; i++) {
        const Feat& f = feats[i];
        if (f.type == F_NONE) continue;
        if (wx < f.x || wx >= f.x + f.w) continue;
        if (f.type == F_BOX)    return GY - BOX_H;
        if (f.type == F_GAP)    return -1;
        if (f.type == F_STAIRS) return -1;
    }
    return GY;
}
// Rail top at world x, or -1.
static int skRailAt(int32_t wx) {
    for (int i = 0; i < MAX_FEATS; i++) {
        const Feat& f = feats[i];
        if (f.type == F_RAIL && wx >= f.x && wx < f.x + f.w) return GY - RAIL_H;
    }
    return -1;
}
// A solid curb-height obstacle (cone/hydrant/rail end) at world x?
static uint8_t skBlockAt(int32_t wx) {
    for (int i = 0; i < MAX_FEATS; i++) {
        const Feat& f = feats[i];
        if (f.type == F_CONE || f.type == F_HYDRANT) {
            if (wx >= f.x && wx < f.x + f.w) return f.type;
        } else if (f.type == F_RAIL) {
            // the entry/exit posts are solid; the span is grind-only overhead
            if ((wx >= f.x - 1 && wx < f.x + 2) || (wx >= f.x + f.w - 2 && wx < f.x + f.w + 1))
                return F_RAIL;
        }
    }
    return F_NONE;
}
static bool skKickerCross(int32_t prevX, int32_t nowX) {
    for (int i = 0; i < MAX_FEATS; i++) {
        const Feat& f = feats[i];
        if (f.type != F_KICKER) continue;
        int32_t lip = f.x + f.w;
        if (prevX < lip && nowX >= lip) return true;
    }
    return false;
}

// Pattern recipes. Padding scales with CURRENT speed so lookahead stays fair.
static float spd;                            // fwd-declared use: current speed
static void skGenPattern() {
    int tier = (int)(genX / 4200);           // ~every 420 m the street sharpens
    if (tier > 4) tier = 4;
    int pad = 96 + (int)(spd * 30.0f);
    int r = (int)(skRng() % 100);
    if (tier == 0 && r > 78) r = 40;         // early street stays gentle
    if (r < 12) {                                        // breather flat
        genX += pad + 90;
    } else if (r < 26) {                                 // cone
        featAdd(F_CONE, genX + pad, 8);
        genX += pad + 8 + 60;
    } else if (r < 36) {                                 // hydrant (needs charge)
        featAdd(F_HYDRANT, genX + pad, 9);
        genX += pad + 9 + 64;
    } else if (r < 44 && tier >= 1) {                    // cone pair
        featAdd(F_CONE, genX + pad, 8);
        featAdd(F_CONE, genX + pad + 52, 8);
        genX += pad + 120;
    } else if (r < 60) {                                 // rail
        int w = 96 + (int)(skRng() % 64);
        featAdd(F_RAIL, genX + pad, (int16_t)w);
        genX += pad + w + 70;
    } else if (r < 72) {                                 // gap
        int w = 36 + tier * 9 + (int)(skRng() % 10);
        featAdd(F_GAP, genX + pad, (int16_t)w);
        genX += pad + w + 70;
    } else if (r < 80) {                                 // stair set (deep gap)
        int w = 56 + tier * 6;
        featAdd(F_STAIRS, genX + pad, (int16_t)w);
        genX += pad + w + 74;
    } else if (r < 88) {                                 // box
        featAdd(F_BOX, genX + pad, 116);
        genX += pad + 116 + 70;
    } else if (r < 95 && tier >= 1) {                    // kicker into gap
        featAdd(F_KICKER, genX + pad, 26);
        featAdd(F_GAP, genX + pad + 40, (int16_t)(60 + tier * 6));
        genX += pad + 40 + 60 + tier * 6 + 84;
    } else if (tier >= 2) {                              // rail over a gap
        int gw = 104;
        featAdd(F_GAP, genX + pad + 18, (int16_t)gw);
        featAdd(F_RAIL, genX + pad, (int16_t)(gw + 40));
        genX += pad + gw + 40 + 80;
    } else {                                             // fallback: rail
        featAdd(F_RAIL, genX + pad, 110);
        genX += pad + 110 + 70;
    }
}
static void skGenUntil(int32_t upTo) {
    int guard = 0;
    while (genX < upTo && ++guard < 12) skGenPattern();
}

// ---- run state --------------------------------------------------------------
enum SkPose : uint8_t { P_ROLL, P_CROUCH, P_AIR, P_FLIP, P_SHOVE, P_GRAB, P_GRIND, P_MANUAL, P_BAIL };
enum AnimT  : uint8_t { AN_NONE = 0, AN_FLIP, AN_SHOVE };

static int32_t camX;                         // world scroll; skater wx = camX+SK_X
static float   camF;                         // float master for camX
static float   skY, vy;                      // board-contact point (screen y == world y)
static bool    onGround, grinding, manualOn;
static int     manualT;
static int     charge;                       // 0..26 crouch depth
static bool    charging;
static AnimT   anim; static int animT;
static bool    grabHeld; static int grabT;
static bool    landPrep;                     // R pressed inside the landing window
static bool    pendShove; static uint32_t pendShoveAt;
static float   pot; static float mult; static bool comboOpen;
static int     grindsInCombo;
static uint32_t score;
static int     bails, bailT;
static float   boardX, boardY, boardVx, boardVy;   // the flying deck
static int     perfects;
static int     rollF;                        // roll-bob phase
static uint32_t frameNo;

// ---- fx ----------------------------------------------------------------------
struct SkPart { float x, y, vx, vy; uint8_t life, kind; };  // kind 0 dust 1 spark 2 star
static SkPart* parts = nullptr;   // [24] on the heap while the app is open
static int    shakeT = 0;
static void fxSpawn(float x, float y, float vx, float vy, uint8_t life, uint8_t kind) {
    for (int i = 0; i < 24; i++)
        if (!parts[i].life) { parts[i] = { x, y, vx, vy, life, kind }; return; }
}
static void fxDust(float wx, float y, int n) {
    for (int i = 0; i < n; i++)
        fxSpawn(wx - (i * 2), y - 1, -0.6f - (i % 3) * 0.3f, -0.5f - (i % 2) * 0.4f, 12 + (i % 6), 0);
}
static void fxTick() {
    for (int i = 0; i < 24; i++) {
        if (!parts[i].life) continue;
        parts[i].vy += 0.06f;
        parts[i].x += parts[i].vx;
        parts[i].y += parts[i].vy;
        parts[i].life--;
    }
    if (shakeT) shakeT--;
}

// ---- popups ------------------------------------------------------------------
struct SkPop { char txt[16]; int x, y; uint8_t t; uint8_t big; };
static SkPop* pops = nullptr;   // [4] on the heap while the app is open
static void popup(const char* s, int x, int y, bool big = false) {
    int slot = 0; uint8_t lo = 255;
    for (int i = 0; i < 4; i++) { if (pops[i].t < lo) { lo = pops[i].t; slot = i; } }
    snprintf(pops[slot].txt, sizeof(pops[slot].txt), "%s", s);
    pops[slot].x = x; pops[slot].y = y; pops[slot].t = 26; pops[slot].big = big;
}
static void popsTick() { for (int i = 0; i < 4; i++) if (pops[i].t) pops[i].t--; }

// ---- combo -------------------------------------------------------------------
static uint32_t bestBankThisRun = 0;
static void comboAdd(float pts, float multGain, const char* name) {
    if (!comboOpen) { comboOpen = true; pot = 0; mult = 1.0f; grindsInCombo = 0; }
    pot += pts;
    mult += multGain;
    if (mult > 4.0f) mult = 4.0f;
    if (name) popup(name, SK_X + 14, (int)skY - 34);
}
static void comboBank(float factor, const char* label) {
    if (!comboOpen) return;
    uint32_t banked = (uint32_t)(pot * mult * factor);
    score += banked;
    if (banked > bestBankThisRun) bestBankThisRun = banked;
    if (banked > 0) {
        char b[16]; snprintf(b, sizeof(b), "+%u", (unsigned)banked);
        popup(b, SK_X + 10, (int)skY - 46, true);
        if (label) popup(label, SK_X + 10, (int)skY - 60);
        skAward(ACH_FIRSTPOP);
        if (banked >= 500)  skAward(ACH_COMBO500);
        if (banked >= 2000) skAward(ACH_COMBO2000);
        if (grindsInCombo >= 3) skAward(ACH_RAILWIZ);
    }
    comboOpen = false; pot = 0; mult = 1.0f; grindsInCombo = 0;
}
static void comboLose() {
    if (comboOpen) popup("LOST", SK_X + 12, (int)skY - 44);
    comboOpen = false; pot = 0; mult = 1.0f; grindsInCombo = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  PROTOCOL (house convention; see mantiscannon/versus for the pattern notes)
// ─────────────────────────────────────────────────────────────────────────────
enum SkMsgType : uint8_t { SM_HELLO = 1, SM_CONFIRM, SM_START, SM_STATUS, SM_BYE };
struct __attribute__((packed)) SkMsg {
    uint16_t magic;      // 0x4B53
    uint8_t  type;
    uint8_t  seq;
    uint32_t id;
    uint32_t seed;       // START
    uint32_t score;      // STATUS
    uint16_t dist;       // STATUS (meters)
    uint8_t  bails;
    uint8_t  finished;
};
static constexpr uint16_t SK_MAGIC = 0x4B53;
static const uint8_t SK_BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static volatile bool rxHello, rxConfirm, rxStart, rxStatus, rxBye;
static uint8_t  rxMac[6];
static uint32_t rxId, rxSeed, rxScore;
static uint16_t rxDist;
static uint8_t  rxBails, rxFinished;
static uint16_t rxStartSeqSeen = 0x1FF;      // dedup; sentinel outside uint8

static uint32_t skMyId = 0;
static uint8_t  skPeer[6];
static bool     skPeerKnown = false, skLinked = false, skEspReady = false;
static bool     skConfirmedLocal = false, skConfirmedPeer = false;
static uint32_t skLastHello = 0, skLastStatus = 0, skLastPeerSeen = 0, skSearchT0 = 0;
static uint8_t  skSeq = 0;
static bool     skIsHost = false;

static void skOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if ((size_t)len < sizeof(SkMsg)) return;
    const SkMsg* m = (const SkMsg*)data;
    if (m->magic != SK_MAGIC || m->id == skMyId) return;
    memcpy((void*)rxMac, mac, 6);
    rxId = m->id;
    switch (m->type) {
        case SM_HELLO:   rxHello = true;   break;
        case SM_CONFIRM: rxConfirm = true; break;
        case SM_START:
            if ((uint16_t)m->seq != rxStartSeqSeen) {
                rxStartSeqSeen = m->seq;
                rxSeed = m->seed;
                rxStart = true;
            }
            break;
        case SM_STATUS:
            rxScore = m->score; rxDist = m->dist;
            rxBails = m->bails; rxFinished = m->finished;
            rxStatus = true;
            break;
        case SM_BYE:     rxBye = true;     break;
    }
}
static void skClearRx() { rxHello = rxConfirm = rxStart = rxStatus = rxBye = false; }

static uint16_t distMeters();
static bool     vsMode = false;
static bool     vsIFinished = false;
static uint32_t vsRivalScore = 0;
static uint16_t vsRivalDist = 0;
static uint8_t  vsRivalBails = 0;
static bool     vsRivalFinished = false, vsRivalGone = false;
static int      vsFrames = 0;
static int      countdownT = 0;

static void skSend(SkMsgType t, const uint8_t* to) {
    SkMsg m; memset(&m, 0, sizeof(m));
    m.magic = SK_MAGIC; m.type = t; m.id = skMyId; m.seq = ++skSeq;
    if (t == SM_START)  m.seed = runSeed;
    if (t == SM_STATUS) { m.score = score; m.dist = distMeters(); m.bails = (uint8_t)bails; m.finished = vsIFinished ? 1 : 0; }
    esp_now_send(to, (uint8_t*)&m, sizeof(m));
    if (t == SM_START) {                    // double-send, same seq -> dedup'd
        delay(4);
        esp_now_send(to, (uint8_t*)&m, sizeof(m));
    }
}

static bool skAddPeer(const uint8_t* mac) {
    esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.encrypt = false;
    if (esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p) == ESP_OK;
}
static bool skRadioUp() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    skMyId = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(skOnRecv));
    skAddPeer(SK_BCAST);
    return true;
}
static void skRadioDown(bool sayBye) {
    if (skEspReady) {
        if (sayBye && skPeerKnown) skSend(SM_BYE, skPeer);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        skEspReady = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    skPeerKnown = skLinked = false;
    skConfirmedLocal = skConfirmedPeer = false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREENS + INPUT
// ─────────────────────────────────────────────────────────────────────────────
enum SkScreen : uint8_t { SC_MENU = 0, SC_LOBBY, SC_RUN, SC_RESULTS, SC_ACH, SC_LINKLOST };
static SkScreen scr = SC_MENU;
static int      menuSel = 0;
static uint32_t enterAt = 0;
static uint32_t idleT = 0;
static bool     newBestScore = false, newBestDist = false;
static bool     vsIWon = false;
static int      resT = 0;

static bool lTap, rTap, lHoldFire, rHoldFire;
static bool lDown, rDown, lLongDone, rLongDone;
static bool lPress, rPress, lRelease, rRelease;   // raw edges for the run
static uint32_t lDownAt, rDownAt;
static void skInput() {
    lTap = rTap = lHoldFire = rHoldFire = false;
    lPress = rPress = lRelease = rRelease = false;
    uint32_t now = millis();
    if (btnL && !btnL_prev) { lDown = true; lDownAt = now; lLongDone = false; lPress = true; }
    if (btnL && lDown && !lLongDone && now - lDownAt > SK_LONG_MS) { lLongDone = true; lHoldFire = true; }
    if (!btnL && btnL_prev) { if (lDown && !lLongDone) lTap = true; lRelease = true; lDown = false; }
    if (btnR && !btnR_prev) { rDown = true; rDownAt = now; rLongDone = false; rPress = true; }
    if (btnR && rDown && !rLongDone && now - rDownAt > SK_LONG_MS) { rLongDone = true; rHoldFire = true; }
    if (!btnR && btnR_prev) { if (rDown && !rLongDone) rTap = true; rRelease = true; rDown = false; }
    if (now - enterAt < SK_ENTER_GUARD) {
        lTap = rTap = lHoldFire = rHoldFire = false;
        lPress = rPress = lRelease = rRelease = false;
    }
}

static uint16_t distMeters() { return (uint16_t)(camX / 10); }

// ─────────────────────────────────────────────────────────────────────────────
//  RUN LOGIC
// ─────────────────────────────────────────────────────────────────────────────
static void skNewRun(uint32_t seed, bool versus) {
    runSeed = seed; rngS = seed | 1;
    featClear();
    camX = 0; camF = 0; genX = 260;          // first feature lands ~2.5 s in
    spd = SPD_START;
    skY = GY; vy = 0;
    onGround = true; grinding = manualOn = false;
    manualT = 0;
    charge = 0; charging = false;
    anim = AN_NONE; animT = 0;
    grabHeld = false; grabT = 0;
    landPrep = false; pendShove = false;
    pot = 0; mult = 1.0f; comboOpen = false; grindsInCombo = 0;
    score = 0; bails = 0; bailT = 0;
    perfects = 0; bestBankThisRun = 0;
    rollF = 0; frameNo = 0;
    achQueueN = 0;
    newBestScore = newBestDist = false;
    for (int i = 0; i < 24; i++) parts[i].life = 0;
    for (int i = 0; i < 4; i++) pops[i].t = 0;
    shakeT = 0;
    vsMode = versus;
    vsIFinished = false;
    vsRivalScore = 0; vsRivalDist = 0; vsRivalBails = 0;
    vsRivalFinished = false; vsRivalGone = false;
    vsFrames = VS_RUN_FRAMES;
    countdownT = versus ? 186 : 26;          // 3-2-1 in versus, GO beat solo
    skGenUntil(camX + 480);
    scr = SC_RUN;
}

static void skFinishRun() {
    skSv.runs++;
    if (score > skSv.bestScore) { skSv.bestScore = score; newBestScore = true; }
    uint16_t dm = distMeters();
    if (dm > skSv.bestDist) { skSv.bestDist = dm; newBestDist = true; }
    if (dm >= 1500) skAward(ACH_MARATHON);
    if (perfects >= 5) skAward(ACH_PERFECT5);
    if (skSv.runs >= 10) skAward(ACH_LEGEND);
    skDirty = true;

    if (vsMode) {
        vsIFinished = true;
        if (skLinked) { skSend(SM_STATUS, skPeer); skLastStatus = millis(); }
        // results wait for the rival (or the timer) in SC_RUN's versus check
    }
    resT = 0;
    if (!vsMode) { skFlush(); scr = SC_RESULTS; }
}

static void skSettleVersus() {
    // both finished (or the rival vanished): judge it
    if (!vsRivalGone) {
        vsIWon = (score > vsRivalScore) || (score == vsRivalScore && distMeters() >= vsRivalDist);
        if (vsIWon) {
            skSv.vsWins++;
            qpAddXp(30);
            qpGrantPetXp(8);
            skAward(ACH_DUELDECK);
            skDirty = true;
        }
    } else {
        vsIWon = false;                       // no XP from a walkover
    }
    skFlush();
    if (skEspReady) skRadioDown(true);        // results don't need the link
    resT = 0;
    scr = SC_RESULTS;
}

static void skBail(const char* why) {
    comboLose();
    bails++;
    bailT = 46;
    shakeT = 8;
    spd = SPD_MIN;
    charging = false; charge = 0;
    grinding = manualOn = false; manualT = 0;
    anim = AN_NONE; animT = 0;
    grabHeld = false; landPrep = false; pendShove = false;
    boardX = SK_X + 4; boardY = skY - 6; boardVx = 1.8f; boardVy = -2.4f;
    for (int i = 0; i < 6; i++)
        fxSpawn(SK_X + (i % 3) * 3, skY - 8 - i, (i % 2) ? 0.9f : -0.7f, -1.1f, 20, 2);
    popup(why, SK_X + 12, (int)skY - 40, true);
}

static void skRespawn() {
    // stand back up on solid clear ground; walk forward past hazards
    int guard = 0;
    while (guard++ < 400) {
        int32_t wx = camX + SK_X;
        if (skGroundAt(wx) > 0 && skBlockAt(wx) == F_NONE &&
            skBlockAt(wx + 14) == F_NONE && skGroundAt(wx + 14) > 0) break;
        camF += 6; camX = (int32_t)camF;
    }
    skGenUntil(camX + 480);
    skY = skGroundAt(camX + SK_X);
    vy = 0; onGround = true;
}

// The heart of the game: what happens the moment the wheels touch down.
static void skLand(int gy) {
    skY = gy; vy = 0; onGround = true;
    fxDust(SK_X, skY, 4);
    if (animT > 0 || anim != AN_NONE) { skBail("SKETCHY!"); return; }
    if (grabHeld)                     { skBail("HUNG UP!"); return; }
    if (landPrep) {
        perfects++;
        spd += SPD_PERFECT;
        if (spd > SPD_MAX) spd = SPD_MAX;
        comboBank(1.5f, "PERFECT!");
        charging = btnR;                      // the prep press rolls into the next pop
        landPrep = false;
        fxDust(SK_X, skY, 4);
        return;
    }
    if (btnL) {                               // land into a manual: pot stays open
        manualOn = true; manualT = 0;
        comboAdd(20, 0.05f, "MANUAL");
        return;
    }
    // no input: sloppy
    spd -= SPD_SLOPPY;
    if (spd < SPD_MIN) spd = SPD_MIN;
    comboBank(0.5f, "SLOPPY");
}

static void skRunTick() {
    frameNo++;
    int32_t prevWx = camX + SK_X;

    // countdown gate (versus starts in sync; solo gets a GO beat)
    if (countdownT > 0) { countdownT--; return; }

    // versus clock
    if (vsMode && !vsIFinished) {
        if (--vsFrames <= 0) { skFinishRun(); }
    }
    // once finished in versus the run is frozen: beats + settle checks only
    if (vsMode && vsIFinished) {
        if (skLinked) {
            uint32_t now = millis();
            if (now - skLastStatus > SK_STATUS_MS) { skSend(SM_STATUS, skPeer); skLastStatus = now; }
            if (rxStatus) {
                rxStatus = false;
                skLastPeerSeen = now;
                vsRivalScore = rxScore; vsRivalDist = rxDist;
                vsRivalBails = rxBails; vsRivalFinished = rxFinished != 0;
            }
            if (rxBye) { rxBye = false; skRadioDown(false); vsRivalGone = true; }
            if (skLinked && now - skLastPeerSeen > SK_PEER_LOST) { skRadioDown(false); vsRivalGone = true; }
        }
        if (vsRivalFinished || vsRivalGone || !skLinked) skSettleVersus();
        return;
    }

    // ---- bail tumble --------------------------------------------------------
    if (bailT > 0) {
        bailT--;
        camF += spd * 0.3f;
        camX = (int32_t)camF;
        boardX += boardVx; boardVy += 0.18f; boardY += boardVy;
        if (bailT == 0) {
            if (bails >= MAX_BAILS) { skFinishRun(); return; }
            skRespawn();
        }
        return;
    }

    // ---- scroll (float master so 2.3 speed IS 2.3, not a quantized 2.0) -----
    camF += spd;
    camX = (int32_t)camF;
    int32_t wx = camX + SK_X;
    skGenUntil(camX + 480);

    // ---- kicker launch ------------------------------------------------------
    if (onGround && skKickerCross(prevWx, wx)) {
        onGround = false; charging = false; charge = 0;
        spd += 0.4f;                          // the ramp gives, not takes
        if (spd > SPD_MAX) spd = SPD_MAX;
        vy = -(3.6f + spd * 0.5f);
        comboAdd(25, 0.08f, "AIR!");
        fxDust(SK_X, skY, 5);
    }

    // ---- ground / air / rail physics ---------------------------------------
    if (grinding) {
        int rail = skRailAt(wx);
        if (rail < 0) {                       // rail ended: drop off, pot open
            grinding = false; onGround = false; vy = 0.4f;
        } else {
            skY = rail;
            if ((frameNo & 3) == 0) { pot += spd * 0.6f; }
            fxSpawn(SK_X - 6, skY + 1, -1.4f, -0.4f - (frameNo % 3) * 0.2f, 8, 1);
            // charging an ollie out is allowed
            if (rPress) { charging = true; charge = 0; }
            if (charging && btnR && charge < 26) charge++;
            if (charging && rRelease) {
                charging = false;
                grinding = false; onGround = false;
                vy = -(2.6f + charge * 0.09f);
                charge = 0;
                fxDust(SK_X, skY, 3);
            }
        }
    } else if (onGround) {
        int gy = skGroundAt(wx);
        if (gy < 0) {                          // rode off an edge
            onGround = false; vy = 0.2f;
            if (manualOn) { manualOn = false; }   // pot stays open in the air
        } else {
            if (gy < (int)skY - 8) {
                skBail("BOX!");                // slammed a box face: ollie it
                return;
            } else if (gy < (int)skY - 1) {
                skY = gy;                      // curb-height: roll up it
                fxDust(SK_X, skY, 2);
            } else if (gy > (int)skY + 1) {
                onGround = false; vy = 0.2f;   // rolled off a box end
            } else skY = gy;

            uint8_t blk = skBlockAt(wx + 6);
            if (onGround && blk != F_NONE) {
                skBail(blk == F_RAIL ? "POST!" : "WIPEOUT!");
                return;
            }
            // crouch / pop
            if (rPress) { charging = true; charge = 0; }
            if (charging && btnR && charge < 26) charge++;
            if (charging && rRelease) {
                charging = false;
                onGround = false;
                vy = -(2.6f + charge * 0.09f);
                charge = 0;
                if (manualOn) { manualOn = false; }   // ollie out of a manual
                fxDust(SK_X, skY, 4);
            }
            // manual handling
            if (manualOn) {
                manualT++;
                if ((frameNo & 3) == 0) pot += 3;
                if (!btnL) {                   // released clean: bank
                    manualOn = false; manualT = 0;
                    comboBank(1.0f, "CLEAN");
                } else if (manualT > MANUAL_MAX) {
                    manualOn = false; manualT = 0;
                    comboBank(0.75f, "WOBBLE");
                }
            } else if (lPress && !charging) {
                // pick the nose anywhere: opens a pot or keeps one alive
                manualOn = true; manualT = 0;
                comboAdd(15, 0.05f, "MANUAL");
            }
            if (onGround && spd > 3.2f && (frameNo & 7) == 0) fxDust(SK_X - 8, skY, 1);
            rollF = (frameNo / (spd > 3.0f ? 5 : 8)) & 1;
        }
    } else {
        // ---- airborne -------------------------------------------------------
        vy += SK_GRAV;
        skY += vy;

        // trick inputs: an L TAP is a kickflip; a held L is grind-intent
        // (and, carried through touchdown, a manual)
        if (lTap && anim == AN_NONE) {         // kickflip on release (tap)
            anim = AN_FLIP; animT = 14;
            comboAdd(80, 0.15f, "KICKFLIP");
        }
        if (rPress) {
            // classify: landing prep if touchdown is near, else shove/grab seed
            float toGround = 9999;
            int gy = skGroundAt(wx);
            if (gy > 0 && vy > -0.5f) {
                // frames until skY reaches gy: a t^2 + b t + (skY-gy) = 0
                float a = 0.5f * SK_GRAV, b = vy, c = skY - (float)gy;
                float disc = b * b - 4.0f * a * c;      // c<0 -> always >= b^2
                if (disc >= 0) toGround = (-b + sqrtf(disc)) / (2.0f * a);
            }
            if (toGround <= 10.0f) { landPrep = true; charging = true; charge = 0; }
            else { pendShove = true; pendShoveAt = millis(); }
        }
        if (charging && btnR && charge < 26) charge++;   // buffered next pop
        if (pendShove && rRelease) {
            pendShove = false;
            if (anim == AN_NONE) { anim = AN_SHOVE; animT = 12; comboAdd(60, 0.12f, "SHOVE-IT"); }
        }
        if (pendShove && btnR && millis() - pendShoveAt > SK_LONG_MS) {
            pendShove = false;
            grabHeld = true; grabT = 0;
        }
        if (grabHeld) {
            grabT++;
            if ((frameNo & 3) == 0) pot += 6;
            if (!comboOpen) { comboOpen = true; mult = 1.0f; }
            if (rRelease) {
                grabHeld = false;
                char b[16]; snprintf(b, sizeof(b), "MELON +%d", grabT / 2);
                comboAdd((float)(grabT / 2), 0.10f, b);
            }
        }
        if (animT > 0 && --animT == 0) anim = AN_NONE;

        // grind lock: descending + holding L + meeting a rail top
        if (vy > 0 && btnL && !grabHeld) {
            int rail = skRailAt(wx);
            if (rail > 0 && skY >= rail - 5 && skY <= rail + 6) {
                if (anim != AN_NONE) { skBail("SKETCHY!"); return; }
                grinding = true;
                skY = rail; vy = 0;
                landPrep = false; pendShove = false;
                comboAdd(40, 0.15f, "50-50");
                grindsInCombo++;
                shakeT = 2;
            }
        }
        // touchdown
        if (!grinding && vy > 0) {
            int gy = skGroundAt(wx);
            if (gy > 0 && skY >= gy) { skLand(gy); }
            else if (skY > H + 8)    {           // into the pit
                skY = GY;                        // tumble at the pit edge
                skBail("GAP!");
                return;
            }
        }
    }

    // ---- versus beats -------------------------------------------------------
    if (vsMode && skLinked) {
        uint32_t now = millis();
        if (now - skLastStatus > SK_STATUS_MS) { skSend(SM_STATUS, skPeer); skLastStatus = now; }
        if (rxStatus) {
            rxStatus = false;
            skLastPeerSeen = now;
            bool was = vsRivalBails < MAX_BAILS;
            vsRivalScore = rxScore; vsRivalDist = rxDist;
            if (rxBails > vsRivalBails && was) popup("RIVAL BAILED!", W / 2, 70);
            vsRivalBails = rxBails;
            vsRivalFinished = rxFinished != 0;
        }
        if (rxBye) { rxBye = false; skRadioDown(false); vsRivalGone = true; }
        if (skLinked && now - skLastPeerSeen > SK_PEER_LOST) { skRadioDown(false); vsRivalGone = true; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DRAW
// ─────────────────────────────────────────────────────────────────────────────
static void skBlit(const uint8_t* sp, int sw, int sh, int x, int y, int scale = 1) {
    int rowB = (sw + 1) / 2;
    for (int py = 0; py < sh; py++) {
        for (int px = 0; px < sw; px++) {
            uint8_t b = sp[py * rowB + px / 2];
            uint8_t v = (px & 1) ? (b >> 4) : (b & 0x0F);
            if (!v || v > SK_PAL_N) continue;
            if (scale == 1) fb.drawPixel(x + px, y + py, SK_PAL[v - 1]);
            else fb.fillRect(x + px * scale, y + py * scale, scale, scale, SK_PAL[v - 1]);
        }
    }
}

static void drawParallax(int oy) {
    // far skyline (quarter speed) with a few lit windows
    int off = (int)((camX / 4) % 90);
    for (int bx = -off; bx < W + 40; bx += 45) {
        int hgt = 34 + ((bx + off) / 45 % 3) * 14;
        fb.fillRect(bx, 148 - hgt + oy, 26, hgt, TEAL_DARK);
        if (((bx + off) / 45) % 2 == 0) {
            fb.drawPixel(bx + 6, 148 - hgt + 8 + oy, LIME_DARK);
            fb.drawPixel(bx + 14, 148 - hgt + 16 + oy, LIME_DARK);
        }
    }
    // mid fence line (half speed)
    int f = (int)((camX / 2) % 22);
    for (int bx = -f; bx < W; bx += 22)
        fb.fillRect(bx, 200 + oy, 2, 18, PURPLE_DEEP);
    fb.fillRect(0, 198 + oy, W, 2, PURPLE_DEEP);
    // moon
    fb.fillCircle(W - 24, 40, 7, LIME_PALE);
    fb.fillCircle(W - 27, 38, 6, TEAL_DEEP);
}

static void drawCourse(int oy) {
    // ground body + top edge + scrolling cracks (the speed read)
    for (int sx = 0; sx < W; sx++) {
        int32_t wx = camX + sx;
        int gy = skGroundAt(wx);
        if (gy < 0) continue;
        fb.fillRect(sx, gy + oy, 1, H - 14 - gy, TEAL_DARK);
        fb.drawPixel(sx, gy + oy, TEAL);
        fb.drawPixel(sx, gy + 1 + oy, TEAL);
    }
    int crk = (int)(camX % 48);
    for (int sx = -crk; sx < W; sx += 48) {
        int32_t wx = camX + sx;
        int gy = skGroundAt(wx);
        if (gy > 0) fb.fillRect(sx, gy + 3 + oy, 1, 4, TEAL_DEEP);
    }
    // features
    for (int i = 0; i < MAX_FEATS; i++) {
        const Feat& f = feats[i];
        if (f.type == F_NONE) continue;
        int sx = (int)(f.x - camX);
        if (sx + f.w < -20 || sx > W + 20) continue;
        switch (f.type) {
        case F_CONE:    skBlit(SK_CONE, SK_CONE_W, SK_CONE_H, sx - 1, GY - SK_CONE_H + 1 + oy); break;
        case F_HYDRANT: skBlit(SK_HYDRANT, SK_HYDRANT_W, SK_HYDRANT_H, sx - 1, GY - SK_HYDRANT_H + 1 + oy); break;
        case F_RAIL: {
            int ry = GY - RAIL_H + oy;
            fb.fillRect(sx, ry, f.w, 2, TEAL_LIGHT);
            fb.drawPixel(sx, ry - 1, TEAL_BRIGHT);
            fb.fillRect(sx + 1, ry + 2, 2, RAIL_H - 2, TEAL);
            fb.fillRect(sx + f.w - 3, ry + 2, 2, RAIL_H - 2, TEAL);
            break; }
        case F_GAP:
        case F_STAIRS: {
            // pit walls + depth shading; stair steps on the entry edge
            fb.fillRect(sx, GY + oy, 1, H - 14 - GY, TEAL);
            fb.fillRect(sx + f.w - 1, GY + oy, 1, H - 14 - GY, TEAL);
            for (int yy = GY + 4; yy < H - 14; yy += 6)
                fb.fillRect(sx + 2, yy + oy, f.w - 4, 1, TEAL_DEEP);
            if (f.type == F_STAIRS)
                for (int s = 0; s < 3; s++)
                    fb.fillRect(sx + s * 5, GY + s * 5 + oy, 5, 2, TEAL);
            break; }
        case F_BOX: {
            int by = GY - BOX_H + oy;
            fb.fillRoundRect(sx, by, f.w, BOX_H, 2, PURPLE_DARK);
            fb.fillRect(sx, by, f.w, 2, PURPLE);
            fb.drawPixel(sx + 4, by + 6, PURPLE);
            fb.drawPixel(sx + f.w - 6, by + 9, PURPLE);
            break; }
        case F_KICKER: {
            fb.fillTriangle(sx, GY + oy, sx + f.w, GY + oy, sx + f.w, GY - 14 + oy, PURPLE_DARK);
            fb.drawLine(sx, GY + oy, sx + f.w, GY - 14 + oy, LIME);
            break; }
        }
    }
}

static void drawSkater(int oy) {
    int32_t wx = camX + SK_X;

    // landing shadow: THE timing affordance
    int gy = skGroundAt(wx);
    if (gy > 0 && bailT == 0) {
        int hgt = gy - (int)skY;
        if (hgt < 0) hgt = 0;
        int sw2 = 9 - hgt / 14;
        if (sw2 < 3) sw2 = 3;
        uint16_t sc = (!onGround && vy > 0 && hgt < 26) ? LIME_DARK : TEAL;
        fb.fillRect(SK_X - sw2 + 4, gy + 1 + oy, sw2 * 2, 1, sc);
    }

    if (bailT > 0) {
        skBlit((bailT / 5) & 1 ? SK_BAIL2 : SK_BAIL1, SK_BAIL1_W, SK_BAIL1_H,
               SK_X - 10, (int)skY - 20 + oy);
        skBlit(SK_BOARD, SK_BOARD_W, SK_BOARD_H, (int)boardX, (int)boardY + oy);
        return;
    }

    const uint8_t* sp = SK_ROLL1;
    if (grinding)                sp = SK_GRIND;
    else if (manualOn)           sp = SK_MANUAL;
    else if (!onGround) {
        if (anim == AN_FLIP)     sp = (animT > 7) ? SK_FLIP1 : SK_FLIP2;
        else if (anim == AN_SHOVE) sp = (animT > 6) ? SK_FLIP2 : SK_AIR;
        else if (grabHeld)       sp = SK_GRAB;
        else                     sp = SK_AIR;
    }
    else if (charging)           sp = SK_CROUCH;
    else                         sp = rollF ? SK_ROLL2 : SK_ROLL1;
    skBlit(sp, 24, 24, SK_X - 12 + 4, (int)skY - 23 + oy);

    // charge tell: a lime coil under the tail
    if (charging && charge > 4) {
        int cw = charge / 3;
        fb.fillRect(SK_X - cw / 2 + 4, (int)skY + 3 + oy, cw, 2, charge >= 24 ? LIME : LIME_DARK);
    }
    // speed lines at high flow
    if (spd > 3.1f && onGround && !manualOn) {
        for (int i = 0; i < 3; i++)
            fb.fillRect(SK_X - 20 - i * 7, (int)skY - 8 - i * 5 + oy, 5, 1, TEAL_LIGHT);
    }
}

static void drawFx(int oy) {
    for (int i = 0; i < 24; i++) {
        if (!parts[i].life) continue;
        uint16_t c = parts[i].kind == 1 ? (parts[i].life & 1 ? LIME : PURPLE_LIGHT)
                   : parts[i].kind == 2 ? PURPLE_LIGHT : TEAL_LIGHT;
        fb.drawPixel((int)parts[i].x, (int)parts[i].y + oy, c);
    }
}

static void drawPops() {
    for (int i = 0; i < 4; i++) {
        if (!pops[i].t) continue;
        int rise = (26 - pops[i].t) / 2;
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(pops[i].big ? LIME : LIME_PALE);
        fb.drawString(pops[i].txt, pops[i].x, pops[i].y - rise, pops[i].big ? 2 : 1);
    }
}

static void drawRunHud() {
    fb.fillRect(0, 0, W, 30, TEAL_DEEP);
    char b[24];
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "%u", (unsigned)score);
    fb.drawString(b, 3, 2, 2);
    fb.setTextColor(TEAL_DARK);
    snprintf(b, sizeof(b), "BEST %u", (unsigned)skSv.bestScore);
    fb.drawString(b, 3, 20, 1);
    // flow meter
    int fw = (int)((spd - SPD_MIN) / (SPD_MAX - SPD_MIN) * 44);
    fb.drawRect(62, 4, 46, 5, TEAL);
    if (fw > 0) fb.fillRect(63, 5, fw, 3, spd > 3.3f ? LIME : TEAL_BRIGHT);
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("FLOW", 62, 11, 1);
    // distance + bails
    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(TEAL_LIGHT);
    snprintf(b, sizeof(b), "%um", (unsigned)distMeters());
    fb.drawString(b, W - 3, 2, 1);
    for (int i = 0; i < MAX_BAILS; i++) {
        uint16_t c = i < MAX_BAILS - bails ? LIME : TEAL_DARK;
        fb.fillRect(W - 12 - i * 9, 14, 7, 2, c);
        fb.drawPixel(W - 11 - i * 9, 17, c);
        fb.drawPixel(W - 7 - i * 9, 17, c);
    }
    // versus strip
    if (vsMode) {
        fb.fillRect(0, 30, W, 12, PURPLE_DEEP);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b, sizeof(b), "RIVAL %u", (unsigned)vsRivalScore);
        fb.drawString(b, 3, 36, 1);
        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(vsFrames < 62 * 15 ? LIME : TEAL_LIGHT);
        int secs = vsFrames / 62;
        snprintf(b, sizeof(b), vsRivalGone ? "RIVAL LOST" : "%d:%02d", secs / 60, secs % 60);
        if (vsRivalGone) fb.drawString("RIVAL LOST", W - 3, 36, 1);
        else fb.drawString(b, W - 3, 36, 1);
    }
    // combo readout
    if (comboOpen) {
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor((grabHeld || grinding || manualOn) && (frameNo & 8) ? LIME : LIME_PALE);
        snprintf(b, sizeof(b), "%d x%d.%d", (int)pot, (int)mult, (int)(mult * 10) % 10);
        fb.drawString(b, W / 2, vsMode ? 46 : 34, 2);
        if (manualOn) {
            int mw = ((MANUAL_MAX - manualT) * 40) / MANUAL_MAX;
            fb.drawRect(W / 2 - 21, vsMode ? 64 : 52, 42, 4, TEAL);
            fb.fillRect(W / 2 - 20, vsMode ? 65 : 53, mw, 2, manualT > MANUAL_MAX - 25 ? PURPLE_LIGHT : LIME);
        }
    }
}

static void skFooter(const char* l, const char* r) {
    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM);
    fb.drawString(l, 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM);
    fb.drawString(r, W - 4, H - 7, 1);
}

static void drawRun() {
    int oy = 0;
    if (shakeT) oy = (int)((frameNo * 2654435761u >> 9) & 3) - 1;
    fb.fillSprite(TEAL_DEEP);
    drawParallax(oy);
    drawCourse(oy);
    drawFx(oy);
    drawSkater(oy);
    drawPops();
    drawRunHud();
    if (countdownT > 0) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(LIME);
        if (vsMode) {
            char b[4]; snprintf(b, sizeof(b), "%d", countdownT / 62 + 1);
            fb.drawString(b, W / 2, 140, 6);
        } else if (countdownT > 20) {
            fb.drawString("GO!", W / 2, 140, 4);
        }
    }
    if (vsMode && vsIFinished) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("WAITING FOR RIVAL...", W / 2, 150, 1);
    }
    skFooter("L: trick/grind/manual", "R: pop/land");
}

static void drawMenu() {
    fb.fillSprite(TEAL_DEEP);
    drawParallax(0);
    fb.fillRect(0, GY, W, H - 14 - GY, TEAL_DARK);
    fb.fillRect(0, GY, W, 2, TEAL);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS", W / 2, 26, 4);
    fb.drawString("SKATE", W / 2, 48, 4);
    // hero: 2x air pose with speed lines
    skBlit(SK_AIR, 24, 24, W / 2 - 24, 74, 2);
    for (int i = 0; i < 3; i++)
        fb.fillRect(W / 2 - 44 - i * 8, 92 + i * 8, 10, 1, TEAL_LIGHT);

    static const char* ITEMS[3] = { "SKATE THE LINE", "VERSUS RUN", "DECK MEDALS" };
    char sub[3][26];
    snprintf(sub[0], 26, "best %u  -  %um", (unsigned)skSv.bestScore, (unsigned)skSv.bestDist);
    snprintf(sub[1], 26, "90s duel over ESP-NOW");
    {
        int got = 0;
        for (int i = 0; i < 8; i++) if ((skSv.ach >> i) & 1) got++;
        snprintf(sub[2], 26, "%d/8  -  %u vs wins", got, (unsigned)skSv.vsWins);
    }
    for (int i = 0; i < 3; i++) {
        int y = 138 + i * 52;
        bool sel = (i == menuSel);
        fb.fillRoundRect(10, y, W - 20, 44, 5, sel ? TEAL_DARK : TEAL_DEEP);
        fb.drawRoundRect(10, y, W - 20, 44, 5, sel ? LIME : TEAL_DARK);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(sel ? LIME_PALE : TEAL_BRIGHT);
        fb.drawString(ITEMS[i], W / 2, y + 15, 2);
        fb.setTextColor(sel ? TEAL_LIGHT : TEAL_DARK);
        fb.drawString(sub[i], W / 2, y + 32, 1);
    }
    skFooter("L: next", "R: select");
}

static void drawLobby() {
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("FINDING A RIVAL", W / 2, 60, 2);
    float a = (millis() % 2000) * 0.00314f;
    fb.drawCircle(W / 2, 150, 38, TEAL_DARK);
    fb.drawCircle(W / 2, 150, 24, TEAL_DARK);
    fb.drawLine(W / 2, 150, W / 2 + (int)(cosf(a) * 38), 150 + (int)(sinf(a) * 38), LIME);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(skPeerKnown ? "RIVAL FOUND - LINKING" : "hold devices close", W / 2, 212, 1);
    char t[16];
    uint32_t left = (SK_SEARCH_MS - (millis() - skSearchT0)) / 1000;
    snprintf(t, sizeof(t), "%us", (unsigned)left);
    fb.drawString(t, W / 2, 230, 1);
    skFooter("L: cancel", "");
}

static void drawResults() {
    fb.fillSprite(TEAL_DEEP);
    drawParallax(0);
    fb.fillRoundRect(10, 56, W - 20, 212, 8, TEAL_DEEP);
    fb.drawRoundRect(10, 56, W - 20, 212, 8, LIME_DARK);
    fb.setTextDatum(MC_DATUM);

    if (vsMode) {
        fb.setTextColor(vsIWon ? LIME : PURPLE_LIGHT);
        fb.drawString(vsIWon ? "YOU WIN!" : (vsRivalGone ? "RIVAL LOST" : "RIVAL WINS"), W / 2, 74, 4);
        char b[26];
        fb.setTextColor(LIME_PALE);
        snprintf(b, sizeof(b), "YOU   %u", (unsigned)score);
        fb.drawString(b, W / 2, 100, 2);
        fb.setTextColor(TEAL_BRIGHT);
        snprintf(b, sizeof(b), "RIVAL %u", (unsigned)vsRivalScore);
        fb.drawString(b, W / 2, 118, 2);
        if (vsIWon) { fb.setTextColor(TEAL_LIGHT); fb.drawString("+30 XP", W / 2, 136, 1); }
    } else {
        // rank stamp
        const char* rank = score >= 12000 ? "S" : score >= 7000 ? "A" :
                           score >= 3500 ? "B" : score >= 1500 ? "C" : "D";
        fb.setTextColor(LIME);
        fb.drawString(rank, W / 2, 86, 7);
        fb.setTextColor(LIME_PALE);
        char b[26];
        snprintf(b, sizeof(b), "SCORE %u", (unsigned)score);
        fb.drawString(b, W / 2, 118, 2);
        if (newBestScore) { fb.setTextColor(LIME); fb.drawString("NEW BEST!", W / 2, 134, 1); }
    }
    char b[28];
    fb.setTextColor(TEAL_LIGHT);
    snprintf(b, sizeof(b), "%um  -  best combo %u", (unsigned)distMeters(), (unsigned)bestBankThisRun);
    fb.drawString(b, W / 2, vsMode ? 152 : 150, 1);
    snprintf(b, sizeof(b), "%d perfect  -  %d bails", perfects, bails);
    fb.drawString(b, W / 2, vsMode ? 164 : 162, 1);

    int y = 182;
    for (int i = 0; i < achQueueN && y < 250; i++, y += 14) {
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b, sizeof(b), "MEDAL: %s +%dXP", ACHS[achQueue[i]].name, ACHS[achQueue[i]].xp);
        fb.drawString(b, W / 2, y, 1);
    }
    skFooter("L: menu", "R: skate again");
}

static void drawAch() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("DECK MEDALS", 4, 9, 1);
    char b[16];
    int got = 0;
    for (int i = 0; i < 8; i++) if ((skSv.ach >> i) & 1) got++;
    fb.setTextDatum(MR_DATUM);
    snprintf(b, sizeof(b), "%d/8", got);
    fb.drawString(b, W - 4, 9, 1);
    for (int i = 0; i < 8; i++) {
        int y = 24 + i * 35;
        bool have = (skSv.ach >> i) & 1;
        fb.fillRoundRect(4, y, W - 8, 32, 4, TEAL_DEEP);
        fb.drawRoundRect(4, y, W - 8, 32, 4, have ? LIME_DARK : TEAL_DARK);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(have ? LIME : TEAL_LIGHT);
        fb.drawString(ACHS[i].name, 10, y + 9, 1);
        fb.setTextColor(have ? LIME_PALE : TEAL_DARK);
        fb.drawString(ACHS[i].how, 10, y + 22, 1);
        if (have) {
            fb.setTextDatum(MR_DATUM);
            fb.setTextColor(LIME);
            snprintf(b, sizeof(b), "+%d", ACHS[i].xp);
            fb.drawString(b, W - 10, y + 15, 1);
        }
    }
    skFooter("", "L/R: back");
}

void mantisskateDraw() {
    switch (scr) {
    case SC_MENU:    drawMenu();    break;
    case SC_LOBBY:   drawLobby();   break;
    case SC_RUN:     drawRun();     break;
    case SC_RESULTS: drawResults(); break;
    case SC_ACH:     drawAch();     break;
    case SC_LINKLOST:
        fb.fillSprite(TEAL_DEEP);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("LINK LOST", W / 2, 130, 4);
        skFooter("", "L/R: menu");
        break;
    }
    fb.pushSprite(0, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOBBY + TOP-LEVEL TICK + LIFECYCLE
// ─────────────────────────────────────────────────────────────────────────────
static void skTickLobby() {
    uint32_t now = millis();
    if (now - skLastHello > SK_HELLO_MS) { skSend(SM_HELLO, SK_BCAST); skLastHello = now; }
    if (rxHello && !skPeerKnown) {
        rxHello = false;
        memcpy(skPeer, (const void*)rxMac, 6);
        skAddPeer(skPeer);
        skPeerKnown = true;
        skIsHost = (skMyId < rxId);
        skSend(SM_CONFIRM, skPeer);
        skConfirmedLocal = true;
    }
    if (rxConfirm && skPeerKnown) {
        rxConfirm = false;
        skConfirmedPeer = true;
        if (!skConfirmedLocal) { skSend(SM_CONFIRM, skPeer); skConfirmedLocal = true; }
    }
    if (skConfirmedLocal && skConfirmedPeer && !skLinked) {
        skLinked = true;
        skLastPeerSeen = now;
        skLastStatus = now;
        if (skIsHost) {
            runSeed = (uint32_t)esp_random() | 1;
            skSend(SM_START, skPeer);          // double-send inside skSend
            skClearRx();
            skNewRun(runSeed, true);
        }
    }
    if (skLinked && !skIsHost && rxStart) {
        rxStart = false;
        skClearRx();
        skNewRun(rxSeed, true);
    }
    if (!skLinked && now - skSearchT0 > SK_SEARCH_MS) {
        skRadioDown(false);
        scr = SC_MENU;
    }
    if (lTap || lHoldFire) { skRadioDown(true); scr = SC_MENU; }
}

void mantisskateTick() {
    skInput();
    fxTick();
    popsTick();
    idleT++;

    switch (scr) {

    case SC_MENU:
        if (lTap) menuSel = (menuSel + 1) % 3;
        if (rTap) {
            if (menuSel == 0) skNewRun((uint32_t)esp_random() | 1, false);
            else if (menuSel == 1) {
                skClearRx();
                rxStartSeqSeen = 0x1FF;
                skEspReady = skRadioUp();
                if (skEspReady) { skSearchT0 = millis(); skLastHello = 0; scr = SC_LOBBY; }
            } else scr = SC_ACH;
        }
        break;

    case SC_LOBBY:
        skTickLobby();
        break;

    case SC_RUN:
        skRunTick();
        break;

    case SC_RESULTS:
        resT++;
        if (resT > 30 && rTap) {
            if (vsMode) {
                // versus rematch keeps it simple: back through the lobby
                if (skEspReady) skRadioDown(true);
                skClearRx();
                rxStartSeqSeen = 0x1FF;
                skEspReady = skRadioUp();
                if (skEspReady) { skSearchT0 = millis(); skLastHello = 0; scr = SC_LOBBY; }
                else scr = SC_MENU;
            } else {
                skNewRun((uint32_t)esp_random() | 1, false);
            }
        }
        if (resT > 30 && (lTap || lHoldFire)) {
            if (skEspReady) skRadioDown(true);
            scr = SC_MENU;
        }
        break;

    case SC_ACH:
        if (lTap || rTap) scr = SC_MENU;
        break;

    case SC_LINKLOST:
        if (lTap || rTap) scr = SC_MENU;
        break;
    }
}

// ---- house rule: an app holds its working buffers ONLY while it is open ----
// Taken at the very start of Enter, freed at the very end of Shutdown, so a
// closed app costs no RAM and every other app (TLS especially) gets it back.
struct AppHeap_skate {
    SkPart parts[24];
    Feat feats[MAX_FEATS];
    SkPop pops[4];
};
static AppHeap_skate* g_heap_skate = nullptr;
static void heapUp_skate(){
    if (!g_heap_skate) g_heap_skate = (AppHeap_skate*)malloc(sizeof(AppHeap_skate));
    if (!g_heap_skate) return;
    memset(g_heap_skate, 0, sizeof(AppHeap_skate));
    parts = g_heap_skate->parts;
    feats = g_heap_skate->feats;
    pops = g_heap_skate->pops;
}
static void heapDown_skate(){
    free(g_heap_skate); g_heap_skate = nullptr;
    parts = nullptr;
    feats = nullptr;
    pops = nullptr;
}

void mantisskateEnter() {
    heapUp_skate();                       // first: everything below may use the buffers
    fb.setRotation(0);
    skLoad();
    scr = SC_MENU;
    menuSel = 0;
    enterAt = millis();
    idleT = 0;
    lDown = rDown = false;
    lLongDone = rLongDone = false;
    skEspReady = false;
    skPeerKnown = skLinked = false;
    skConfirmedLocal = skConfirmedPeer = false;
    skClearRx();
    rxStartSeqSeen = 0x1FF;
    skSeq = 0;
    vsMode = false;
    featClear();
    for (int i = 0; i < 24; i++) parts[i].life = 0;
    for (int i = 0; i < 4; i++) pops[i].t = 0;
    camX = 0; camF = 0; spd = SPD_START;
}

void mantisskateShutdown() {
    skRadioDown(true);
    skFlush();
    heapDown_skate();                     // last: after saves and radio teardown
}
