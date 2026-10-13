// FILE: src/roadhunt/roadhunt_mode.cpp
// ROAD HUNT - a road-trip scavenger hunt for the long drive.
//
// Solo or "Road Party" (ESP-NOW). Every player gets their OWN randomized card
// of 12 things to spot out the window. Check them off as you see them. First
// to clear the card wins; otherwise the host ends the trip and we tally.
//
// NETWORKING follows the proven Versus/Pet pattern, not a host-authoritative
// snapshot stream:
//   * recv callback is FAST - it only memcpy's into a buffer and sets a
//     volatile flag. All logic runs in tick().
//   * Each device OWNS its own score and card. Peers adopt a broadcast value
//     only as THAT peer's value; we never overwrite our own truth.
//   * HELLO beacons during the lobby only. Once rolling, one small STATUS
//     every ~2s, plus an IMMEDIATE FIND event the moment somebody claims an
//     item so the taunt/celebration screens feel live.
//   * FIND carries a per-sender sequence byte so ESP-NOW's duplicate frames
//     don't double-count (same trick as Versus' vsLastBallId).
//   * Radio is NEVER initialized in solo play. Full teardown on exit.
//
// FLASH: one packed blob in NVS "roadhunt", written behind a dirty flag with a
// 10s minimum interval, plus a forced write on trip end / shutdown. A 4 hour
// trip costs a handful of writes.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "../power_guard.h"
#include "roadhunt_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ─────────────────────────────────────────────────────────────────────────────
//  TUNABLES
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int      CARD_SIZE       = 12;
static constexpr int      RH_MAX_PLAYERS  = 6;
static constexpr int      RH_NAME_LEN     = 10;
static constexpr uint32_t RH_LONG_MS      = 420;    // long-press threshold
static constexpr uint32_t RH_HELLO_MS     = 300;    // lobby beacon (Versus rate)
static constexpr uint32_t RH_STATUS_MS    = 2000;   // in-trip heartbeat
static constexpr uint32_t RH_PEER_TIMEOUT = 12000;  // drop a silent peer
static constexpr uint32_t RH_ENTER_GUARD  = 250;    // swallow the entering press
static constexpr int      SKIP_COST       = 5;

// ─────────────────────────────────────────────────────────────────────────────
//  ITEM POOL
// ─────────────────────────────────────────────────────────────────────────────
enum RoadCat : uint8_t {
    RCAT_ROAD = 0, RCAT_NATURE, RCAT_FOOD, RCAT_VEHICLE,
    RCAT_ANIMAL, RCAT_LANDMARK, RCAT_WEIRD, RCAT_MANTIS, RCAT_COUNT
};

struct ScavItem {
    const char* name;    // keep <= 20 chars for font 1
    uint8_t     cat;
    uint8_t     rarity;  // 1 common .. 4 legendary
    uint8_t     points;
};

static const ScavItem ITEMS[] = {
    // ── ROAD ────────────────────────────────────────────────────────────────
    { "STOP SIGN",      RCAT_ROAD,     1, 10 },
    { "BRIDGE",         RCAT_ROAD,     1, 10 },
    { "ROAD WORK",      RCAT_ROAD,     1, 10 },
    { "BILLBOARD",      RCAT_ROAD,     1, 10 },
    { "REST STOP",      RCAT_ROAD,     1, 10 },
    { "MILE MARKER 0",  RCAT_ROAD,     3, 35 },
    { "TOLL BOOTH",     RCAT_ROAD,     2, 18 },
    { "TUNNEL",         RCAT_ROAD,     2, 18 },
    { "RUNAWAY RAMP",   RCAT_ROAD,     3, 35 },
    { "COVERED BRIDGE", RCAT_ROAD,     3, 35 },
    // ── NATURE ──────────────────────────────────────────────────────────────
    { "COW",            RCAT_NATURE,   1, 10 },
    { "RIVER",          RCAT_NATURE,   1, 10 },
    { "WATER TOWER",    RCAT_NATURE,   1, 10 },
    { "HAY BALES",      RCAT_NATURE,   1, 10 },
    { "WINDMILL",       RCAT_NATURE,   2, 18 },
    { "HORSE",          RCAT_NATURE,   2, 18 },
    { "SNOWY PEAK",     RCAT_NATURE,   2, 18 },
    { "WATERFALL",      RCAT_NATURE,   3, 35 },
    { "WILD CACTUS",    RCAT_NATURE,   2, 18 },
    { "RAINBOW",        RCAT_NATURE,   4, 75 },
    // ── FOOD ────────────────────────────────────────────────────────────────
    { "FAST FOOD",      RCAT_FOOD,     1, 10 },
    { "GAS STATION",    RCAT_FOOD,     1, 10 },
    { "OLD DINER",      RCAT_FOOD,     2, 18 },
    { "ICE CREAM STAND",RCAT_FOOD,     2, 18 },
    { "FARM STAND",     RCAT_FOOD,     2, 18 },
    { "FOOD TRUCK",     RCAT_FOOD,     2, 18 },
    { "BBQ SMOKER",     RCAT_FOOD,     3, 35 },
    // ── VEHICLE ─────────────────────────────────────────────────────────────
    { "SEMI TRUCK",     RCAT_VEHICLE,  1, 10 },
    { "MOTORCYCLE",     RCAT_VEHICLE,  1, 10 },
    { "RV CAMPER",      RCAT_VEHICLE,  1, 10 },
    { "SCHOOL BUS",     RCAT_VEHICLE,  1, 10 },
    { "POLICE CAR",     RCAT_VEHICLE,  2, 18 },
    { "TRACTOR",        RCAT_VEHICLE,  2, 18 },
    { "AMBULANCE",      RCAT_VEHICLE,  2, 18 },
    { "FIRE TRUCK",     RCAT_VEHICLE,  3, 35 },
    { "CAR ON A TRUCK", RCAT_VEHICLE,  3, 35 },
    { "HELICOPTER",     RCAT_VEHICLE,  4, 75 },
    // ── ANIMAL ──────────────────────────────────────────────────────────────
    { "DOG IN A CAR",   RCAT_ANIMAL,   1, 10 },
    { "ROADSIDE BIRD",  RCAT_ANIMAL,   1, 10 },
    { "DEER",           RCAT_ANIMAL,   2, 18 },
    { "HAWK ON A POLE", RCAT_ANIMAL,   2, 18 },
    { "WILD TURKEY",    RCAT_ANIMAL,   3, 35 },
    { "EAGLE",          RCAT_ANIMAL,   4, 75 },
    // ── LANDMARK ────────────────────────────────────────────────────────────
    { "WELCOME SIGN",   RCAT_LANDMARK, 1, 10 },
    { "OLD BARN",       RCAT_LANDMARK, 1, 10 },
    { "CHURCH STEEPLE", RCAT_LANDMARK, 1, 10 },
    { "HISTORIC MARKER",RCAT_LANDMARK, 2, 18 },
    { "GRAIN SILO",     RCAT_LANDMARK, 2, 18 },
    { "BIG STATUE",     RCAT_LANDMARK, 3, 35 },
    { "DRIVE-IN SCREEN",RCAT_LANDMARK, 3, 35 },
    // ── WEIRD ───────────────────────────────────────────────────────────────
    { "SHOE ON A WIRE", RCAT_WEIRD,    2, 18 },
    { "LOST COUCH",     RCAT_WEIRD,    2, 18 },
    { "GIANT FIBERGLASS",RCAT_WEIRD,   3, 35 },
    { "MYSTERY SPOT",   RCAT_WEIRD,    3, 35 },
    { "UFO SIGN",       RCAT_WEIRD,    4, 75 },
    // ── MANTIS ──────────────────────────────────────────────────────────────
    { "BTC ATM",        RCAT_MANTIS,   3, 35 },
    { "GREEN NEON",     RCAT_MANTIS,   2, 18 },
    { "A REAL MANTIS",  RCAT_MANTIS,   4, 99 },
};
static constexpr int NUM_ITEMS = sizeof(ITEMS) / sizeof(ITEMS[0]);

static uint16_t catColor(uint8_t c) {
    switch (c) {
        case RCAT_ROAD:     return TEAL_LIGHT;
        case RCAT_NATURE:   return LIME;
        case RCAT_FOOD:     return LIME_PALE;
        case RCAT_VEHICLE:  return TEAL_BRIGHT;
        case RCAT_ANIMAL:   return LIME_MED;
        case RCAT_LANDMARK: return PURPLE_LIGHT;
        case RCAT_WEIRD:    return PURPLE_BRIGHT;
        default:           return LIME;
    }
}

// 8x8 1-bit category icons (MSB = leftmost pixel). Prototype of the shared
// icon module that Sightings will formalize.
static const uint8_t RCAT_ICON[RCAT_COUNT][8] = {
    { 0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18 },  // ROAD     diamond sign
    { 0x18,0x3C,0x7E,0xFF,0x7E,0x18,0x18,0x3C },  // NATURE   tree
    { 0xFF,0x7E,0x7E,0x3C,0x3C,0x18,0x18,0x3C },  // FOOD     cup
    { 0x00,0x38,0x3C,0xFE,0xFF,0xFF,0x66,0x00 },  // VEHICLE  truck
    { 0x66,0x66,0x00,0x3C,0x7E,0x7E,0x3C,0x00 },  // ANIMAL   paw
    { 0x18,0x3C,0x7E,0x7E,0x66,0x66,0x66,0xFF },  // LANDMARK monument
    { 0x00,0x18,0x3C,0xFF,0xFF,0x3C,0x42,0x81 },  // WEIRD    ufo
    { 0x81,0x42,0x3C,0x7E,0xDB,0xFF,0x7E,0x3C },  // MANTIS   mantis head
};

static void drawIcon(uint8_t cat, int x, int y, uint16_t col) {
    if (cat >= RCAT_COUNT) return;
    for (int r = 0; r < 8; r++) {
        uint8_t bits = RCAT_ICON[cat][r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) fb.drawPixel(x + c, y + r, col);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WAYPOINTS  (the trip map)
// ─────────────────────────────────────────────────────────────────────────────
struct Waypoint { const char* name; uint16_t at; uint16_t color; };
static const Waypoint WAYPOINTS[] = {
    { "START",        0,   TH_TEAL_DARK    },
    { "GAS STOP",     25,  TH_LIME_DARK    },
    { "REST AREA",    55,  TH_TEAL_LIGHT   },
    { "SCENIC VIEW",  90,  TH_TEAL_BRIGHT  },
    { "SMALL TOWN",   130, TH_LIME_PALE    },
    { "STATE LINE",   175, TH_PURPLE       },
    { "MOUNTAIN PASS",225, TH_PURPLE_LIGHT },
    { "COAST HWY",    280, TH_TEAL_BRIGHT  },
    { "THE STRIP",    340, TH_LIME         },
    { "MANTIS DEN",   420, TH_LIME_PALE    },
};
static constexpr int NUM_WAYPOINTS = sizeof(WAYPOINTS) / sizeof(WAYPOINTS[0]);

// ─────────────────────────────────────────────────────────────────────────────
//  TROPHIES
// ─────────────────────────────────────────────────────────────────────────────
struct Trophy { const char* name; const char* desc; uint16_t color; };
static const Trophy TROPHIES[] = {
    { "FIRST BLOOD",  "Spot your first thing",    TH_LIME         },
    { "EAGLE EYE",    "3 rare finds in one trip", TH_PURPLE_LIGHT },
    { "CLEAN SWEEP",  "Clear a card, no skips",   TH_TEAL_BRIGHT  },
    { "ROAD WARRIOR", "Score 300+ in one trip",   TH_PURPLE       },
    { "SOCIALITE",    "Finish a Road Party",      TH_LIME_PALE    },
    { "CHECKERED FLAG","Win a Road Party",        TH_LIME         },
    { "MANTIS LORD",  "Spot A REAL MANTIS",       TH_LIME         },
    { "LONG HAULER",  "Complete 10 trips",        TH_TEAL_LIGHT   },
};
static constexpr int NUM_TROPHIES = sizeof(TROPHIES) / sizeof(TROPHIES[0]);
enum { TR_FIRST=0, TR_EAGLE, TR_SWEEP, TR_WARRIOR, TR_SOCIAL, TR_WIN, TR_MANTIS, TR_HAUL };

// Taunts fired when a rival claims something you still need.
static const char* TAUNTS[] = {
    "beat you to it",
    "eyes like a hawk",
    "were you even looking?",
    "too slow!",
    "that one's gone",
    "keep up back there",
    "called it first",
    "blink and you miss it",
};
static constexpr int NUM_TAUNTS = sizeof(TAUNTS) / sizeof(TAUNTS[0]);

static const char* NICKS[] = {
    "Waffles","Pickle","Noodle","Bandit","Gizmo","Sprocket","Tater","Biscuit",
    "Wombat","Pretzel","Snacky","Bloop","Zonk","Ziggy","Mochi","Gonzo",
};
static constexpr int NUM_NICKS = sizeof(NICKS) / sizeof(NICKS[0]);

// Trip length options the host picks in the lobby. 0 = no limit.
static const uint16_t TRIP_MINS[] = { 0, 30, 60, 120, 180, 240, 360, 480 };
static const char*    TRIP_LABEL[] = { "NO LIMIT","30 MIN","1 HOUR","2 HOURS",
                                       "3 HOURS","4 HOURS","6 HOURS","8 HOURS" };
static constexpr int NUM_TRIP_OPTS = sizeof(TRIP_MINS) / sizeof(TRIP_MINS[0]);

// ─────────────────────────────────────────────────────────────────────────────
//  PERSISTENCE  (one packed blob, coalesced writes)
// ─────────────────────────────────────────────────────────────────────────────
static const char* NVS_RH = "roadhunt";
static constexpr uint16_t RH_MAGIC = 0x5248;   // 'RH'
static constexpr uint8_t  RH_VER   = 1;

struct __attribute__((packed)) RhSave {
    uint16_t magic;
    uint8_t  version;
    uint8_t  trophies;            // bitmask, 8 trophies
    uint16_t bestScore;
    uint16_t runsDone;
    uint32_t lifetimeFinds;
    // in-progress trip so a pocket-sleep doesn't lose the drive
    uint8_t  runActive;
    uint8_t  card[CARD_SIZE];     // indices into ITEMS
    uint8_t  mark[CARD_SIZE];     // 0 open, 1 found, 2 skipped
    uint16_t score;
    uint8_t  skips;
    uint8_t  rares;
};
static RhSave sv;
static bool     svDirty    = false;
static uint32_t svLastWrite = 0;

static void rhDefaults() {
    memset(&sv, 0, sizeof(sv));
    sv.magic = RH_MAGIC;
    sv.version = RH_VER;
}

static void rhLoad() {
    rhDefaults();
    Preferences p;
    if (!p.begin(NVS_RH, true)) return;
    RhSave tmp;
    size_t n = p.getBytes("blob", &tmp, sizeof(tmp));
    p.end();
    if (n == sizeof(tmp) && tmp.magic == RH_MAGIC && tmp.version == RH_VER) sv = tmp;
}

static void rhFlush() {
    Preferences p;
    if (!p.begin(NVS_RH, false)) return;
    p.putBytes("blob", &sv, sizeof(sv));
    p.end();
    svDirty = false;
    svLastWrite = millis();
}

static void rhMarkDirty() { svDirty = true; rhFlush(); }  // spotting is a discrete checkpoint


// ─────────────────────────────────────────────────────────────────────────────
//  RUN STATE
// ─────────────────────────────────────────────────────────────────────────────
enum RhScreen : uint8_t {
    SC_TITLE = 0, SC_MENU, SC_LOBBY, SC_PLAY, SC_CLAIM, SC_TAUNT,
    SC_MAP, SC_STANDINGS, SC_RESULTS, SC_TROPHY
};
static RhScreen scr = SC_TITLE;
static RhScreen scrReturn = SC_PLAY;      // where CLAIM/TAUNT/MAP go back to

enum MenuAct : uint8_t { MA_RESUME = 0, MA_SOLO, MA_PARTY, MA_TROPHY, MA_BACK };
static const char* menuLabel[5];
static uint8_t     menuAct[5];
static int         menuCount = 0;

static bool     party      = false;       // multiplayer trip?
static int      cursor     = 0;
static int      menuSel    = 0;
static int      trophySel  = 0;
static uint32_t enterAt    = 0;
static uint32_t tripStart  = 0;
static uint16_t tripMins   = 0;           // 0 = no limit
static int      tripOpt    = 0;
static int      waypoint   = 0;
static bool     cardDone   = false;
static bool     wonTrip    = false;
static uint32_t animT      = 0;           // frame counter for the active anim
static bool     wpAdvanced = false;      // claim bumped us to a new waypoint
static int      claimItem  = 0;
static int      claimPts   = 0;
static char     tauntWho[RH_NAME_LEN];
static int      tauntItem  = 0;
static int      tauntIdx   = 0;
static bool     tauntMine  = false;       // did I already have it?
static int      newTrophy  = -1;
static uint32_t trophyAt   = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  RNG
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t rngS = 1;
static uint32_t rng() { rngS = rngS * 1103515245u + 12345u; return (rngS >> 8); }
static int rngRange(int a, int b) { if (b <= a) return a; return a + (int)(rng() % (uint32_t)(b - a + 1)); }

// ─────────────────────────────────────────────────────────────────────────────
//  INPUT  (tap fires on release; long fires once while held)
// ─────────────────────────────────────────────────────────────────────────────
static bool     lTap, rTap, lHoldFire, rHoldFire;
static bool     lDown, rDown;
static uint32_t lDownAt, rDownAt;
static bool     lLongDone, rLongDone;

static void rhInput() {
    lTap = rTap = lHoldFire = rHoldFire = false;
    uint32_t now = millis();

    if (btnL && !btnL_prev) { lDown = true; lDownAt = now; lLongDone = false; }
    if (btnL && lDown && !lLongDone && now - lDownAt > RH_LONG_MS) { lLongDone = true; lHoldFire = true; }
    if (!btnL && btnL_prev) { if (lDown && !lLongDone) lTap = true; lDown = false; }

    if (btnR && !btnR_prev) { rDown = true; rDownAt = now; rLongDone = false; }
    if (btnR && rDown && !rLongDone && now - rDownAt > RH_LONG_MS) { rLongDone = true; rHoldFire = true; }
    if (!btnR && btnR_prev) { if (rDown && !rLongDone) rTap = true; rDown = false; }

    // swallow the press that entered the app
    if (now - enterAt < RH_ENTER_GUARD) { lTap = rTap = lHoldFire = rHoldFire = false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ESP-NOW  (Versus/Pet pattern: fast callback, volatile stash, tick consumes)
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t RH_BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

enum RhMsgType : uint8_t { RM_HELLO = 1, RM_STATUS, RM_FIND, RM_START, RM_END, RM_BYE };

struct __attribute__((packed)) RhMsg {
    uint8_t  type;
    uint16_t id;
    uint8_t  seq;                  // per-sender event counter (FIND dedup)
    char     name[RH_NAME_LEN];
    uint16_t score;
    uint8_t  found;                // how many of their 12 are checked
    uint8_t  flags;                // bit0 = card complete
    uint8_t  itemIdx;              // ITEMS index for RM_FIND
    uint16_t tripMins;             // RM_START payload
};

struct RhPeer {
    uint16_t id;
    char     name[RH_NAME_LEN];
    uint16_t score;
    uint8_t  found;
    uint8_t  done;
    uint8_t  lastSeq;
    uint32_t lastSeen;
    uint8_t  mac[6];
    bool     active;
};
static RhPeer   peers[RH_MAX_PLAYERS];
static int      peerCount = 0;

static bool     espReady  = false;
static uint16_t myId      = 0;
static char     myName[RH_NAME_LEN];
static uint8_t  mySeq     = 0;
static uint32_t lastHello = 0;
static uint32_t lastStatus = 0;

// recv-callback stash
static volatile bool rxHello = false, rxStatus = false, rxFind = false;
static volatile bool rxStart = false, rxEnd = false, rxBye = false;
static RhMsg  rxBuf;
static uint8_t rxMac[6];

static void rhOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if ((size_t)len < sizeof(RhMsg)) return;
    memcpy(&rxBuf, data, sizeof(RhMsg));
    memcpy(rxMac, mac, 6);
    switch (rxBuf.type) {
        case RM_HELLO:  rxHello  = true; break;
        case RM_STATUS: rxStatus = true; break;
        case RM_FIND:   rxFind   = true; break;
        case RM_START:  rxStart  = true; break;
        case RM_END:    rxEnd    = true; break;
        case RM_BYE:    rxBye    = true; break;
        default: break;
    }
}

static void rhClearRx() {
    rxHello = rxStatus = rxFind = rxStart = rxEnd = rxBye = false;
}

static bool rhAddPeerMac(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

static void rhNickFor(uint16_t id, char* out) {
    snprintf(out, RH_NAME_LEN, "%s", NICKS[id % NUM_NICKS]);
    size_t l = strlen(out);
    if (l < RH_NAME_LEN - 2) snprintf(out + l, RH_NAME_LEN - l, "%X", id & 0xF);
}

static void rhSend(uint8_t type, const uint8_t* to, uint8_t itemIdx = 0, uint8_t seq = 0);

static bool rhNetBegin() {
    if (espReady) return true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    myId = (uint16_t)((mac[4] << 8) | mac[5]);
    if (myId == 0) myId = (uint16_t)(esp_random() & 0xFFFF);
    rhNickFor(myId, myName);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(rhOnRecv));
    rhAddPeerMac(RH_BCAST);
    espReady = true;
    rhClearRx();
    return true;
}

static void rhNetEnd() {
    if (espReady) {
        rhSend(RM_BYE, RH_BCAST);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        espReady = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    peerCount = 0;
    for (int i = 0; i < RH_MAX_PLAYERS; i++) peers[i].active = false;
    rhClearRx();
}

// forward decls used by rhSend
static int rhFoundCount();
static uint16_t rhScore();

static void rhSend(uint8_t type, const uint8_t* to, uint8_t itemIdx, uint8_t seq) {
    if (!espReady) return;
    RhMsg m; memset(&m, 0, sizeof(m));
    m.type = type;
    m.id = myId;
    m.seq = seq;
    memcpy(m.name, myName, RH_NAME_LEN);
    m.name[RH_NAME_LEN - 1] = '\0';
    m.score = rhScore();
    m.found = (uint8_t)rhFoundCount();
    m.flags = cardDone ? 1 : 0;
    m.itemIdx = itemIdx;
    m.tripMins = tripMins;
    esp_now_send(to, (const uint8_t*)&m, sizeof(m));
}

static int rhPeerIndex(uint16_t id) {
    for (int i = 0; i < peerCount; i++) if (peers[i].active && peers[i].id == id) return i;
    return -1;
}

// Register or refresh a peer from whatever message just arrived.
static int rhTouchPeer(const RhMsg& m, const uint8_t* mac) {
    if (m.id == myId) return -1;
    int i = rhPeerIndex(m.id);
    if (i < 0) {
        if (peerCount >= RH_MAX_PLAYERS) return -1;
        i = peerCount++;
        memset(&peers[i], 0, sizeof(RhPeer));
        peers[i].id = m.id;
        peers[i].lastSeq = 0xFF;
        peers[i].active = true;
        memcpy(peers[i].mac, mac, 6);
        rhAddPeerMac(mac);
    }
    memcpy(peers[i].name, m.name, RH_NAME_LEN);
    peers[i].name[RH_NAME_LEN - 1] = '\0';
    peers[i].score = m.score;
    peers[i].found = m.found;
    peers[i].done  = (m.flags & 1) ? 1 : 0;
    peers[i].lastSeen = millis();
    return i;
}

static void rhPrunePeers() {
    uint32_t now = millis();
    for (int i = 0; i < peerCount; i++) {
        if (peers[i].active && now - peers[i].lastSeen > RH_PEER_TIMEOUT) peers[i].active = false;
    }
}

// Deterministic host: lowest id in the room (matches the id-comparison trick
// Versus and Pet use to pick a role without negotiation).
static bool rhIsHost() {
    if (!party) return true;
    for (int i = 0; i < peerCount; i++)
        if (peers[i].active && peers[i].id < myId) return false;
    return true;
}

static int rhActivePeers() {
    int n = 0;
    for (int i = 0; i < peerCount; i++) if (peers[i].active) n++;
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CARD
// ─────────────────────────────────────────────────────────────────────────────
static int rhFoundCount() {
    int n = 0;
    for (int i = 0; i < CARD_SIZE; i++) if (sv.mark[i] == 1) n++;
    return n;
}
static uint16_t rhScore() { return sv.score; }

static int rhOpenCount() {
    int n = 0;
    for (int i = 0; i < CARD_SIZE; i++) if (sv.mark[i] == 0) n++;
    return n;
}

// Pull one unused item of a given rarity.
static int pickRarity(uint8_t rarity, const bool* used) {
    int pool[NUM_ITEMS], n = 0;
    for (int i = 0; i < NUM_ITEMS; i++)
        if (!used[i] && ITEMS[i].rarity == rarity) pool[n++] = i;
    if (n == 0) {
        for (int i = 0; i < NUM_ITEMS; i++) if (!used[i]) pool[n++] = i;
    }
    if (n == 0) return 0;
    return pool[rngRange(0, n - 1)];
}

// Every player rolls their OWN card. Composition is fixed so the difficulty
// curve is consistent; the picks inside each tier are random.
static void rhBuildCard() {
    bool used[NUM_ITEMS];
    memset(used, 0, sizeof(used));
    static const uint8_t MIX[CARD_SIZE] = { 1,1,1,1,1, 2,2,2,2, 3,3, 4 };
    for (int i = 0; i < CARD_SIZE; i++) {
        int idx = pickRarity(MIX[i], used);
        used[idx] = true;
        sv.card[i] = (uint8_t)idx;
        sv.mark[i] = 0;
    }
    // shuffle so the legendary isn't always last
    for (int i = CARD_SIZE - 1; i > 0; i--) {
        int j = rngRange(0, i);
        uint8_t t = sv.card[i]; sv.card[i] = sv.card[j]; sv.card[j] = t;
    }
}

static void rhNewRun() {
    rngS = esp_random() ^ millis();
    rhBuildCard();
    sv.score = 0;
    sv.skips = 0;
    sv.rares = 0;
    sv.runActive = 1;
    cursor = 0;
    waypoint = 0;
    cardDone = false;
    wonTrip = false;
    tripStart = millis();
    rhMarkDirty();
}

static void rhBuildMenu() {
    menuCount = 0;
    if (sv.runActive) { menuLabel[menuCount] = "RESUME TRIP";   menuAct[menuCount++] = MA_RESUME; }
    menuLabel[menuCount] = sv.runActive ? "NEW SOLO TRIP" : "SOLO DRIVE";
    menuAct[menuCount++] = MA_SOLO;
    menuLabel[menuCount] = "ROAD PARTY";  menuAct[menuCount++] = MA_PARTY;
    menuLabel[menuCount] = "TROPHY CASE"; menuAct[menuCount++] = MA_TROPHY;
    menuLabel[menuCount] = "BACK";        menuAct[menuCount++] = MA_BACK;
}

static void rhUpdateWaypoint() {
    int wp = 0;
    for (int i = 1; i < NUM_WAYPOINTS; i++) if (sv.score >= WAYPOINTS[i].at) wp = i;
    waypoint = wp;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TROPHIES + XP
// ─────────────────────────────────────────────────────────────────────────────
static bool rhHasTrophy(int i) { return (sv.trophies >> i) & 1; }

static void rhGrantTrophy(int i) {
    if (rhHasTrophy(i)) return;
    sv.trophies |= (uint8_t)(1 << i);
    newTrophy = i;
    trophyAt = millis();
    qpAddXp(40);
    rhMarkDirty();
}

// XP is event-driven, never per-point (matches how CYOA/Aquarium pay out).
static void rhAwardTripXp() {
    uint32_t xp = 30;                              // finishing a trip at all
    if (cardDone) xp += 25;                        // cleared the card
    if (cardDone && sv.skips == 0) xp += 15;       // clean sweep
    if (wonTrip) xp += 60;                         // won a Road Party
    qpAddXp(xp);
    qpGrantPetXp(xp / 4);                          // the mantis eats too
    if (cardDone && sv.rares >= 3) qpAddItem(ITEM_CY_LUCKCHARM, 1);
}

static void rhCheckTrophies() {
    if (rhFoundCount() >= 1) rhGrantTrophy(TR_FIRST);
    if (sv.rares >= 3)       rhGrantTrophy(TR_EAGLE);
    if (sv.score >= 300)     rhGrantTrophy(TR_WARRIOR);
    for (int i = 0; i < CARD_SIZE; i++) {
        if (sv.mark[i] == 1 && strcmp(ITEMS[sv.card[i]].name, "A REAL MANTIS") == 0)
            rhGrantTrophy(TR_MANTIS);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACTIONS
// ─────────────────────────────────────────────────────────────────────────────
static void rhMoveCursor() {
    // step to the next still-open item; if none are open we're done anyway
    if (rhOpenCount() == 0) return;
    for (int k = 0; k < CARD_SIZE; k++) {
        cursor = (cursor + 1) % CARD_SIZE;
        if (sv.mark[cursor] == 0) return;
    }
}

static void rhEndTrip(bool completed);

static void rhClaim() {
    if (sv.mark[cursor] != 0) return;
    const ScavItem& it = ITEMS[sv.card[cursor]];
    int pts = it.points;
    sv.mark[cursor] = 1;
    sv.score = (uint16_t)(sv.score + pts);
    sv.lifetimeFinds++;
    if (it.rarity >= 3) sv.rares++;
    if (it.rarity >= 4) qpAddXp(15);               // legendary spot is its own reward

    claimItem = sv.card[cursor];
    claimPts = pts;
    animT = 0;
    scrReturn = SC_PLAY;
    scr = SC_CLAIM;

    int wpBefore = waypoint;
    rhUpdateWaypoint();
    wpAdvanced = (waypoint > wpBefore);
    rhCheckTrophies();
    rhMarkDirty();

    if (party && espReady) {
        mySeq++;
        rhSend(RM_FIND, RH_BCAST, (uint8_t)claimItem, mySeq);   // push it NOW
    }

    if (rhOpenCount() == 0) {
        cardDone = true;
        if (sv.skips == 0) rhGrantTrophy(TR_SWEEP);
    }
    rhMoveCursor();
}

static void rhSkip() {
    if (sv.mark[cursor] != 0) return;
    sv.mark[cursor] = 2;
    sv.skips++;
    int s = (int)sv.score - SKIP_COST;
    sv.score = (uint16_t)(s < 0 ? 0 : s);
    rhUpdateWaypoint();
    rhMarkDirty();
    if (rhOpenCount() == 0) cardDone = true;
    rhMoveCursor();
}

// Am I ahead of every active peer?
static bool rhAmLeading() {
    for (int i = 0; i < peerCount; i++)
        if (peers[i].active && peers[i].score > sv.score) return false;
    return true;
}

static void rhEndTrip(bool completed) {
    (void)completed;
    if (party) {
        wonTrip = rhAmLeading();
        rhGrantTrophy(TR_SOCIAL);
        if (wonTrip) rhGrantTrophy(TR_WIN);
    }
    sv.runsDone++;
    if (sv.runsDone >= 10) rhGrantTrophy(TR_HAUL);
    if (sv.score > sv.bestScore) sv.bestScore = sv.score;
    rhAwardTripXp();
    sv.runActive = 0;
    scr = SC_RESULTS;
    animT = 0;
    rhFlush();                                     // forced write: trip is over
}

// ─────────────────────────────────────────────────────────────────────────────
//  TIME
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t rhElapsedSec() { return (millis() - tripStart) / 1000; }

static uint32_t rhRemainSec() {
    if (tripMins == 0) return 0;
    uint32_t total = (uint32_t)tripMins * 60;
    uint32_t el = rhElapsedSec();
    return (el >= total) ? 0 : total - el;
}

static void fmtHMS(uint32_t secs, char* out, size_t n) {
    uint32_t h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0) snprintf(out, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else       snprintf(out, n, "%u:%02u", (unsigned)m, (unsigned)s);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────────────────────────────────────
static void drawHeader(const char* title) {
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(title, 4, 9, 1);
}

static void drawFooter(const char* left, const char* right) {
    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM);
    fb.drawString(left, 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM);
    fb.drawString(right, W - 4, H - 7, 1);
}

// Little side-view car used all over the app.
static void drawCar(int x, int y, uint32_t phase, uint16_t body) {
    int by = y + (int)(sinf(phase * 0.05f) * 2.0f);
    fb.fillRect(x - 12, by - 6, 24, 10, body);
    fb.fillRect(x - 8,  by - 11, 15, 6, TEAL_BRIGHT);
    fb.fillCircle(x - 8, by + 4, 4, TEAL_DARK);
    fb.fillCircle(x + 8, by + 4, 4, TEAL_DARK);
    fb.fillCircle(x - 8, by + 4, 2, TEAL_LIGHT);
    fb.fillCircle(x + 8, by + 4, 2, TEAL_LIGHT);
    fb.fillCircle(x + 11, by - 3, 2, LIME_PALE);
    fb.drawLine(x - 3, by - 11, x - 5, by - 18, TEAL_LIGHT);
    fb.fillCircle(x - 5, by - 18, 1, LIME);
}

static void drawRoad(int y, uint32_t phase) {
    fb.fillRect(0, y, W, H - y - 14, TEAL_DARK);
    int off = (int)((phase * 2) % 20);
    for (int x = -off; x < W; x += 20) fb.drawLine(x, y + 9, x + 10, y + 9, TEAL_LIGHT);
    fb.drawLine(4, y, 4, H - 14, TEAL_BRIGHT);
    fb.drawLine(W - 5, y, W - 5, H - 14, TEAL_BRIGHT);
}

// Scrolling roadside scenery: poles + hills, cheap parallax.
static void drawScenery(int horizon, uint32_t phase) {
    int hoff = (int)((phase / 3) % 60);
    for (int x = -hoff; x < W + 60; x += 60) {
        int peak = 18 + ((x * 7 + 43) % 14);
        fb.fillTriangle(x, horizon, x + 30, horizon - peak, x + 60, horizon, TEAL_DARK);
    }
    int poff = (int)(phase % 48);
    for (int x = -poff; x < W + 48; x += 48) {
        fb.drawLine(x, horizon, x, horizon - 22, TEAL_DARK);
        fb.drawLine(x - 4, horizon - 20, x + 4, horizon - 20, TEAL_DARK);
    }
}

static void drawProgressBar(int x, int y, int w, int h, int val, int max, uint16_t fg) {
    fb.fillRect(x, y, w, h, TEAL_DARK);
    if (max > 0) {
        int pw = (val * w) / max;
        if (pw > w) pw = w;
        if (pw > 0) fb.fillRect(x, y, pw, h, fg);
    }
}

static void drawStarburst(int cx, int cy, uint32_t t, uint16_t col) {
    for (int s = 0; s < 8; s++) {
        float a = s * 0.785f + t * 0.05f;
        int rr = 14 + (int)(t * 1.6f);
        if (rr > 58) rr = 58;
        int sx = cx + (int)(cosf(a) * rr);
        int sy = cy + (int)(sinf(a) * rr);
        int r = 3 - (int)(t / 14);
        if (r < 1) r = 1;
        fb.fillCircle(sx, sy, r, col);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREENS
// ─────────────────────────────────────────────────────────────────────────────
static void drawTitle() {
    fb.fillSprite(TEAL_DEEP);
    uint32_t ph = millis() / 20;
    drawScenery(196, ph);
    drawRoad(200, ph);
    drawCar(W / 2, 214, ph, LIME);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("ROAD HUNT", W / 2, 58, 4);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("SCAVENGER", W / 2, 84, 2);
    fb.drawLine(24, 100, W - 24, 100, TEAL_DARK);
    fb.drawLine(34, 103, W - 34, 103, TEAL_DARK);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("SPOT IT BEFORE THEY DO", W / 2, 118, 1);

    if (sv.bestScore > 0) {
        char b[24];
        snprintf(b, sizeof(b), "BEST %u", (unsigned)sv.bestScore);
        fb.setTextColor(LIME);
        fb.drawString(b, W / 2, 140, 1);
    }
    if ((millis() / 600) % 2 == 0) {
        fb.setTextColor(LIME);
        fb.drawString("PRESS TO START", W / 2, 165, 2);
    }
    drawFooter("L/R: start", "");
}

static void drawMenu() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("ROAD HUNT");
    int top = 46 + (5 - menuCount) * 16;
    for (int i = 0; i < menuCount; i++) {
        int y = top + i * 38;
        bool sel = (i == menuSel);
        if (sel) {
            fb.fillRoundRect(10, y - 12, W - 20, 30, 4, PURPLE_DARK);
            fb.drawRoundRect(10, y - 12, W - 20, 30, 4, LIME);
            fb.setTextColor(LIME_PALE);
        } else {
            fb.setTextColor(TEAL_LIGHT);
        }
        fb.setTextDatum(MC_DATUM);
        fb.drawString(menuLabel[i], W / 2, y + 3, 2);
    }
    if (sv.runActive) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_DARK);
        char b[28];
        snprintf(b, sizeof(b), "trip in progress: %d/%d", rhFoundCount(), CARD_SIZE);
        fb.drawString(b, W / 2, top + menuCount * 38 + 4, 1);
    }
    drawCar(W / 2, 262, millis() / 24, LIME_DARK);
    drawFooter("L: scroll", "R: select");
}

static void drawLobby() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("ROAD PARTY");

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("LOOKING FOR RIDERS", W / 2, 30, 1);
    int dots = (millis() / 400) % 4;
    char d[5] = { 0 };
    for (int i = 0; i < dots; i++) d[i] = '.';
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(d, W / 2, 44, 2);

    int y = 62;
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME);
    fb.drawString(myName, 14, y, 1);
    fb.setTextDatum(MR_DATUM);
    fb.setTextColor(rhIsHost() ? LIME_PALE : TEAL_DARK);
    fb.drawString(rhIsHost() ? "YOU (HOST)" : "YOU", W - 14, y, 1);
    y += 16;
    for (int i = 0; i < peerCount; i++) {
        if (!peers[i].active) continue;
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString(peers[i].name, 14, y, 1);
        y += 16;
        if (y > 150) break;
    }
    if (rhActivePeers() == 0) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("nobody else yet", W / 2, y + 6, 1);
    }

    // trip length selector (host only)
    fb.fillRoundRect(10, 176, W - 20, 46, 4, PURPLE_DARK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("TRIP LENGTH", W / 2, 188, 1);
    fb.setTextColor(rhIsHost() ? LIME_PALE : TEAL_DARK);
    fb.drawString(TRIP_LABEL[tripOpt], W / 2, 208, 2);

    fb.setTextColor(TEAL_DARK);
    fb.drawString("host can end the trip", W / 2, 234, 1);
    fb.drawString("any time from standings", W / 2, 246, 1);

    drawCar(W / 2, 276, millis() / 24, PURPLE);

    if (rhIsHost()) drawFooter("L: length", "R: ROLL OUT");
    else            drawFooter("L: leave", "waiting for host");
}

static void drawPlay() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader(party ? "ROAD PARTY" : "SOLO DRIVE");

    char buf[28];

    // score / waypoint / clock strip
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(LIME_PALE);
    snprintf(buf, sizeof(buf), "%u", (unsigned)sv.score);
    fb.drawString(buf, 4, 21, 2);

    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(TEAL_DARK);
    if (tripMins > 0) fmtHMS(rhRemainSec(), buf, sizeof(buf));
    else              fmtHMS(rhElapsedSec(), buf, sizeof(buf));
    fb.drawString(buf, W - 4, 22, 1);

    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(thc(WAYPOINTS[waypoint].color));
    fb.drawString(WAYPOINTS[waypoint].name, W / 2, 22, 1);

    drawProgressBar(4, 38, W - 8, 4, rhFoundCount(), CARD_SIZE, LIME);

    // the card
    const int ROW_H = 26;
    const int VIS   = 8;
    int start = cursor - 3;
    if (start > CARD_SIZE - VIS) start = CARD_SIZE - VIS;
    if (start < 0) start = 0;

    for (int r = 0; r < VIS && (start + r) < CARD_SIZE; r++) {
        int i = start + r;
        int y = 46 + r * ROW_H;
        const ScavItem& it = ITEMS[sv.card[i]];
        bool sel   = (i == cursor);
        bool found = (sv.mark[i] == 1);
        bool skip  = (sv.mark[i] == 2);

        if (sel) {
            fb.fillRoundRect(2, y, W - 4, ROW_H - 2, 3, PURPLE_DARK);
            fb.drawRoundRect(2, y, W - 4, ROW_H - 2, 3, LIME_DARK);
        }

        uint16_t cc = catColor(it.cat);
        drawIcon(it.cat, 7, y + 8, found ? TEAL_DARK : cc);

        fb.setTextDatum(ML_DATUM);
        if (found) {
            fb.setTextColor(TEAL_DARK);
            fb.drawString(it.name, 20, y + 12, 1);
            fb.setTextColor(LIME);
            fb.setTextDatum(MR_DATUM);
            fb.drawString("GOT IT", W - 8, y + 12, 1);
        } else if (skip) {
            fb.setTextColor(TEAL_DARK);
            fb.drawString(it.name, 20, y + 12, 1);
            fb.setTextDatum(MR_DATUM);
            fb.drawString("SKIP", W - 8, y + 12, 1);
        } else {
            fb.setTextColor(sel ? LIME_PALE : TEAL_BRIGHT);
            fb.drawString(it.name, 20, y + 12, 1);
            for (int d = 0; d < it.rarity; d++)
                fb.fillCircle(W - 9 - d * 6, y + 12, 2, cc);
        }
    }

    // live rival ticker
    if (party) {
        int n = rhActivePeers();
        if (n > 0) {
            fb.fillRect(0, H - 28, W, 14, TEAL_DARK);
            fb.setTextDatum(ML_DATUM);
            int x = 4;
            for (int i = 0; i < peerCount && x < W - 30; i++) {
                if (!peers[i].active) continue;
                bool ahead = peers[i].score > sv.score;
                fb.setTextColor(ahead ? PURPLE_LIGHT : TEAL_LIGHT);
                snprintf(buf, sizeof(buf), "%s %u/%u", peers[i].name,
                         (unsigned)peers[i].found, (unsigned)CARD_SIZE);
                fb.drawString(buf, x, H - 21, 1);
                x += fb.textWidth(buf, 1) + 8;
            }
        }
    }

    drawFooter("L: next  hold: skip", "R: GOT IT");
}

static void drawClaim() {
    drawPlay();
    const ScavItem& it = ITEMS[claimItem];
    fb.fillRoundRect(12, 92, W - 24, 116, 6, TEAL_DEEP);
    fb.drawRoundRect(12, 92, W - 24, 116, 6, LIME);

    drawStarburst(W / 2, 132, animT, LIME_PALE);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(animT < 12 ? "SPOTTED!" : "GOT IT!", W / 2, 112, 2);

    drawIcon(it.cat, W / 2 - 4, 126, catColor(it.cat));

    fb.setTextColor(catColor(it.cat));
    fb.drawString(it.name, W / 2, 150, 1);

    char b[16];
    snprintf(b, sizeof(b), "+%d", claimPts);
    fb.setTextColor(TEAL_BRIGHT);
    int bob = 172 + (int)(sinf(animT * 0.25f) * 4.0f);
    fb.drawString(b, W / 2, bob, 4);

    if (it.rarity >= 4) {
        fb.setTextColor(LIME);
        fb.drawString("LEGENDARY!", W / 2, 196, 1);
    }
}

static void drawTaunt() {
    drawPlay();
    const ScavItem& it = ITEMS[tauntItem];
    uint16_t edge = tauntMine ? TEAL_LIGHT : PURPLE_LIGHT;

    fb.fillRoundRect(10, 100, W - 20, 100, 6, PURPLE_DARK);
    fb.drawRoundRect(10, 100, W - 20, 100, 6, edge);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(edge);
    fb.drawString(tauntWho, W / 2, 118, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("spotted", W / 2, 136, 1);

    fb.setTextColor(catColor(it.cat));
    fb.drawString(it.name, W / 2, 154, 2);

    fb.setTextColor(tauntMine ? TEAL_DARK : LIME_PALE);
    fb.drawString(tauntMine ? "you already had that one" : TAUNTS[tauntIdx], W / 2, 178, 1);

    // a flashing border sells the urgency
    if (!tauntMine && (animT / 5) % 2 == 0) {
        for (int t = 0; t < 3; t++) fb.drawRect(t, t, W - 2 * t, H - 2 * t, PURPLE_LIGHT);
    }
}

static void drawMap() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("TRIP MAP");

    int ry[NUM_WAYPOINTS];
    for (int i = 0; i < NUM_WAYPOINTS; i++) {
        int fx = 18 + (i * (W - 36)) / (NUM_WAYPOINTS - 1);
        ry[i] = 70 + (int)(sinf(i * 0.85f) * 26.0f) + (int)(cosf(i * 1.25f) * 12.0f);
        bool reached = (i <= waypoint);
        if (i > 0) {
            int px = 18 + ((i - 1) * (W - 36)) / (NUM_WAYPOINTS - 1);
            fb.drawLine(px, ry[i - 1], fx, ry[i], reached ? TEAL_LIGHT : TEAL_DARK);
        }
        fb.fillCircle(fx, ry[i], reached ? 5 : 3, reached ? thc(WAYPOINTS[i].color) : TEAL_DARK);
        fb.drawCircle(fx, ry[i], reached ? 5 : 3, TEAL_LIGHT);
    }
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(thc(WAYPOINTS[waypoint].color));
    fb.drawString(WAYPOINTS[waypoint].name, W / 2, 140, 2);

    int cx = 18 + (waypoint * (W - 36)) / (NUM_WAYPOINTS - 1);
    drawCar(cx, ry[waypoint] - 16, millis() / 24, LIME);

    char b[24];
    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "%u PTS", (unsigned)sv.score);
    fb.drawString(b, W / 2, 168, 2);

    if (waypoint + 1 < NUM_WAYPOINTS) {
        fb.setTextColor(TEAL_DARK);
        snprintf(b, sizeof(b), "%u to %s",
                 (unsigned)(WAYPOINTS[waypoint + 1].at - sv.score), WAYPOINTS[waypoint + 1].name);
        fb.drawString(b, W / 2, 188, 1);
    }
    drawFooter("L/R: back", "");
}

// Sort helper shared by standings + results.
static void rhBuildBoard(uint16_t* sc, char names[][RH_NAME_LEN], uint8_t* fnd, int* total, int* meIdx) {
    int n = 0;
    sc[n] = sv.score;
    snprintf(names[n], RH_NAME_LEN, "%s", party ? myName : "YOU");
    fnd[n] = (uint8_t)rhFoundCount();
    n++;
    for (int i = 0; i < peerCount && n < RH_MAX_PLAYERS + 1; i++) {
        if (!peers[i].active) continue;
        sc[n] = peers[i].score;
        snprintf(names[n], RH_NAME_LEN, "%s", peers[i].name);
        fnd[n] = peers[i].found;
        n++;
    }
    int me = 0;
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (sc[j] < sc[j + 1]) {
                uint16_t ts = sc[j]; sc[j] = sc[j + 1]; sc[j + 1] = ts;
                uint8_t tf = fnd[j]; fnd[j] = fnd[j + 1]; fnd[j + 1] = tf;
                char tn[RH_NAME_LEN];
                memcpy(tn, names[j], RH_NAME_LEN);
                memcpy(names[j], names[j + 1], RH_NAME_LEN);
                memcpy(names[j + 1], tn, RH_NAME_LEN);
                if (me == j) me = j + 1; else if (me == j + 1) me = j;
            }
        }
    }
    *total = n;
    *meIdx = me;
}

static void drawStandings() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("STANDINGS");

    uint16_t sc[RH_MAX_PLAYERS + 1];
    char nm[RH_MAX_PLAYERS + 1][RH_NAME_LEN];
    uint8_t fd[RH_MAX_PLAYERS + 1];
    int total = 0, me = 0;
    rhBuildBoard(sc, nm, fd, &total, &me);

    for (int i = 0; i < total; i++) {
        int y = 30 + i * 34;
        bool isMe = (i == me);
        if (isMe) {
            fb.fillRoundRect(4, y, W - 8, 30, 3, PURPLE_DARK);
            fb.drawRoundRect(4, y, W - 8, 30, 3, LIME_DARK);
        }
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(i == 0 ? LIME : (isMe ? LIME_PALE : TEAL_LIGHT));
        char b[28];
        snprintf(b, sizeof(b), "%d. %s", i + 1, nm[i]);
        fb.drawString(b, 10, y + 10, 2);
        fb.setTextColor(TEAL_DARK);
        snprintf(b, sizeof(b), "%u/%u spotted", (unsigned)fd[i], (unsigned)CARD_SIZE);
        fb.drawString(b, 10, y + 23, 1);
        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(TEAL_BRIGHT);
        snprintf(b, sizeof(b), "%u", (unsigned)sc[i]);
        fb.drawString(b, W - 10, y + 14, 2);
    }

    if (party && rhIsHost()) drawFooter("hold L: END TRIP", "R: back");
    else                     drawFooter("L/R: back", "");
}

static void drawResults() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("TRIP COMPLETE");

    char b[28];
    fb.setTextDatum(MC_DATUM);

    if (party) {
        fb.setTextColor(wonTrip ? LIME : PURPLE_LIGHT);
        fb.drawString(wonTrip ? "YOU WIN!" : "TRIP OVER", W / 2, 40, 4);
    } else {
        fb.setTextColor(cardDone ? LIME : TEAL_BRIGHT);
        fb.drawString(cardDone ? "CARD CLEARED!" : "TRIP OVER", W / 2, 40, 2);
    }

    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "%u", (unsigned)sv.score);
    fb.drawString(b, W / 2, 74, 4);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("POINTS", W / 2, 96, 1);

    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b, sizeof(b), "Spotted %d of %d", rhFoundCount(), CARD_SIZE);
    fb.drawString(b, W / 2, 118, 1);
    fmtHMS(rhElapsedSec(), b, sizeof(b));
    char t2[40];
    snprintf(t2, sizeof(t2), "Drive time %s", b);
    fb.drawString(t2, W / 2, 132, 1);
    snprintf(b, sizeof(b), "Skips %u   Rares %u", (unsigned)sv.skips, (unsigned)sv.rares);
    fb.drawString(b, W / 2, 146, 1);

    if (sv.score >= sv.bestScore && sv.score > 0) {
        fb.setTextColor(LIME);
        fb.drawString("NEW BEST!", W / 2, 166, 2);
    } else {
        fb.setTextColor(TEAL_DARK);
        snprintf(b, sizeof(b), "Best %u", (unsigned)sv.bestScore);
        fb.drawString(b, W / 2, 166, 1);
    }

    fb.setTextColor(thc(WAYPOINTS[waypoint].color));
    fb.drawString(WAYPOINTS[waypoint].name, W / 2, 186, 1);

    if (newTrophy >= 0 && millis() - trophyAt < 6000) {
        fb.fillRoundRect(10, 202, W - 20, 40, 4, PURPLE_DARK);
        fb.drawRoundRect(10, 202, W - 20, 40, 4, LIME);
        fb.setTextColor(LIME_PALE);
        fb.drawString("TROPHY UNLOCKED", W / 2, 214, 1);
        fb.setTextColor(thc(TROPHIES[newTrophy].color));
        fb.drawString(TROPHIES[newTrophy].name, W / 2, 231, 1);
    }

    drawCar(W / 2, 266, millis() / 24, LIME_DARK);
    drawFooter("L/R: menu", "");
}

static void drawTrophies() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("TROPHY CASE");
    int got = 0;
    for (int i = 0; i < NUM_TROPHIES; i++) if (rhHasTrophy(i)) got++;
    char b[28];
    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b, sizeof(b), "%d/%d", got, NUM_TROPHIES);
    fb.drawString(b, W - 4, 22, 1);

    for (int i = 0; i < NUM_TROPHIES; i++) {
        int y = 36 + i * 31;
        bool un = rhHasTrophy(i);
        bool sel = (i == trophySel);
        if (sel) {
            fb.fillRoundRect(4, y - 2, W - 8, 29, 3, PURPLE_DARK);
            fb.drawRoundRect(4, y - 2, W - 8, 29, 3, LIME_DARK);
        }
        uint16_t c = un ? thc(TROPHIES[i].color) : TEAL_DARK;
        fb.fillRect(10, y + 5, 12, 12, c);
        fb.drawRect(10, y + 5, 12, 12, un ? TEAL_LIGHT : TEAL_DARK);
        if (!un) {
            fb.drawLine(12, y + 8, 20, y + 15, TEAL_DEEP);
            fb.drawLine(20, y + 8, 12, y + 15, TEAL_DEEP);
        }
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(un ? TEAL_BRIGHT : TEAL_DARK);
        fb.drawString(TROPHIES[i].name, 30, y + 8, 1);
        fb.setTextColor(un ? TEAL_LIGHT : TEAL_DARK);
        fb.drawString(TROPHIES[i].desc, 30, y + 20, 1);
    }
    drawFooter("L: scroll", "R: back");
}

// ─────────────────────────────────────────────────────────────────────────────
//  NET CONSUMPTION  (all logic here, never in the callback)
// ─────────────────────────────────────────────────────────────────────────────
static void rhConsumeNet() {
    if (!party || !espReady) return;

    if (rxHello)  { rxHello  = false; rhTouchPeer(rxBuf, rxMac); }
    if (rxStatus) { rxStatus = false; rhTouchPeer(rxBuf, rxMac); }

    if (rxFind) {
        rxFind = false;
        int pi = rhTouchPeer(rxBuf, rxMac);
        if (pi >= 0 && peers[pi].lastSeq != rxBuf.seq) {    // dedup repeats
            peers[pi].lastSeq = rxBuf.seq;
            if (scr == SC_PLAY || scr == SC_CLAIM) {
                uint8_t idx = rxBuf.itemIdx;
                if (idx < NUM_ITEMS) {
                    tauntItem = idx;
                    snprintf(tauntWho, sizeof(tauntWho), "%s", peers[pi].name);
                    tauntIdx = rngRange(0, NUM_TAUNTS - 1);
                    // sting harder if it's still open on MY card
                    tauntMine = true;
                    for (int i = 0; i < CARD_SIZE; i++)
                        if (sv.card[i] == idx && sv.mark[i] == 0) { tauntMine = false; break; }
                    animT = 0;
                    scrReturn = SC_PLAY;
                    scr = SC_TAUNT;
                }
            }
        }
    }

    if (rxStart) {
        rxStart = false;
        if (scr == SC_LOBBY) {
            rhTouchPeer(rxBuf, rxMac);
            tripMins = rxBuf.tripMins;
            rhNewRun();
            scr = SC_PLAY;
        }
    }

    if (rxEnd) {
        rxEnd = false;
        if (scr == SC_PLAY || scr == SC_CLAIM || scr == SC_TAUNT ||
            scr == SC_MAP  || scr == SC_STANDINGS) {
            rhTouchPeer(rxBuf, rxMac);
            rhEndTrip(false);
        }
    }

    if (rxBye) {
        rxBye = false;
        int pi = rhPeerIndex(rxBuf.id);
        if (pi >= 0) peers[pi].active = false;
    }

    rhPrunePeers();
}

// ─────────────────────────────────────────────────────────────────────────────
//  LIFECYCLE
// ─────────────────────────────────────────────────────────────────────────────
void roadhuntEnter() {
    pgRegisterFlush(rhFlush);
    fb.setRotation(0);
    rhLoad();
    scr = SC_TITLE;
    party = false;
    menuSel = 0;
    trophySel = 0;
    cursor = 0;
    tripOpt = 0;
    tripMins = 0;
    cardDone = false;
    wonTrip = false;
    newTrophy = -1;
    peerCount = 0;
    espReady = false;
    animT = 0;
    enterAt = millis();
    svLastWrite = millis();
    svDirty = false;
    lDown = rDown = false;
    lLongDone = rLongDone = false;
    rngS = esp_random() ^ millis();
    for (int i = 0; i < RH_MAX_PLAYERS; i++) peers[i].active = false;
    rhUpdateWaypoint();
    rhBuildMenu();
    // A trip left running (device slept mid-drive) resumes from the menu.
    if (sv.runActive) { tripStart = millis(); cursor = 0; rhMoveCursor(); }
}

void roadhuntShutdown() {
    if (espReady) rhNetEnd();   // solo never brought the radio up at all
    if (svDirty)  rhFlush();
}

void roadhuntTick() {
    rhInput();
    uint32_t now = millis();

    rhConsumeNet();

    switch (scr) {

    case SC_TITLE:
        if (lTap || rTap) { rhBuildMenu(); scr = SC_MENU; menuSel = 0; }
        break;

    case SC_MENU:
        if (lTap) menuSel = (menuSel + 1) % menuCount;
        if (rTap) {
            switch (menuAct[menuSel]) {
            case MA_RESUME:
                party = false;
                tripMins = 0;
                tripStart = millis();
                cardDone = (rhOpenCount() == 0);
                rhUpdateWaypoint();
                cursor = 0; rhMoveCursor();
                scr = SC_PLAY;
                break;
            case MA_SOLO:
                party = false;
                tripMins = 0;
                rhNewRun();
                rhUpdateWaypoint();
                scr = SC_PLAY;
                break;
            case MA_PARTY:
                party = true;
                if (rhNetBegin()) { scr = SC_LOBBY; lastHello = 0; }
                else party = false;
                break;
            case MA_TROPHY: scr = SC_TROPHY; trophySel = 0; break;
            case MA_BACK:   scr = SC_TITLE; break;
            }
        }
        break;

    case SC_LOBBY:
        if (now - lastHello > RH_HELLO_MS) { lastHello = now; rhSend(RM_HELLO, RH_BCAST); }
        if (rhIsHost()) {
            if (lTap) tripOpt = (tripOpt + 1) % NUM_TRIP_OPTS;
            if (rTap) {
                tripMins = TRIP_MINS[tripOpt];
                rhSend(RM_START, RH_BCAST);
                delay(5);
                rhSend(RM_START, RH_BCAST);          // cheap redundancy
                rhNewRun();
                scr = SC_PLAY;
            }
        } else {
            if (lTap) { rhNetEnd(); party = false; scr = SC_MENU; }
        }
        break;

    case SC_PLAY: {
        if (lTap)      rhMoveCursor();
        if (rTap)      rhClaim();
        if (lHoldFire) rhSkip();
        if (rHoldFire) { scr = SC_STANDINGS; }

        if (party && now - lastStatus > RH_STATUS_MS) {
            lastStatus = now;
            rhSend(RM_STATUS, RH_BCAST);
        }
        if (cardDone)                       rhEndTrip(true);
        else if (tripMins > 0 && rhRemainSec() == 0) rhEndTrip(false);
        break;
    }

    case SC_CLAIM:
        animT++;
        if (animT > 78 || lTap || rTap) {            // ~1.25s at 62fps
            animT = 0;
            if (wpAdvanced) { wpAdvanced = false; scrReturn = SC_PLAY; scr = SC_MAP; }
            else            { scr = SC_PLAY; }
        }
        break;

    case SC_TAUNT:
        animT++;
        if (animT > 96 || lTap || rTap) { scr = scrReturn; animT = 0; }
        break;

    case SC_MAP:
        animT++;
        if (animT > 150 || lTap || rTap) { animT = 0; scr = scrReturn; }
        break;

    case SC_STANDINGS:
        if (party && now - lastStatus > RH_STATUS_MS) {
            lastStatus = now;
            rhSend(RM_STATUS, RH_BCAST);
        }
        if (rTap) scr = SC_PLAY;
        if (lHoldFire && party && rhIsHost()) {      // HOST: END TRIP
            rhSend(RM_END, RH_BCAST);
            delay(5);
            rhSend(RM_END, RH_BCAST);
            rhEndTrip(false);
        } else if (lTap && !(party && rhIsHost())) {
            scr = SC_PLAY;
        }
        break;

    case SC_RESULTS:
        if (lTap || rTap) {
            if (party) { rhNetEnd(); party = false; }
            newTrophy = -1;
            rhBuildMenu();
            scr = SC_MENU;
            menuSel = 0;
        }
        break;

    case SC_TROPHY:
        if (lTap) trophySel = (trophySel + 1) % NUM_TROPHIES;
        if (rTap) { rhBuildMenu(); scr = SC_MENU; menuSel = menuCount - 3; }
        break;
    }

}

void roadhuntDraw() {
    switch (scr) {
    case SC_TITLE:     drawTitle();     break;
    case SC_MENU:      drawMenu();      break;
    case SC_LOBBY:     drawLobby();     break;
    case SC_PLAY:      drawPlay();      break;
    case SC_CLAIM:     drawClaim();     break;
    case SC_TAUNT:     drawTaunt();     break;
    case SC_MAP:       drawMap();       break;
    case SC_STANDINGS: drawStandings(); break;
    case SC_RESULTS:   drawResults();   break;
    case SC_TROPHY:    drawTrophies();  break;
    }
    fb.pushSprite(0, 0);
}
