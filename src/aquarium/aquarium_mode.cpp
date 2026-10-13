// aquarium_mode.cpp - Living aquarium sim with unlockables + game loop.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <math.h>
#include "aquarium_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"
#include "../sysclock/sysclock.h"
#include "../backlight/backlight.h"
#include <FS.h>
#include <LittleFS.h>

// ---- Background image system (.aqbg paletted RLE) --------------------------
// Backgrounds are tiny palette+RLE files made by the installer's Backgrounds
// tab. We keep only the palette in RAM (no big buffer - no PSRAM) and stream
// the RLE straight to the framebuffer at draw time. Palette slots 0 and 1 are
// the "glow" colors the black light illuminates.
static const int AQBG_W = 170, AQBG_H = 300;
static uint16_t aqBgPalette[16];
static uint8_t  aqBgPalCount = 0;
static uint8_t  aqBgGlowCount = 0;        // how many leading slots are glow colors
static uint8_t* aqBgRle = nullptr;        // heap-held RLE stream (freed on shutdown)
static int      aqBgRleLen = 0;
static int      aqBgCurrent = -1;         // which background index is loaded (-1 none)
static bool     aqBlackLight = false;     // black light on/off (toggle, when unlocked)

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

static const char* NVS_AQ = "aquarium";

// Water column occupies most of the screen below the header.
static const int WATER_TOP = 20;
static const int WATER_BOT = H - 16;

// ---- v2 sprite accent colors ------------------------------------------------
// How big fish are DRAWN, relative to their species `size`. This is a pure
// presentation knob: growth per pellet, sell prices, and the shop all still
// use the raw size, so nudging this never touches the economy. 1.00 = the
// original scale; 1.15 = 15% chunkier. Anything past ~1.4 starts to crowd the
// 170px tank once several fish are fattened.
static const float AQ_FISH_SCALE = 1.15f;

// The theme palette stays the base for everything; these are the handful of
// "real world" colors the redesigned fish and decor need (koi orange, treasure
// gold, wood browns, sand). Kept to a short list so the look stays cohesive.
static const uint16_t AQ_ORANGE     = rgb565(255, 120, 30);
static const uint16_t AQ_GOLD       = rgb565(255, 200, 60);
static const uint16_t AQ_RED        = rgb565(255, 70, 60);
static const uint16_t AQ_BROWN      = rgb565(110, 70, 35);
static const uint16_t AQ_BROWN_DARK = rgb565(70, 45, 22);
static const uint16_t AQ_SAND       = rgb565(126, 108, 72);
static const uint16_t AQ_SAND_HI    = rgb565(170, 150, 104);
static const uint16_t AQ_WHITE      = rgb565(230, 240, 240);
// Built-in "Default" water: 7-band depth gradient, light at the surface
// fading to near-black at the sand, plus a faint god-ray color.
static const uint16_t AQ_GRAD[7] = {
    rgb565(0, 74, 74), rgb565(0, 62, 64), rgb565(0, 50, 54), rgb565(0, 38, 44),
    rgb565(0, 27, 34), rgb565(0, 17, 24), rgb565(0, 9, 15)
};
static const uint16_t AQ_RAY = rgb565(0, 92, 92);

// ---- Species (unlockable) --------------------------------------------------
struct Species {
    const char* name;
    uint16_t bodyCol;
    uint16_t finCol;
    int      cost;       // coins to unlock; 0 = owned from start
    uint8_t  size;       // base body length in px
    uint8_t  baseSell;   // sell value scale: price = baseSell * grow^2 / 2
    bool     questOnly;  // true = only obtainable via quest rewards (not in shop)
    bool     neon;       // true = glows under the black light
};
// ── Economy ──────────────────────────────────────────────────────────────────
// Selling scales with BREED and SIZE: price = baseSell * grow^2 / 2, so a
// fresh fish (grow 1.0) sells for half its base value and a fully-fattened
// one (grow 2.0) for double. Numbers are tuned so the guppy farm works: a
// fresh guppy sells for 4c (below the 5c repurchase - no arbitrage), a maxed
// one for 18c, and fattening costs ~8c of pellets - a real profit ladder that
// funds Neon (15c) then Koi (40c). Premium breeds sell for far more but are
// harder to farm: unlock capital, slower growth (bigger fish need more feeds,
// see the size-scaled growth on eat), and they compete for the same pellets.
static const Species SPECIES[] = {
    { "Guppy",    TH_TEAL_LIGHT,   TH_TEAL_BRIGHT,  0,   10, 8,   false, false },
    { "Neon",     TH_LIME,         TH_LIME_PALE,    15,  9,  14,  false, true  },
    { "Koi",      TH_PURPLE_LIGHT, TH_LIME_PALE,    40,  14, 24,  false, false },
    { "Angel",    TH_LIME_PALE,    TH_TEAL_LIGHT,   80,  13, 36,  false, false },
    { "Mantis",   TH_LIME,         TH_PURPLE_LIGHT, 150, 12, 55,  false, true  },  // mascot fish-shrimp
    // --- quest-reward exclusive (never appear in the shop) ---
    { "Lantern",  TH_PURPLE_LIGHT, TH_LIME_PALE,    0,   11, 60,  true,  true  },  // deep-sea glow
    { "Phantom",  TH_TEAL_LIGHT,   TH_PURPLE_LIGHT, 0,   13, 75,  true,  true  },  // translucent rare
    { "Dragon",   TH_LIME_PALE,    TH_PURPLE_LIGHT, 0,   16, 100, true,  false },  // big showpiece
    // --- live-catch exclusives from Mantis Fishing (never in the shop) ---
    { "Trout",    TH_TEAL_BRIGHT,  TH_PURPLE_LIGHT, 0,   13,  70, true,  false },  // came in alive
    { "Salmon",   TH_PURPLE_LIGHT, TH_LIME_PALE,    0,   14,  80, true,  false },  // came in alive
};

struct Fish;                              // defined below
static int aqSellPrice(const Fish& f);    // fwd (breed+size based)
static const int SPECIES_COUNT = sizeof(SPECIES)/sizeof(SPECIES[0]);
static const int SHOP_SPECIES_COUNT = 5;   // first N species are shop-buyable; rest quest-only

// ---- Decorations (unlockable) ----------------------------------------------
struct Decor { const char* name; int cost; bool questOnly; bool neon; };
static const Decor DECOR[] = {
    { "Plant",     20,  false, false },
    { "Castle",    60,  false, false },
    { "Coral",     100, false, true  },   // coral glows under black light
    { "Chest",     35,  false, true  },   // treasure chest bubbler (extra bubbles!)
    // --- quest-reward exclusive ---
    { "Shipwreck", 0,   true,  false },
    { "Geode",     0,   true,  true  },   // glowing crystal cluster
};
static const int DECOR_COUNT = sizeof(DECOR)/sizeof(DECOR[0]);
static const int SHOP_DECOR_COUNT = 4;     // first N decor are shop-buyable; rest quest-only

// v2 decor floor layout, left to right so nothing overlaps on the 170px floor:
// Plant ~8..26, Shipwreck 26..76, Coral 76..101, Chest 104..120,
// Castle 127..152, Geode 148..168. The chest is also the bubble source.
static const int AQ_CHEST_X = 112;

// ---- Backgrounds (discovered at runtime from a manifest the installer writes) --
// The installer's Backgrounds tab writes each image as <stem>.aqbg AND appends a
// line to /backgrounds.idx on the content partition:
//     stem|Display Name|category|cost
// where category is "shop" or "quest". The firmware loads that manifest at boot,
// so adding a background never requires touching this source file. Index 0 is
// always the built-in "None" (plain water).
struct BgDef {
    char    name[16];
    char    file[16];
    int     cost;
    bool    questOnly;
};
static const int BG_MAX = 16;          // hard cap (fits a uint16 unlock mask)
static BgDef   BACKGROUNDS[BG_MAX];
static int     BG_COUNT = 1;           // at least the "None" entry
static int     SHOP_BG_COUNT = 1;      // how many leading entries are shop-buyable

// Parse /backgrounds.idx into the BACKGROUNDS catalog. Shop entries are listed
// Mount the "content" LittleFS partition once, exactly like content_reader and
// cyoa do (same label, same guard). Files live under /content/ on this
// partition (e.g. /content/reef.aqbg), matching the .pak files.
static bool aqFsReady = false;
static bool aqFsMount() {
    if (!aqFsReady) aqFsReady = LittleFS.begin(true, "/littlefs", 10, "content");
    return aqFsReady;
}

// first, then quest entries, so indices stay grouped (matches the unlock masks).
static void aqLoadBgCatalog() {
    // entry 0 = Default (plain water), always present and selectable in the shop
    strncpy(BACKGROUNDS[0].name, "Default", sizeof(BACKGROUNDS[0].name)-1);
    BACKGROUNDS[0].name[sizeof(BACKGROUNDS[0].name)-1] = '\0';
    BACKGROUNDS[0].file[0] = '\0';
    BACKGROUNDS[0].cost = 0; BACKGROUNDS[0].questOnly = false;
    BG_COUNT = 1; SHOP_BG_COUNT = 1;

    if (!aqFsMount()) return;
    fs::File f = LittleFS.open("/content/backgrounds.idx", "r");
    if (!f) return;

    // First pass: collect shop entries, then quest entries (two passes keeps
    // the catalog grouped without a sort).
    for (int pass = 0; pass < 2; pass++) {
        f.seek(0);
        while (f.available() && BG_COUNT < BG_MAX) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            // split on '|'
            int p1 = line.indexOf('|');
            int p2 = line.indexOf('|', p1+1);
            int p3 = line.indexOf('|', p2+1);
            if (p1 < 0 || p2 < 0 || p3 < 0) continue;
            String stem = line.substring(0, p1);
            String disp = line.substring(p1+1, p2);
            String cat  = line.substring(p2+1, p3);
            int    cost = line.substring(p3+1).toInt();
            bool   quest = cat.equalsIgnoreCase("quest");
            if ((pass == 0) == quest) continue;   // pass 0 = shop, pass 1 = quest

            BgDef& b = BACKGROUNDS[BG_COUNT];
            strncpy(b.file, stem.c_str(), sizeof(b.file)-1); b.file[sizeof(b.file)-1]='\0';
            strncpy(b.name, disp.c_str(), sizeof(b.name)-1); b.name[sizeof(b.name)-1]='\0';
            b.cost = cost; b.questOnly = quest;
            BG_COUNT++;
            if (!quest) SHOP_BG_COUNT++;
        }
    }
    f.close();
}

// ---- Fish instances --------------------------------------------------------
struct Fish {
    float x, y, vx, vy;
    uint8_t species;
    uint8_t nameIdx;  // index into FISH_NAMES (goofy nickname)
    float grow;       // 1.0..2.0 growth multiplier
    bool active;
    uint32_t lastEat;
};
static const int MAX_FISH = 12;
static Fish fish[MAX_FISH];

// ---- Goofy fish nicknames (assigned randomly when a fish is spawned) -------
static const char* FISH_NAMES[] = {
    "Bubbles", "Finly", "SeaLeg", "Little Sushi", "Sir Finsworth", "Gilbert", "Noodle", "Sushi", "Captain Glub",
    "Mr. Splashy", "Wanda", "Finnegan", "Chairman Meow", "Bloop", "Tako",
    "Lord Scales", "Pickles", "Goober", "Sashimi", "Admiral Bubbles", "Zippy",
    "McFishface", "Sir Bubbles", "Waddles", "Munchkin", "Torpedo", "Gizmo",
    "Professor Gill", "Squirt", "Doplhin", "Mister Wiggles", "Nibbles", "Pebble",
    "Glubsworth", "Tiny", "Moby", "Flapjack", "Snorkel", "Biscuit",
};
static const int FISH_NAME_COUNT = sizeof(FISH_NAMES)/sizeof(FISH_NAMES[0]);

// ---- Food pellets ----------------------------------------------------------
struct Pellet { float x, y, vy; bool active; };
static const int MAX_PELLETS = 8;
static Pellet pellets[MAX_PELLETS];

// ---- Persistent player state ----------------------------------------------
static uint32_t aqCoins = 0;
static uint16_t aqPellets = 10;          // fish feed inventory (starts at 10)
static constexpr uint32_t AQ_ENTER_GUARD = 250;  // swallow the R press that entered the app
static uint32_t aqEnterAt = 0;
static char     aqMsg[30] = "";          // transient tank message
static uint32_t aqMsgAt = 0;
static void aqShowMsg(const char* m);    // fwd (defined near dropFood)
static bool      aqFedToday = false;     // fed the fish this session/day?
static uint32_t  aqLastCoinDay = 0;      // epoch-day we last paid the feeding coin
static uint16_t  aqBgUnlocked = 1;       // bitmask of unlocked backgrounds (bit0=None always)
static int       aqBgSelected = 0;       // currently selected background index
static bool      aqBlackLightUnlocked = false;
static void aqCheckAllFish();
static bool aqBgLoad(int bgIndex);
static void aqBgFree();
static inline uint16_t aqGlowTint(uint16_t c);
// Darken a 565 color by an integer divisor per channel (black-light dimming).
static inline uint16_t aqDim(uint16_t c, int d) {
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return (uint16_t)(((r / d) << 11) | ((g / d) << 5) | (b / d));
}
// A dark shadow of a color's neon self - drawn behind glowing sprites as a
// cheap "light spill" aura under the black light.
static inline uint16_t aqHalo(uint16_t c) { return aqDim(aqGlowTint(c), 3); }
// Live-fish inbox shared with Mantis Fishing (namespace "aqlive").
static bool aqTakeLiveFish(const char* key) {
    Preferences p;
    if (!p.begin("aqlive", false)) return false;
    uint8_t n = p.getUChar(key, 0);
    if (n) p.putUChar(key, 0);
    p.end();
    return n != 0;
}

static void aqSpawnFish(uint8_t species, float x, float y);
// Feeding tests against the sprite's drawn mouth (see aqMouthFrac).
static void aqFishMouth(const Fish& f, float* mx, float* my);
// The one place a fish's on-screen size is computed. Drawing, the mouth
// offset, the bite radius, and the wall bounce all go through this, so they
// can never disagree about how big a fish is.
static inline int aqFishSz(uint8_t spIdx, float grow) {
    const Species& s = SPECIES[spIdx < SPECIES_COUNT ? spIdx : 0];
    int sz = (int)(s.size * grow * AQ_FISH_SCALE);
    return sz < 6 ? 6 : sz;
}
static void aqSave();
static uint16_t aqSpeciesUnlocked = 0;  // bitmask
static uint8_t  aqDecorUnlocked = 0;     // bitmask
static uint8_t  aqFishSpecies[MAX_FISH]; // which species each saved fish is
static uint8_t  aqFishNames[MAX_FISH];   // saved name index per fish
static uint8_t  aqFishGrow[MAX_FISH];    // saved growth*100 per fish
static bool     aqHaveSavedNames = false;
static bool     aqHaveSavedGrow = false;
static uint8_t  aqFishCount = 0;

// ---- UI mode ---------------------------------------------------------------
enum AqScreen { AQ_TANK, AQ_SHOP, AQ_SCREENSAVER, AQ_FISHLIST };
static AqScreen aqScreen = AQ_TANK;
static int aqShopSel = 0;        // index into combined shop list
static int aqFishSel = 0;        // selected fish index in the fish-list screen
static uint32_t aqLastTick = 0;
static uint32_t lHoldStart = 0; static bool lHolding = false;

// ---- Both-button (menu gesture) suppression --------------------------------
// The launcher's exit gesture is a double-click of BOTH buttons. The raw
// button globals stay untouched (main.cpp needs them to detect the gesture),
// but the aquarium must never treat the chord as a feed / buy / sell / UV
// toggle. Whenever both buttons are down together we "poison" the presses in
// flight: a poisoned press does nothing on release. A short cooldown after
// the chord ends also poisons the second click of the double-click, so
// neither click of the gesture can leak a game action. Worst case, one
// genuine tap within ~350ms after a chord is swallowed - a fine trade for
// never buying a Koi on your way to the menu.
static bool     aqChordActive = false;   // both buttons currently down
static bool     aqLPoisoned = false;     // current LEFT press is part of a chord
static bool     aqRPoisoned = false;     // current RIGHT press is part of a chord
static uint32_t aqChordCoolUntil = 0;    // presses starting before this are poisoned

static void aqChordUpdate(uint32_t now) {
    // both down together -> chord; poison whatever presses are in flight
    if (btnL && btnR) {
        aqChordActive = true;
        aqLPoisoned = aqRPoisoned = true;
    }
    // chord fully released -> start the cooldown window
    if (aqChordActive && !btnL && !btnR) {
        aqChordActive = false;
        aqChordCoolUntil = now + 350;
    }
    // presses that begin during the cooldown are part of the gesture too
    if (now < aqChordCoolUntil) {
        if (btnL) aqLPoisoned = true;
        if (btnR) aqRPoisoned = true;
    }
    // Un-poison only once a button has been fully up for at least one tick
    // past the cooldown. Checking _prev keeps the release tick itself
    // suppressed: the button handlers run after this and must still see the
    // poison on the frame the button comes up.
    if (!aqChordActive && now >= aqChordCoolUntil) {
        if (!btnL && !btnL_prev) aqLPoisoned = false;
        if (!btnR && !btnR_prev) aqRPoisoned = false;
    }
}

// bubbles for ambiance
struct Bubble { float x, y; uint8_t r; bool active; };
static const int MAX_BUBBLES = 16;   // second half becomes the chest's column
static Bubble bubbles[MAX_BUBBLES];

// ---------------------------------------------------------------------------
static void aqSave() {
    Preferences p;
    if (!p.begin(NVS_AQ, false)) return;
    p.putULong("coins", aqCoins);
    p.putUShort("pellets", aqPellets);
    p.putUShort("spunlock", aqSpeciesUnlocked);
    p.putUChar("dcunlock", aqDecorUnlocked);
    p.putUChar("decorv", 2);    // decor bit layout version (v2 = Chest at bit 3)
    p.putULong("coinday", aqLastCoinDay);
    p.putUShort("bgunlock", aqBgUnlocked);
    p.putInt("bgsel", aqBgSelected);
    p.putBool("blacklt", aqBlackLightUnlocked);
    // store current fish roster: species, name index, and growth
    uint8_t n = 0;
    static uint8_t saveNames[MAX_FISH];
    static uint8_t saveGrow[MAX_FISH];
    for (int i = 0; i < MAX_FISH; i++) if (fish[i].active) {
        aqFishSpecies[n] = fish[i].species;
        saveNames[n] = fish[i].nameIdx;
        saveGrow[n] = (uint8_t)(fish[i].grow * 100.0f);   // 1.00..2.00 -> 100..200
        n++;
    }
    p.putUChar("fcount", n);
    p.putBytes("fish", aqFishSpecies, n);
    p.putBytes("fnames", saveNames, n);
    p.putBytes("fgrow", saveGrow, n);
    p.end();
}

static void aqSpawnFish(uint8_t species, float x, float y) {
    for (int i = 0; i < MAX_FISH; i++) {
        if (!fish[i].active) {
            fish[i].active = true;
            fish[i].species = species;
            fish[i].nameIdx = (uint8_t)(esp_random() % FISH_NAME_COUNT);
            fish[i].x = x; fish[i].y = y;
            fish[i].vx = ((esp_random() % 100) / 100.0f - 0.5f) * 1.2f;
            fish[i].vy = ((esp_random() % 100) / 100.0f - 0.5f) * 0.6f;
            if (fabsf(fish[i].vx) < 0.3f) fish[i].vx = fish[i].vx < 0 ? -0.4f : 0.4f;
            fish[i].grow = 1.0f;
            fish[i].lastEat = millis();
            return;
        }
    }
}

static void aqLoad() {
    Preferences p;
    aqCoins = 0; aqSpeciesUnlocked = 1; aqDecorUnlocked = 0;  // guppy always owned
    aqFishCount = 0;
    if (p.begin(NVS_AQ, true)) {
        aqCoins = p.getULong("coins", 0);
        aqPellets = p.getUShort("pellets", 10);   // existing saves get the starter 10
        aqSpeciesUnlocked = p.getUShort("spunlock", 1);
        aqDecorUnlocked = p.getUChar("dcunlock", 0);
        // v2 decor migration: inserting "Chest" at shop slot 3 pushed the
        // quest decor (Shipwreck, Geode) from bits 3,4 up to bits 4,5. Old
        // saves (version < 2) get their mask remapped exactly once - aqSave
        // then stamps version 2 so this never runs again.
        if (p.getUChar("decorv", 1) < 2) {
            uint8_t old = aqDecorUnlocked;
            aqDecorUnlocked = (uint8_t)((old & 0x07) | ((old & 0x18) << 1));
        }
        aqLastCoinDay = p.getULong("coinday", 0);
        aqBgUnlocked = p.getUShort("bgunlock", 1) | 1;   // bit0 (Default) always owned
        aqBgSelected = p.getInt("bgsel", 0);
        aqBlackLightUnlocked = p.getBool("blacklt", false);
        aqFishCount = p.getUChar("fcount", 0);
        if (aqFishCount > MAX_FISH) aqFishCount = MAX_FISH;
        p.getBytes("fish", aqFishSpecies, aqFishCount);
        // Names/growth may be absent in older saves. Only treat them as valid if
        // the stored blob is the right size; otherwise leave sentinels so the
        // random spawn names are kept (fixes "every fish named Bubbles").
        aqHaveSavedNames = (p.getBytesLength("fnames") == aqFishCount && aqFishCount > 0);
        aqHaveSavedGrow  = (p.getBytesLength("fgrow")  == aqFishCount && aqFishCount > 0);
        if (aqHaveSavedNames) p.getBytes("fnames", aqFishNames, aqFishCount);
        if (aqHaveSavedGrow)  p.getBytes("fgrow",  aqFishGrow,  aqFishCount);
        p.end();
    }
    aqSpeciesUnlocked |= 1;  // ensure guppy

    // populate fish
    for (int i = 0; i < MAX_FISH; i++) fish[i].active = false;
    if (aqFishCount == 0) {
        // first run: two starter guppies
        aqSpawnFish(0, W * 0.4f, H * 0.4f);
        aqSpawnFish(0, W * 0.6f, H * 0.6f);
    } else {
        for (int i = 0; i < aqFishCount; i++) {
            uint8_t sp = aqFishSpecies[i] < SPECIES_COUNT ? aqFishSpecies[i] : 0;
            aqSpawnFish(sp, (esp_random()%(W-30))+15, (esp_random()%(WATER_BOT-WATER_TOP-20))+WATER_TOP+10);
        }
        // re-apply saved attributes in order to the active fish (only when the
        // saved data is valid; random spawn names/growth stay otherwise)
        int idx = 0;
        for (int i = 0; i < MAX_FISH && idx < aqFishCount; i++) {
            if (fish[i].active) {
                if (aqHaveSavedNames && aqFishNames[idx] < FISH_NAME_COUNT)
                    fish[i].nameIdx = aqFishNames[idx];
                if (aqHaveSavedGrow && aqFishGrow[idx] >= 100)
                    fish[i].grow = aqFishGrow[idx] / 100.0f;
                idx++;
            }
        }
    }
    // init bubbles
    for (int i = 0; i < MAX_BUBBLES; i++) {
        bubbles[i].active = true;
        bubbles[i].x = esp_random() % W;
        bubbles[i].y = WATER_TOP + esp_random() % (WATER_BOT - WATER_TOP);
        bubbles[i].r = 1 + esp_random() % 2;
    }
    for (int i = 0; i < MAX_PELLETS; i++) pellets[i].active = false;
}

void aquariumEnter() {
    fb.setRotation(0);
    aqScreen = AQ_TANK;
    aqShopSel = 0;
    aqLoad();
    // Discover available backgrounds BEFORE processing quest rewards - the
    // decor reward below unlocks quest-only backgrounds, which only exist
    // once the catalog is loaded. (Previously this ran after, so on a fresh
    // boot the reward was consumed but the background never unlocked.)
    aqLoadBgCatalog();
    aqFedToday = false;
    // NOTE: fish never shrink - growth is permanent once earned (by design).
    // Apply any aquarium items used from the Quest inventory.
    uint16_t feed = qpTakePending(ITEM_AQ_FEED);
    if (feed) { aqPellets = (uint16_t)min(999, (int)aqPellets + 10*(int)feed);
        aqShowMsg("Quest food: +pellets!"); }
    uint8_t packs = qpTakeAqCoinPacks();
    if (packs) { aqCoins += (uint32_t)packs * 50; }      // Coin Pack = +50 each
    uint8_t rare = qpTakeAqRareFish();
    if (rare) {
        // Prefer unlocking a quest-exclusive species first (the whole point of
        // a rare-fish reward); fall back to any locked species otherwise.
        int pick = -1;
        for (int i = 0; i < SPECIES_COUNT; i++)
            if (SPECIES[i].questOnly && !(aqSpeciesUnlocked & (1<<i))) { pick = i; break; }
        if (pick < 0)
            for (int i = 0; i < SPECIES_COUNT; i++)
                if (!(aqSpeciesUnlocked & (1<<i))) { pick = i; break; }
        if (pick >= 0) { aqSpeciesUnlocked |= (1<<pick); aqSpawnFish(pick, W/2, H/2); }
    }

    // Live catches from Mantis Fishing: the exact species the angler landed,
    // swimming home. Hidden from the shop until this moment.
    // Live catches from Mantis Fishing arrive on their own tiny NVS inbox
    // ("aqlive"), NOT through the quest profile - adding item ids there would
    // resize the saved profile blob.
    if (aqTakeLiveFish("trout")) {
        int t = SPECIES_COUNT - 2;               // "Trout"
        aqSpeciesUnlocked |= (uint16_t)(1 << t);
        aqSpawnFish((uint8_t)t, W/2, H/2);
    }
    if (aqTakeLiveFish("salmon")) {
        int t = SPECIES_COUNT - 1;               // "Salmon"
        aqSpeciesUnlocked |= (uint16_t)(1 << t);
        aqSpawnFish((uint8_t)t, W/2, H/2);
    }
    uint8_t decor = qpTakeAqDecor();
    if (decor) {
        int pick = -1;
        for (int i = 0; i < DECOR_COUNT; i++)
            if (DECOR[i].questOnly && !(aqDecorUnlocked & (1<<i))) { pick = i; break; }
        if (pick < 0)
            for (int i = 0; i < DECOR_COUNT; i++)
                if (!(aqDecorUnlocked & (1<<i))) { pick = i; break; }
        if (pick >= 0) aqDecorUnlocked |= (1<<pick);
        // a decor reward also unlocks the next quest-only background, if any
        for (int b = SHOP_BG_COUNT; b < BG_COUNT; b++)
            if (BACKGROUNDS[b].questOnly && !(aqBgUnlocked & (1<<b))) { aqBgUnlocked |= (1<<b); break; }
    }
    if (packs || rare || decor) { aqCheckAllFish(); aqSave(); }
    // Load the selected background image (if any) into the heap RLE buffer.
    if (aqBgSelected > 0 && aqBgSelected < BG_COUNT && (aqBgUnlocked & (1 << aqBgSelected))) {
        aqBgLoad(aqBgSelected);
    }
    aqLastTick = millis();
    aqEnterAt  = millis();   // the menu's R-select press must not feed a fish
}
void aquariumShutdown() {
    aqSave();
    aqBgFree();   // release the heap background buffer
}

static void aqShowMsg(const char* m) {
    strncpy(aqMsg, m, sizeof(aqMsg)-1); aqMsg[sizeof(aqMsg)-1]='\0';
    aqMsgAt = millis();
}

// drop food (consumes one pellet from the feed inventory)
static void dropFood() {
    if (aqPellets == 0) { aqShowMsg("Out of pellets! (shop)"); return; }
    for (int i = 0; i < MAX_PELLETS; i++) {
        if (!pellets[i].active) {
            aqPellets--;
            pellets[i].active = true;
            pellets[i].x = 20 + esp_random() % (W - 40);
            pellets[i].y = WATER_TOP + 4;
            pellets[i].vy = 0.4f + (esp_random() % 30) / 100.0f;
            return;
        }
    }
}

// ---- shop actions ----------------------------------------------------------
// combined shop list: species first, then decor, then "add fish" actions
static bool aqAllFishBanner = false;   // show a one-time "story unlocked" banner

// Mask of the species that count toward the "all fish" achievement. Only the
// shop-obtainable species count - the quest-only rares are a bonus, not a
// requirement (otherwise the story would be gated behind rare RNG drops).
static uint16_t aqAllSpeciesMask() {
    uint16_t m = 0;
    for (int i = 0; i < SHOP_SPECIES_COUNT; i++) m |= (1 << i);
    return m;
}

// Call after any species unlock: if the player now owns every fish, unlock the
// aquarium-gated CYOA story (once).
static void aqCheckAllFish() {
    if ((aqSpeciesUnlocked & aqAllSpeciesMask()) == aqAllSpeciesMask()) {
        if (!qpStoryUnlocked(STORY_TIDE)) {
            qpUnlockStory(STORY_TIDE);
            aqAllFishBanner = true;
        }
        // collecting every shop fish also unlocks the black light
        aqBlackLightUnlocked = true;
    }
}

static void shopBuy() {
    // shop shows only the first SHOP_SPECIES_COUNT species + SHOP_DECOR_COUNT
    // decor; quest-only items are never indexable here. Last row = fish food.
    // The background section INCLUDES index 0 ("Default") so players can always
    // switch back to the built-in water after buying other backgrounds.
    int total = SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT + SHOP_BG_COUNT + 1;
    if (aqShopSel == total - 1) {
        // 5 pellets for 2 coins, repeatable.
        if (aqCoins >= 2 && aqPellets <= 994) {
            aqCoins -= 2; aqPellets += 5; aqSave();
        }
        return;
    }
    if (aqShopSel < SHOP_SPECIES_COUNT) {
        int s = aqShopSel;
        if (!(aqSpeciesUnlocked & (1 << s)) && aqCoins >= (uint32_t)SPECIES[s].cost) {
            aqCoins -= SPECIES[s].cost;
            aqSpeciesUnlocked |= (1 << s);
            aqSpawnFish(s, W/2, H/2);  // gift one
            qpGrantPetXp(20);          // milestone: a new species feeds the mantis
            aqCheckAllFish();
            aqSave();
        } else if (aqSpeciesUnlocked & (1 << s)) {
            // already unlocked -> buy another fish of this species for 5 coins
            if (aqCoins >= 5) { aqCoins -= 5; aqSpawnFish(s, W/2, H/2); aqSave(); }
        }
    } else if (aqShopSel < SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT) {
        int d = aqShopSel - SHOP_SPECIES_COUNT;
        if (d >= 0 && d < SHOP_DECOR_COUNT) {
            if (!(aqDecorUnlocked & (1 << d)) && aqCoins >= (uint32_t)DECOR[d].cost) {
                aqCoins -= DECOR[d].cost;
                aqDecorUnlocked |= (1 << d);
                aqSave();
            }
        }
    } else {
        // Backgrounds: shop entries 0..SHOP_BG_COUNT-1. Index 0 = Default,
        // free and always owned - selecting it unloads any image background.
        int b = aqShopSel - SHOP_SPECIES_COUNT - SHOP_DECOR_COUNT;
        if (b == 0) {
            aqBgSelected = 0;
            aqBgLoad(0);      // frees the image; drawTank falls back to plain water
            aqSave();
        } else if (b >= 1 && b < SHOP_BG_COUNT) {
            if (!(aqBgUnlocked & (1 << b)) && aqCoins >= (uint32_t)BACKGROUNDS[b].cost) {
                aqCoins -= BACKGROUNDS[b].cost;
                aqBgUnlocked |= (1 << b);
                aqBgSelected = b; aqBgLoad(b);
                aqSave();
            } else if (aqBgUnlocked & (1 << b)) {
                // already owned -> select it (apply as the tank background)
                aqBgSelected = b; aqBgLoad(b);
                aqSave();
            }
        }
    }
}

// Sell price by BREED and SIZE (see the economy note at the species table).
static int aqSellPrice(const Fish& f) {
    const Species& sp = SPECIES[f.species < SPECIES_COUNT ? f.species : 0];
    float g = f.grow; if (g < 1.0f) g = 1.0f; if (g > 2.0f) g = 2.0f;
    int price = (int)((float)sp.baseSell * g * g / 2.0f + 0.5f);
    return price < 1 ? 1 : price;
}

// Sell the currently-selected fish (in the fish-list screen) for its
// breed+size price. Never let the tank go empty - keep at least one fish.
static void sellSelectedFish() {
    int activeCount = 0;
    for (int i = 0; i < MAX_FISH; i++) if (fish[i].active) activeCount++;
    if (activeCount <= 1) return;   // keep at least one fish
    // find the aqFishSel-th active fish
    int idx = 0, target = -1;
    for (int i = 0; i < MAX_FISH; i++) {
        if (fish[i].active) {
            if (idx == aqFishSel) { target = i; break; }
            idx++;
        }
    }
    if (target < 0) return;
    aqCoins += (uint32_t)aqSellPrice(fish[target]);
    fish[target].active = false;
    // keep selection in range
    if (aqFishSel >= activeCount - 1) aqFishSel = activeCount - 2;
    if (aqFishSel < 0) aqFishSel = 0;
    aqSave();
}

void aquariumTick() {
    uint32_t now = millis();
    aqChordUpdate(now);   // menu-gesture chord detection runs before anything acts
    bool lEdge = btnL && !btnL_prev;
    bool rEdge = btnR && !btnR_prev;

    // Swallow the button press that selected AQUARIUM from the menu. Without
    // this the R-select edge carries into the first tick and immediately feeds
    // a pellet. Matches roadhunt's RH_ENTER_GUARD convention.
    if (now - aqEnterAt < AQ_ENTER_GUARD) { lEdge = rEdge = false; }

    // dismiss the one-time unlock banner on any tap (chord included - dismissing
    // a banner is harmless, so no poison check here)
    if (aqAllFishBanner && (lEdge || rEdge)) { aqAllFishBanner = false; return; }

    // Feeding earns ONE coin per real day (not per feed). Bigger coin income
    // comes from quest rewards (coin packs / completing tasks).
    if (aqFedToday && sysclockValid()) {
        uint32_t today = (uint32_t)sysclockEpochDays();
        if (today != aqLastCoinDay) {
            aqCoins += 1;
            aqLastCoinDay = today;
            aqSave();
        }
        aqFedToday = false;
    }

    // LEFT hold detection: short tap = navigate/scroll, hold = change screen.
    if (btnL && !lHolding) { lHolding = true; lHoldStart = now; }
    if (!btnL && lHolding) {
        lHolding = false;
        if (!aqLPoisoned) {   // a poisoned press = part of the menu chord: ignore
            bool wasHold = (now - lHoldStart) >= 600;
            if (aqScreen == AQ_TANK) {
                if (wasHold) aqScreen = AQ_SCREENSAVER;   // hold -> zen
                else         aqScreen = AQ_SHOP;          // tap  -> shop
            } else if (aqScreen == AQ_SCREENSAVER) {
                aqScreen = AQ_TANK;                       // any release exits saver
            } else if (aqScreen == AQ_SHOP) {
                if (wasHold) aqScreen = AQ_FISHLIST;      // hold -> fish list
                else aqShopSel = (aqShopSel + 1) % (SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT + SHOP_BG_COUNT + 1); // tap -> next shop item (incl. Default bg + pellets)
            } else if (aqScreen == AQ_FISHLIST) {
                if (wasHold) { aqScreen = AQ_TANK; aqSave(); }   // hold -> back to tank
                else {                                            // tap -> next fish
                    int n = 0; for (int i=0;i<MAX_FISH;i++) if (fish[i].active) n++;
                    if (n > 0) aqFishSel = (aqFishSel + 1) % n;
                }
            }
        }
    }

    if (aqScreen == AQ_TANK) {
        // tap R = feed; hold R = toggle black light (once unlocked)
        static uint32_t tankRHold = 0; static bool tankRHolding = false;
        if (btnR && !tankRHolding) { tankRHolding = true; tankRHold = now; }
        if (!btnR && tankRHolding) {
            tankRHolding = false;
            if (!aqRPoisoned) {
                if (now - tankRHold >= 600) {
                    if (aqBlackLightUnlocked) aqBlackLight = !aqBlackLight;
                } else {
                    dropFood();
                }
            }
        }
    } else if (aqScreen == AQ_SHOP) {
        // Buy on RELEASE, not press: on a press-edge the chord isn't
        // detectable yet (R can land a few ms before L), which is exactly how
        // the menu gesture used to buy random shop items on the way out.
        static bool shopRHolding = false;
        if (btnR && !shopRHolding) shopRHolding = true;
        if (!btnR && shopRHolding) {
            shopRHolding = false;
            if (!aqRPoisoned) shopBuy();
        }
        // (LEFT handled above via hold/tap)
    } else if (aqScreen == AQ_SCREENSAVER) {
        // Zen mode: R cycles backlight brightness (off/dim/mid/bright).
        // Release-based + poison-checked for the same chord reason as the shop.
        // LEFT (handled above) exits back to the tank.
        static bool zenRHolding = false;
        if (btnR && !zenRHolding) zenRHolding = true;
        if (!btnR && zenRHolding) {
            zenRHolding = false;
            if (!aqRPoisoned) backlightCycle();
        }
    } else if (aqScreen == AQ_FISHLIST) {
        // Hold R to sell the selected fish for its breed+size price.
        static uint32_t rHoldStart = 0; static bool rHolding = false;
        if (btnR && !rHolding) { rHolding = true; rHoldStart = now; }
        if (!btnR && rHolding) {
            rHolding = false;
            if (!aqRPoisoned && now - rHoldStart >= 700) sellSelectedFish();
        }
    }

    // physics tick at ~30fps regardless of screen (so the tank keeps living)
    if (now - aqLastTick < 33) return;
    aqLastTick = now;

    // pellets sink
    for (int i = 0; i < MAX_PELLETS; i++) {
        if (!pellets[i].active) continue;
        pellets[i].y += pellets[i].vy;
        if (pellets[i].y > WATER_BOT - 2) pellets[i].active = false;
    }

    // fish swim + seek food
    for (int i = 0; i < MAX_FISH; i++) {
        Fish& f = fish[i];
        if (!f.active) continue;

        // Find the nearest pellet, measured from the MOUTH (where the art is),
        // not the body origin - see aqMouthFrac.
        float mx, my; aqFishMouth(f, &mx, &my);
        int target = -1; float bd = 1e9;
        for (int j = 0; j < MAX_PELLETS; j++) {
            if (!pellets[j].active) continue;
            float dx = pellets[j].x - mx, dy = pellets[j].y - my;
            float d = dx*dx + dy*dy;
            if (d < bd) { bd = d; target = j; }
        }
        if (target >= 0 && bd < 70*70) {
            // steer toward pellet
            float dx = pellets[target].x - mx, dy = pellets[target].y - my;
            float d = sqrtf(dx*dx + dy*dy) + 0.001f;
            f.vx += (dx/d) * 0.08f;
            f.vy += (dy/d) * 0.08f;
            // Bite radius scales with the drawn sprite: a fattened Dragon has a
            // much bigger head than a fresh Guppy, and a fixed 6px window made
            // big fish nose straight past the food.
            const Species& esp = SPECIES[f.species < SPECIES_COUNT ? f.species : 0];
            int szNow = aqFishSz(f.species, f.grow);
            float bite = 4.0f + szNow * 0.35f;
            if (bd < bite * bite) {  // eat
                pellets[target].active = false;
                // Bigger breeds fatten slower - farming premium fish takes
                // more pellets (part of the "harder and more expensive" deal).
                f.grow = fminf(2.0f, f.grow + 0.05f * (10.0f / (float)esp.size));
                f.lastEat = now;
                aqFedToday = true;   // feeding earns 1 coin/day (granted on entry)
            }
        } else {
            // gentle wander
            f.vx += ((esp_random()%100)/100.0f - 0.5f) * 0.04f;
            f.vy += ((esp_random()%100)/100.0f - 0.5f) * 0.03f;
        }

        // (No idle shrink: growth is permanent. f.lastEat is kept as
        // bookkeeping for future features like breeding.)

        // clamp speed
        float sp = sqrtf(f.vx*f.vx + f.vy*f.vy);
        float maxv = 1.4f;
        if (sp > maxv) { f.vx = f.vx/sp*maxv; f.vy = f.vy/sp*maxv; }

        f.x += f.vx; f.y += f.vy;

        // bounce off walls
        int sz = aqFishSz(f.species, f.grow) / 2;   // half-length keeps big fish off the glass
        if (f.x < 6 + sz)      { f.x = 6 + sz;      f.vx =  fabsf(f.vx); }
        if (f.x > W - 6 - sz)  { f.x = W - 6 - sz;  f.vx = -fabsf(f.vx); }
        if (f.y < WATER_TOP+6) { f.y = WATER_TOP+6; f.vy =  fabsf(f.vy); }
        if (f.y > WATER_BOT-6) { f.y = WATER_BOT-6; f.vy = -fabsf(f.vy); }
    }

    // bubbles rise. When the treasure chest is owned (decor bit 3), the second
    // half of the bubble pool streams from the chest mouth as a lively column.
    bool chestOwned = (aqDecorUnlocked & (1 << 3)) != 0;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        if (!bubbles[i].active) continue;
        bool fromChest = chestOwned && i >= MAX_BUBBLES/2;
        bubbles[i].y -= fromChest ? 0.9f : 0.5f;
        if (fromChest) bubbles[i].x += (float)((int)(esp_random() % 3) - 1) * 0.3f;
        if (bubbles[i].y < WATER_TOP) {
            if (fromChest) {
                bubbles[i].y = WATER_BOT - 16;
                bubbles[i].x = AQ_CHEST_X - 3 + (int)(esp_random() % 7);
            } else {
                bubbles[i].y = WATER_BOT;
                bubbles[i].x = esp_random() % W;
            }
        }
    }
}

// Where each v2 sprite's mouth actually sits, as a fraction of sz forward from
// the fish's logical center. The sprites are drawn with different anchors
// (Dragon builds back from a head at +sz/4, Mantis forward from +sz/3, etc.),
// so feeding has to test against THIS point, not the raw (x,y) - otherwise a
// fish visibly swims its mouth straight through a pellet without eating.
static float aqMouthFrac(uint8_t spIdx) {
    switch (spIdx) {
        case 0: return 0.70f;   // Guppy   - snout past the offset body circle
        case 1: return 0.50f;   // Neon    - eye/nose at +sz/2
        case 2: return 0.70f;   // Koi     - same build as the guppy
        case 3: return 0.50f;   // Angel   - rhombus nose at +sz/2
        case 4: return 0.83f;   // Mantis  - claws reach past the head
        case 5: return 0.67f;   // Lantern - toothy jaw ahead of the head
        case 6: return 0.80f;   // Phantom - outline head circle
        case 8: return 0.72f;   // Trout   - snout at the front body circle
        case 9: return 0.78f;   // Salmon  - kype jaw juts past the head
        default: return 0.58f;  // Dragon  - head at +sz/4 plus whiskers
    }
}
static void aqFishMouth(const Fish& f, float* mx, float* my) {
    int sz = aqFishSz(f.species, f.grow);
    float dir = (f.vx >= 0) ? 1.0f : -1.0f;
    *mx = f.x + dir * sz * aqMouthFrac(f.species);
    *my = f.y;
}


// v2 fish sprites: every breed has its own silhouette so nothing looks like a
// palette-swap (the old renderer drew one shared shape, which made Koi and
// Lantern pixel-identical). `ph` drives a subtle tail wiggle. Under the black
// light, neon breeds snap to their electric selves (aqGlowTint) with a soft
// halo behind them; everything else dims so the glow really pops.
static void drawFishSprite(uint8_t spIdx, int x, int y, int dir, float grow, float ph) {
    const Species& s = SPECIES[spIdx < SPECIES_COUNT ? spIdx : 0];
    int sz = aqFishSz(spIdx, grow);
    bool uv = aqBlackLight;
    int w = (int)lroundf(sinf(ph) * 2.0f);   // tail wiggle offset
    #define FC(c) (uv ? (s.neon ? aqGlowTint(c) : aqDim((c), 3)) : (c))
    if (uv && s.neon) fb.fillCircle(x, y, sz/2 + 2, aqHalo(thc(s.bodyCol)));
    switch (spIdx) {
        case 0: {   // Guppy: round body, oversized two-tone fan tail, dark spots
            uint16_t b = FC(TEAL_LIGHT);
            fb.fillCircle(x, y, sz/2, b);
            fb.fillCircle(x + dir*(sz/3), y, sz/2 - 1, b);
            int tx = x - dir*(sz/2);
            fb.fillTriangle(tx, y, tx - dir*sz, y - (int)(sz*0.6f) + w, tx - dir*(int)(sz*0.45f), y - 1, FC(TEAL_BRIGHT));
            fb.fillTriangle(tx, y, tx - dir*sz, y + (int)(sz*0.6f) + w, tx - dir*(int)(sz*0.45f), y + 1, FC(PURPLE_LIGHT));
            fb.drawPixel(x - dir*2, y - 1, PURPLE_DARK);
            fb.drawPixel(x + dir,   y + 2, PURPLE_DARK);
            fb.drawPixel(x - dir*4, y + 1, PURPLE_DARK);
            fb.fillTriangle(x, y - sz/2, x - dir*(sz/4), y - sz/2 - 3, x + dir*(sz/5), y - sz/2, FC(TEAL_BRIGHT));
            fb.fillCircle(x + dir*(sz/2), y - sz/6, 1, TEAL_DEEP);
            fb.drawPixel(x + dir*(sz/2) - dir, y - sz/6 - 1, AQ_WHITE);
            break;
        }
        case 1: {   // Neon tetra: slim body, electric side stripe, red rear stripe
            uint16_t b = FC(TEAL);
            fb.fillCircle(x - dir*(sz/4), y, sz/3, b);
            fb.fillCircle(x + dir*(sz/4), y, sz/3, b);
            int x0 = x - dir*(int)(sz*0.7f), x1 = x + dir*(sz/2);
            if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
            fb.fillRect(x0, y - 1, x1 - x0, 2, FC(TEAL_BRIGHT));
            int r0 = x - dir*(int)(sz*0.7f), r1 = x;
            if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
            fb.fillRect(r0, y + 1, r1 - r0, 2, FC(AQ_RED));
            int tx = x - dir*(int)(sz*0.8f);
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y - sz/3 + w, tx - dir*(sz/4), y, FC(LIME_PALE));
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y + sz/3 + w, tx - dir*(sz/4), y, FC(LIME_PALE));
            fb.fillCircle(x + dir*(sz/2), y - 1, 1, TEAL_DEEP);
            fb.drawPixel(x + dir*(sz/2) - dir, y - 2, AQ_WHITE);
            break;
        }
        case 2: {   // Koi: whitish body, orange + plum patches, forked orange tail
            uint16_t b = FC(LIME_PALE);
            fb.fillCircle(x, y, sz/2, b);
            fb.fillCircle(x + dir*(sz/3), y, sz/2 - 1, b);
            fb.fillCircle(x + dir*(sz/5), y - sz/6, sz/4, FC(AQ_ORANGE));
            fb.fillCircle(x - dir*(sz/4), y + 1, sz/5, FC(AQ_ORANGE));
            fb.fillCircle(x + dir, y + sz/5, sz/6, FC(PURPLE));
            int tx = x - dir*(int)(sz*0.85f);
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y - sz/3 + w, tx - dir*(sz/5), y, FC(AQ_ORANGE));
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y + sz/3 + w, tx - dir*(sz/5), y, FC(AQ_ORANGE));
            fb.fillTriangle(x, y - sz/2, x - dir*(sz/4), y - sz/2 - sz/5, x + dir*(sz/5), y - sz/2, FC(AQ_ORANGE));
            fb.fillCircle(x + dir*(sz/2), y - sz/6, 1, TEAL_DEEP);
            fb.drawPixel(x + dir*(sz/2 + 2), y + 1, FC(AQ_ORANGE));   // barbel
            break;
        }
        case 3: {   // Angelfish: tall rhombus, vertical bars, trailing streamers
            uint16_t b = FC(LIME_PALE);
            int nx = x + dir*(sz/2), bx = x - dir*(sz/3);
            fb.fillTriangle(nx, y, x, y - (int)(sz*0.7f), bx, y, b);
            fb.fillTriangle(nx, y, x, y + (int)(sz*0.7f), bx, y, b);
            fb.drawLine(x - dir, y - (int)(sz*0.55f), x - dir, y + (int)(sz*0.55f), PURPLE_DARK);
            fb.drawLine(x + dir*(sz/4), y - sz/3, x + dir*(sz/4), y + sz/3, PURPLE_DARK);
            fb.drawLine(x, y - (int)(sz*0.7f), x - dir*(sz/2), y - (int)(sz*0.9f) + w, FC(TEAL_LIGHT));
            fb.drawLine(x, y + (int)(sz*0.7f), x - dir*(sz/2), y + (int)(sz*0.9f) + w, FC(TEAL_LIGHT));
            int tx = bx - dir;
            fb.fillTriangle(tx, y, tx - dir*(sz/3), y - sz/4 + w, tx - dir*(sz/3), y + sz/4 + w, FC(TEAL_LIGHT));
            fb.fillCircle(nx - dir, y - 2, 1, TEAL_DEEP);
            break;
        }
        case 4: {   // Mantis shrimp: banded segments, eyestalks, claws, tail fan
            int hx = x + dir*(sz/3);
            for (int i = 0; i < 4; i++) {
                int cx = hx - dir*(int)(i * sz * 0.28f);
                fb.fillCircle(cx, y, sz/3, FC((i & 1) ? LIME_MED : LIME));
            }
            fb.drawLine(hx + dir, y - sz/3, hx + dir*3, y - sz/2 - 2, FC(LIME_MED));
            fb.drawLine(hx - dir, y - sz/3, hx + dir, y - sz/2 - 3, FC(LIME_MED));
            fb.fillCircle(hx + dir*3, y - sz/2 - 2, 1, FC(PURPLE_LIGHT));
            fb.fillCircle(hx + dir,   y - sz/2 - 3, 1, FC(PURPLE_LIGHT));
            fb.fillTriangle(hx + dir*2, y + 2, hx + dir*(sz/2 + 3), y + 1, hx + dir*(sz/2 + 2), y + sz/3, FC(PURPLE_LIGHT));
            int bx = hx - dir*(int)(sz*0.95f);
            fb.fillTriangle(bx, y, bx - dir*(sz/3), y - sz/4 + w, bx - dir*(sz/4), y, FC(LIME_MED));
            fb.fillTriangle(bx, y, bx - dir*(sz/3), y + sz/4 + w, bx - dir*(sz/4), y, FC(LIME_MED));
            for (int i = 0; i < 3; i++)
                fb.drawLine(hx - dir*(2 + i*3), y + sz/3, hx - dir*(3 + i*3), y + sz/3 + 3, LIME_DARK);
            if (uv) {   // the mascot's signature glow markings (kept from v1)
                uint16_t g1 = aqGlowTint(LIME), g2 = aqGlowTint(PURPLE_LIGHT);
                for (int i = 0; i < 4; i++)
                    fb.drawPixel(hx - dir*(int)(i * sz * 0.28f), y - sz/3, g1);
                fb.drawPixel(hx + dir*(sz/2 + 2), y + sz/3, g2);
                fb.drawCircle(hx + dir*3, y - sz/2 - 2, 2, g2);
            }
            break;
        }
        case 5: {   // Lanternfish (angler): big dark head, teeth, glowing lure
            int hx = x + dir*(sz/6);
            fb.fillCircle(x - dir*(sz/3), y, sz/3, uv ? aqDim(PURPLE_DEEP, 2) : PURPLE_DEEP);
            fb.fillCircle(hx, y, sz/2, uv ? aqDim(PURPLE_DARK, 2) : PURPLE_DARK);
            for (int i = 0; i < 3; i++)
                fb.drawPixel(x - dir*i*3, y + sz/3, FC(LIME_PALE));   // photophores
            for (int i = 0; i < 3; i++)   // needle teeth along the jaw
                fb.fillTriangle(hx + dir*(sz/2 - 1), y + 1 + i*2,
                                hx + dir*(sz/2 - 4), y + 2 + i*2,
                                hx + dir*(sz/2 - 1), y + 3 + i*2, AQ_WHITE);
            fb.drawLine(hx + dir, y - sz/2, hx + dir*(sz/2), y - (int)(sz*0.9f), FC(LIME_PALE));
            int bxx = hx + dir*(sz/2 + 1), byy = y - (int)(sz*0.9f) - 1;
            if (uv) { fb.fillCircle(bxx, byy, 4, aqHalo(LIME_PALE)); fb.fillCircle(bxx, byy, 2, aqGlowTint(LIME_PALE)); }
            else      fb.fillCircle(bxx, byy, 2, LIME_PALE);
            int tx = x - dir*(int)(sz*0.7f);
            fb.fillTriangle(tx, y, tx - dir*(sz/3), y - sz/4 + w, tx - dir*(sz/3), y + sz/4 + w, FC(PURPLE_LIGHT));
            fb.fillCircle(hx + dir*(sz/4), y - sz/5, 1, FC(LIME_PALE));
            break;
        }
        case 6: {   // Phantom: translucent outline ghost with a dithered interior
            uint16_t oc = FC(TEAL_LIGHT);
            fb.drawCircle(x, y, sz/2, oc);
            fb.drawCircle(x + dir*(sz/3), y, sz/2 - 1, oc);
            uint16_t dith = uv ? oc : aqDim(oc, 2);
            for (int dy = -sz/3; dy <= sz/3; dy += 2)
                for (int dx = -sz/3; dx <= sz/3; dx += 3)
                    if (((dx + dy) & 1) == 0) fb.drawPixel(x + dx, y + dy, dith);
            fb.drawLine(x - dir*(sz/2), y, x + dir*(int)(sz*0.7f), y, FC(TEAL_BRIGHT));
            int tx = x - dir*(int)(sz*0.7f);
            fb.drawLine(tx, y, tx - dir*(sz/2), y - sz/3 + w, FC(PURPLE_LIGHT));
            fb.drawLine(tx, y, tx - dir*(sz/2), y + w,        FC(PURPLE_LIGHT));
            fb.drawLine(tx, y, tx - dir*(sz/2), y + sz/3 + w, FC(PURPLE_LIGHT));
            fb.fillCircle(x + dir*(sz/2 - 1), y - 2, 1, FC(TEAL_BRIGHT));
            break;
        }
        case 8: {   // Trout: streamlined silver body, rosy lateral stripe, spots,
                    //        forked tail + adipose fin - matches RAINBOW TROUT sprite
            uint16_t b  = FC(LIME_PALE);       // bright silver flank
            uint16_t bk = FC(TEAL_LIGHT);      // darker olive back
            // torpedo body: two overlapping circles, longer than a panfish
            fb.fillCircle(x, y, sz/2, b);
            fb.fillCircle(x + dir*(sz/3), y, sz/2 - 1, b);
            fb.fillCircle(x - dir*(sz/4), y, sz/2 - 2, b);
            // olive back shading along the top
            for (int i = -sz/2; i <= sz/2; i++)
                fb.drawPixel(x + i, y - sz/3, bk);
            // rosy lateral stripe (the trout's signature) - fades red toward tail
            fb.drawLine(x + dir*(sz/2), y, x - dir*(sz/2), y, FC(PURPLE_LIGHT));
            fb.drawLine(x + dir*(sz/4), y, x - dir*(sz/2), y, FC(AQ_RED));
            // dark spots scattered on the upper flank
            fb.fillCircle(x + dir*(sz/4), y - sz/5, 1, bk);
            fb.fillCircle(x - dir*(sz/6), y - sz/4, 1, bk);
            fb.fillCircle(x - dir*(sz/3), y - sz/6, 1, bk);
            fb.fillCircle(x + dir*(sz/8), y - sz/8, 1, bk);
            // small adipose fin on the back near the tail
            fb.fillTriangle(x - dir*(sz/3), y - sz/2 + 1,
                            x - dir*(sz/2), y - sz/2 - 2 + w,
                            x - dir*(sz/4), y - sz/2 + 1, FC(TEAL_BRIGHT));
            // forked tail
            int tx = x - dir*(int)(sz*0.9f);
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y - sz/3 + w, tx - dir*(sz/6), y, FC(TEAL_BRIGHT));
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y + sz/3 + w, tx - dir*(sz/6), y, FC(TEAL_BRIGHT));
            // pectoral fin + eye
            fb.fillTriangle(x + dir*(sz/6), y + sz/6, x - dir*(sz/8), y + sz/3 + w,
                            x + dir*(sz/4), y + sz/6, FC(PURPLE_LIGHT));
            fb.fillCircle(x + dir*(sz/2 - 1), y - 1, 1, TEAL_DEEP);
            break;
        }
        case 9: {   // Salmon: deeper red body, darker head, hooked kype jaw,
                    //         forked tail - matches KOKANEE SALMON (spawning) sprite
            uint16_t b  = FC(AQ_ORANGE);       // reddish spawning body
            uint16_t hd = FC(TEAL_DARK);       // dark greenish head
            uint16_t bk = FC(AQ_RED);          // deep red back
            // slightly larger torpedo body
            fb.fillCircle(x, y, sz/2, b);
            fb.fillCircle(x + dir*(sz/4), y, sz/2 - 1, b);
            fb.fillCircle(x - dir*(sz/3), y, sz/2 - 1, b);
            // deep-red dorsal shading
            for (int i = -sz/2; i <= sz/3; i++)
                fb.drawPixel(x + i, y - sz/3, bk);
            // dark head (kokanee turn green-headed in spawn)
            fb.fillCircle(x + dir*(sz/2), y, sz/3, hd);
            // hooked kype: a small jaw hook at the snout
            fb.fillTriangle(x + dir*(sz/2 + sz/4), y - 2,
                            x + dir*(sz/2 + sz/3), y + 2,
                            x + dir*(sz/2 + sz/6), y + 2, hd);
            // dorsal + adipose fins
            fb.fillTriangle(x, y - sz/2 + 1, x + dir*(sz/6), y - sz/2 - 3 + w,
                            x - dir*(sz/6), y - sz/2 + 1, bk);
            fb.fillTriangle(x - dir*(sz/3), y - sz/2 + 1,
                            x - dir*(sz*0.42f), y - sz/2 - 1 + w,
                            x - dir*(sz/4), y - sz/2 + 1, bk);
            // forked tail
            int tx = x - dir*(int)(sz*0.95f);
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y - sz/3 + w, tx - dir*(sz/6), y, bk);
            fb.fillTriangle(tx, y, tx - dir*(sz/2), y + sz/3 + w, tx - dir*(sz/6), y, bk);
            // pectoral fin + eye
            fb.fillTriangle(x + dir*(sz/5), y + sz/6, x, y + sz/3 + w,
                            x + dir*(sz/3), y + sz/6, bk);
            fb.fillCircle(x + dir*(sz/2 + 1), y - 1, 1, AQ_WHITE);
            break;
        }
        default: {  // Dragon: serpentine gold body, back spikes, whiskers, horns
            int hx = x + dir*(sz/4);
            for (int i = 4; i >= 0; i--) {
                int cx = hx - dir*(int)(i * sz * 0.22f);
                int cy = y + (int)lroundf(sinf(ph + i * 0.9f) * 2.0f);   // undulation
                fb.fillCircle(cx, cy, sz/4, FC((i & 1) ? AQ_ORANGE : AQ_GOLD));
                fb.fillTriangle(cx - 2, cy - sz/4, cx, cy - sz/4 - 4, cx + 2, cy - sz/4, FC(PURPLE_LIGHT));
            }
            fb.fillCircle(hx, y, sz/3, FC(AQ_GOLD));
            fb.fillTriangle(hx - 2, y - sz/3, hx, y - sz/3 - 5, hx + 2, y - sz/3, FC(PURPLE_LIGHT));
            fb.fillTriangle(hx + dir*3 - 2, y - sz/3 + 1, hx + dir*3, y - sz/3 - 4, hx + dir*3 + 2, y - sz/3 + 1, FC(PURPLE_LIGHT));
            fb.drawLine(hx + dir*(sz/3), y,     hx + dir*(sz/3 + 5), y + 3, FC(AQ_GOLD));
            fb.drawLine(hx + dir*(sz/3), y + 1, hx + dir*(sz/3 + 4), y + 5, FC(AQ_GOLD));
            int bx = hx - dir*(int)(sz*1.05f);
            int by = y + (int)lroundf(sinf(ph + 4.5f) * 2.0f);
            fb.fillTriangle(bx, by, bx - dir*(sz/3), by - sz/4 + w, bx - dir*(sz/3), by + sz/4 + w, FC(PURPLE_LIGHT));
            fb.drawPixel(hx + dir*(sz/4), y - 2, AQ_RED);
            break;
        }
    }
    #undef FC
}

static void drawFish(const Fish& f, int idx) {
    drawFishSprite(f.species, (int)f.x, (int)f.y, f.vx >= 0 ? 1 : -1,
                   f.grow, millis() * 0.006f + idx * 1.3f);
}

static void drawDecor() {
    bool uv = aqBlackLight;
    const int bot = WATER_BOT;
    // Plant (bit 0): kelp cluster with leaf blades and glowing growth tips
    if (aqDecorUnlocked & (1 << 0)) {
        for (int s = 0; s < 3; s++) {
            int x0 = 8 + s*6, h = 28 + s*5, bend = (s - 1) * 4;
            uint16_t st = uv ? aqDim(LIME_DARK, 3) : LIME_DARK;
            uint16_t lf = uv ? aqDim(LIME_MED, 3)  : LIME_MED;
            fb.drawLine(x0, bot, x0 + bend/2, bot - h/2, st);
            fb.drawLine(x0 + bend/2, bot - h/2, x0 + bend, bot - h, st);
            for (int l = 8; l < h; l += 8) {
                int lx = x0 + bend * l / h, ly = bot - l, d = ((l >> 3) & 1) ? 1 : -1;
                fb.fillTriangle(lx, ly, lx + d*5, ly - 2, lx + d*2, ly - 5, lf);
            }
            fb.drawPixel(x0 + bend, bot - h, uv ? aqGlowTint(LIME) : LIME);
        }
    }
    // Castle (bit 1): towers, crenellations, arched door, flag, glowing windows
    if (aqDecorUnlocked & (1 << 1)) {
        int cx = 128;
        uint16_t keep = uv ? aqDim(PURPLE_DARK, 3) : PURPLE_DARK;
        uint16_t twr  = uv ? aqDim(PURPLE, 3)      : PURPLE;
        fb.fillRect(cx+4,  bot-24, 16, 24, keep);
        fb.fillRect(cx,    bot-30, 6, 30, twr);
        fb.fillRect(cx+18, bot-30, 6, 30, twr);
        for (int i = 0; i < 3; i++) {   // crenellations
            fb.fillRect(cx + i*2 - 1,  bot-32, 1, 2, twr);
            fb.fillRect(cx + 18 + i*2, bot-32, 1, 2, twr);
        }
        fb.fillRect(cx+7,  bot-26, 2, 2, twr);
        fb.fillRect(cx+11, bot-26, 2, 2, twr);
        fb.fillRect(cx+15, bot-26, 2, 2, twr);
        fb.fillRect(cx+9, bot-10, 6, 10, TEAL_DEEP);       // arched door
        fb.fillCircle(cx+12, bot-10, 3, TEAL_DEEP);
        uint16_t win = uv ? aqGlowTint(LIME_PALE) : aqDim(LIME_PALE, 2);
        fb.fillRect(cx+7,  bot-19, 3, 4, win);
        fb.fillRect(cx+14, bot-19, 3, 4, win);
        fb.drawPixel(cx+2, bot-19, win);                   // tower arrow-slit
        uint16_t pole = uv ? aqDim(AQ_BROWN_DARK, 3) : AQ_BROWN_DARK;
        fb.drawLine(cx+3, bot-30, cx+3, bot-38, pole);
        fb.fillTriangle(cx+3, bot-38, cx+10, bot-36, cx+3, bot-34,
                        uv ? aqGlowTint(PURPLE_LIGHT) : PURPLE_LIGHT);
        fb.fillRect(cx+5,  bot-16, 6, 1, PURPLE_DEEP);     // brick hints
        fb.fillRect(cx+13, bot-8,  5, 1, PURPLE_DEEP);
    }
    // Coral (bit 2): staghorn branches + brain coral; neon under the black light
    if (aqDecorUnlocked & (1 << 2)) {
        int cx = 84;
        uint16_t cc  = uv ? aqGlowTint(PURPLE_LIGHT) : PURPLE_LIGHT;
        uint16_t tip = uv ? aqGlowTint(LIME_PALE)    : LIME_PALE;
        fb.drawLine(cx,   bot,    cx-4, bot-12, cc);
        fb.drawLine(cx-4, bot-12, cx-8, bot-20, cc);
        fb.drawLine(cx-4, bot-12, cx-1, bot-19, cc);
        fb.drawLine(cx,   bot,    cx+3, bot-14, cc);
        fb.drawLine(cx+3, bot-14, cx+7, bot-21, cc);
        fb.drawLine(cx+3, bot-14, cx+1, bot-22, cc);
        fb.drawPixel(cx-8, bot-21, tip); fb.drawPixel(cx-1, bot-20, tip);
        fb.drawPixel(cx+7, bot-22, tip); fb.drawPixel(cx+1, bot-23, tip);
        int bx = cx + 12;   // brain coral dome
        if (uv) fb.fillCircle(bx, bot-3, 6, aqHalo(PURPLE_BRIGHT));
        fb.fillCircle(bx, bot-3, 5, uv ? aqDim(PURPLE_BRIGHT, 2) : PURPLE_BRIGHT);
        fb.drawCircle(bx, bot-4, 3, uv ? aqGlowTint(PURPLE_BRIGHT) : PURPLE_DARK);
    }
    // Chest (bit 3): treasure chest bubbler - gold glow slit; the bubble
    // column itself lives in the shared bubble pool (see aquariumTick).
    if (aqDecorUnlocked & (1 << 3)) {
        int cx = AQ_CHEST_X;
        uint16_t body = uv ? aqDim(AQ_BROWN, 2)      : AQ_BROWN;
        uint16_t lid  = uv ? aqDim(AQ_BROWN_DARK, 2) : AQ_BROWN_DARK;
        fb.fillRect(cx-8, bot-10, 16, 10, body);
        fb.fillRect(cx-8, bot-14, 16, 4, lid);
        fb.fillRect(cx-8, bot-14, 16, 1, uv ? aqDim(AQ_SAND, 2) : AQ_SAND);
        uint16_t g = uv ? aqGlowTint(AQ_GOLD) : AQ_GOLD;
        if (uv) fb.fillCircle(cx, bot-10, 3, aqHalo(AQ_GOLD));
        fb.fillRect(cx-7, bot-10, 14, 1, g);   // treasure glow through the lid gap
        fb.drawPixel(cx, bot-8, g);            // lock plate
    }
    // Shipwreck (bit 4): hull with bow and stern, portholes, breach, broken mast
    if (aqDecorUnlocked & (1 << 4)) {
        int sx = 30;
        uint16_t hull = uv ? aqDim(AQ_BROWN, 3)      : AQ_BROWN;
        uint16_t dk   = uv ? aqDim(AQ_BROWN_DARK, 3) : AQ_BROWN_DARK;
        fb.fillRect(sx+2, bot-12, 36, 12, hull);
        fb.fillTriangle(sx+38, bot-12, sx+46, bot-4, sx+38, bot, dk);   // bow
        fb.fillTriangle(sx+2,  bot-12, sx-4,  bot-6, sx+2,  bot, dk);   // stern
        fb.fillRect(sx-2, bot-1, 48, 1, dk);                            // keel
        for (int i = 0; i < 3; i++) fb.drawPixel(sx+9+i*10, bot-7, TEAL_DEEP);
        fb.fillTriangle(sx+14, bot, sx+20, bot, sx+17, bot-4, TEAL_DEEP);   // breach
        fb.drawLine(sx+20, bot-12, sx+12, bot-32, dk);   // tilted mast
        fb.drawLine(sx+12, bot-32, sx+16, bot-36, dk);   // snapped tip
        fb.fillTriangle(sx+13, bot-30, sx+24, bot-24, sx+14, bot-19,
                        uv ? aqDim(AQ_SAND, 3) : AQ_SAND);   // torn sail
    }
    // Geode (bit 5): faceted crystal cluster, blazing under the black light
    if (aqDecorUnlocked & (1 << 5)) {
        int gx = 156;
        uint16_t a  = uv ? aqGlowTint(PURPLE)        : PURPLE;
        uint16_t b  = uv ? aqGlowTint(PURPLE_BRIGHT) : PURPLE_BRIGHT;
        uint16_t c  = uv ? aqGlowTint(PURPLE_LIGHT)  : PURPLE_LIGHT;
        uint16_t fl = uv ? aqGlowTint(LIME_PALE)     : LIME_PALE;
        if (uv) fb.fillCircle(gx+4, bot-6, 12, aqHalo(PURPLE_LIGHT));
        fb.fillTriangle(gx,   bot, gx-8,  bot, gx-4, bot-16, a);
        fb.fillTriangle(gx+2, bot, gx+12, bot, gx+7, bot-20, b);
        fb.fillTriangle(gx-2, bot, gx+4,  bot, gx+1, bot-9,  c);
        fb.drawLine(gx-4, bot-16, gx-5, bot-4, fl);   // facet edges
        fb.drawLine(gx+7, bot-20, gx+6, bot-4, fl);
        if (uv) { fb.drawPixel(gx-4, bot-18, AQ_WHITE); fb.drawPixel(gx+7, bot-22, AQ_WHITE); }
    }
}

// Black-light glow tint. The old version brightened every channel toward
// white, which read as washed-out pastel. This one snaps the color to its
// most electric self: stretch the channels to full saturation AND full
// brightness, keeping the hue (LIME_PALE -> pure green, PURPLE_LIGHT -> hot
// magenta, TEAL_BRIGHT -> laser cyan). Non-glow content still dims, so the
// contrast between the dark tank and the neon accents gets even stronger.
static inline uint16_t aqGlowTint(uint16_t c) {
    int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn < 16) return (uint16_t)((25 << 11) | (63 << 5) | 31);  // near-gray -> electric ice
    r = (r - mn) * 255 / (mx - mn);
    g = (g - mn) * 255 / (mx - mn);
    b = (b - mn) * 255 / (mx - mn);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Free any loaded background.
static void aqBgFree() {
    if (aqBgRle) { free(aqBgRle); aqBgRle = nullptr; }
    aqBgRleLen = 0; aqBgCurrent = -1; aqBgPalCount = 0; aqBgGlowCount = 0;
}

// Load a background .aqbg from the content partition into the heap RLE buffer.
static bool aqBgLoad(int bgIndex) {
    if (bgIndex == aqBgCurrent) return true;     // already loaded
    aqBgFree();
    if (bgIndex <= 0 || bgIndex >= BG_COUNT) { aqBgCurrent = bgIndex; return false; }
    const char* stem = BACKGROUNDS[bgIndex].file;
    if (!stem || !stem[0]) { aqBgCurrent = bgIndex; return false; }
    if (!aqFsMount()) return false;
    char path[48]; snprintf(path, sizeof(path), "/content/%s.aqbg", stem);
    fs::File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t hdr[10];
    if (f.read(hdr, 10) != 10 || hdr[0] != 'A' || hdr[1] != 'Q' || hdr[2] != 'B' || hdr[3] != 'G') {
        f.close(); return false;
    }
    // Header (10 bytes): "AQBG"(0-3) ver(4) W(5) H_hi(6) H_lo(7) palCount(8) glowCount(9)
    aqBgPalCount  = hdr[8];
    aqBgGlowCount = hdr[9];
    if (aqBgPalCount > 16) aqBgPalCount = 16;
    for (int i = 0; i < aqBgPalCount; i++) {
        uint8_t lo = f.read(), hi = f.read();
        aqBgPalette[i] = (uint16_t)lo | ((uint16_t)hi << 8);
    }
    int rleLen = f.size() - f.position();
    if (rleLen <= 0 || rleLen > 40000) { f.close(); return false; }   // sanity cap
    aqBgRle = (uint8_t*)malloc(rleLen);
    if (!aqBgRle) { f.close(); return false; }
    aqBgRleLen = f.read(aqBgRle, rleLen);
    f.close();
    aqBgCurrent = bgIndex;
    return aqBgRleLen > 0;
}

// Stream the loaded RLE background straight into the framebuffer (water area).
// When the black light is on, glow palette slots (0..glowCount-1) are tinted
// bright; everything else is darkened so the glow really pops.
static void aqBgDrawImage() {
    if (!aqBgRle) return;
    int px = 0;                       // linear pixel index into the WxH image
    const int total = AQBG_W * AQBG_H;
    for (int i = 0; i + 1 < aqBgRleLen && px < total; i += 2) {
        int run = aqBgRle[i];
        int idx = aqBgRle[i+1] & 0x0F;
        if (idx >= aqBgPalCount) idx = 0;
        uint16_t col = aqBgPalette[idx];
        bool isGlow = (idx < aqBgGlowCount);
        if (aqBlackLight) {
            if (isGlow) col = aqGlowTint(col);
            else {
                // darken non-glow under black light
                uint8_t r=(col>>11)&0x1F,g=(col>>5)&0x3F,b=col&0x1F;
                col = ((r/4)<<11)|((g/4)<<5)|(b/4);
            }
        }
        // paint the run (may span rows); rows under the sand are skipped
        while (run > 0 && px < total) {
            int x = px % AQBG_W;
            int y = px / AQBG_W;
            if (WATER_TOP + y >= WATER_BOT) { px = total; break; }
            int rowRemain = AQBG_W - x;
            int span = run < rowRemain ? run : rowRemain;
            fb.fillRect(x, WATER_TOP + y, span, 1, col);
            px += span; run -= span;
        }
    }
}

static void drawTank(bool hud) {
    // Background: a loaded image if one is selected, else the built-in water.
    if (aqBgCurrent > 0 && aqBgRle) {
        aqBgDrawImage();
    } else {
        // Built-in "Default" water: 7-band depth gradient, dithered band seams,
        // and two faint god-rays (rays are skipped under the black light).
        const int Hw = WATER_BOT - WATER_TOP;
        for (int b = 0; b < 7; b++) {
            uint16_t c = AQ_GRAD[b];
            if (aqBlackLight) c = aqDim(c, 3);
            int y0 = WATER_TOP + b * Hw / 7, y1 = WATER_TOP + (b + 1) * Hw / 7;
            fb.fillRect(0, y0, W, y1 - y0, c);
        }
        for (int b = 1; b < 7; b++) {
            uint16_t ca = AQ_GRAD[b-1], cb = AQ_GRAD[b];
            if (aqBlackLight) { ca = aqDim(ca, 3); cb = aqDim(cb, 3); }
            int y = WATER_TOP + b * Hw / 7;
            for (int x = 0; x < W; x++) fb.drawPixel(x, y, ((x + b) & 1) ? ca : cb);
        }
        if (!aqBlackLight) {
            for (int r = 0; r < 2; r++) {
                int bx = 34 + r * 72, h = 110 - r * 20;
                for (int yy = WATER_TOP; yy < WATER_TOP + h; yy += 2) {
                    int xx = bx + (yy - WATER_TOP) * 2 / 9;
                    fb.drawPixel(xx, yy, AQ_RAY);
                    if ((yy & 3) == 0) fb.drawPixel(xx + 1, yy, AQ_RAY);
                }
            }
        }
    }
    // sand: lighter beach tone with a sun-lit top edge and dark speckles
    {
        uint16_t sc = aqBlackLight ? aqDim(AQ_SAND, 3) : AQ_SAND;
        fb.fillRect(0, WATER_BOT - 4, W, 4 + 16, sc);
        fb.fillRect(0, WATER_BOT - 4, W, 1, aqBlackLight ? aqDim(AQ_SAND_HI, 3) : AQ_SAND_HI);
        uint16_t spk = aqDim(AQ_SAND, 2);
        for (int i = 0; i < 24; i++)
            fb.drawPixel((i * 37) % W, WATER_BOT - 2 + (i * 13) % 16, spk);
    }

    drawDecor();

    // bubbles
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (bubbles[i].active) fb.drawCircle((int)bubbles[i].x, (int)bubbles[i].y, bubbles[i].r, TEAL_LIGHT);

    // pellets
    for (int i = 0; i < MAX_PELLETS; i++)
        if (pellets[i].active) fb.fillCircle((int)pellets[i].x, (int)pellets[i].y, 2, LIME_PALE);

    // fish (index feeds each fish its own wiggle phase)
    for (int i = 0; i < MAX_FISH; i++)
        if (fish[i].active) drawFish(fish[i], i);

    if (hud) {
        // header
        fb.fillRect(0, 0, W, 18, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("AQUARIUM", 4, 9, 1);
        char c[24]; snprintf(c, sizeof(c), "%lu c | %u pel",
                             (unsigned long)aqCoins, (unsigned)aqPellets);
        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(aqPellets == 0 ? PURPLE_LIGHT : LIME);
        fb.drawString(c, W - 4, 9, 1);

        // transient message (out of pellets, quest food, ...)
        if (aqMsg[0] && millis() - aqMsgAt < 2200) {
            fb.fillRect(0, 18, W, 12, LIME_DARK);
            fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
            fb.drawString(aqMsg, W/2, 24, 1);
        }

        // footer (compact so the three hints never overlap on the 170px screen)
        fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("L shop", 4, H - 7, 1);
        fb.setTextDatum(MR_DATUM);
        fb.drawString(aqBlackLightUnlocked ? "feed / hold:UV" : "R feed", W - 4, H - 7, 1);
    }
}

// Fish-list screen: every fish with its goofy name, species, and stats.
// LEFT taps move the selection; HOLD R sells the selected fish for its
// breed+size price (see aqSellPrice).
static void drawFishList() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MY FISH", 4, 8, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME);
    char cb[16]; snprintf(cb, sizeof(cb), "%lu c", (unsigned long)aqCoins);
    fb.drawString(cb, W-4, 8, 1);

    // gather active fish into an ordered list
    int order[MAX_FISH]; int n = 0;
    for (int i = 0; i < MAX_FISH; i++) if (fish[i].active) order[n++] = i;
    if (n == 0) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("no fish", W/2, H/2, 2);
        return;
    }
    if (aqFishSel >= n) aqFishSel = n-1;

    // scrolling window of rows (~34px each), keep selection visible
    const int rowH = 34, top = 22, visible = 8;
    int first = aqFishSel - visible/2; if (first < 0) first = 0;
    if (first > n - visible) first = (n > visible) ? n - visible : 0;

    for (int r = 0; r < visible && first+r < n; r++) {
        int fi = order[first+r];
        Fish& f = fish[fi];
        const Species& sp = SPECIES[f.species < SPECIES_COUNT ? f.species : 0];
        int y = top + r*rowH;
        bool sel = (first+r == aqFishSel);
        if (sel) fb.fillRoundRect(2, y-1, W-4, rowH-3, 3, PURPLE_DARK);
        // a live mini sprite as the swatch (wiggles like the tank ones)
        drawFishSprite(f.species, 13, y+10, 1, 0.7f, millis() * 0.006f + r);
        // name (goofy nickname)
        fb.setTextDatum(TL_DATUM);
        fb.setTextColor(sel ? LIME_PALE : TEAL_LIGHT);
        fb.drawString(FISH_NAMES[f.nameIdx < FISH_NAME_COUNT ? f.nameIdx : 0], 24, y+2, 2);
        // species + size stat
        fb.setTextColor(LIME_DARK);
        char line[32];
        int pct = (int)(f.grow * 100.0f);
        snprintf(line, sizeof(line), "%s - size %d%% - %dc", sp.name, pct, aqSellPrice(f));
        fb.drawString(line, 24, y+18, 1);
    }

    // footer with controls
    fb.fillRect(0, H-14, W, 14, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L pick", 4, H-7, 1);
    // live price of the currently-selected fish in the footer
    {
        int n2 = 0, idx2 = 0; int selFish = -1;
        for (int i = 0; i < MAX_FISH; i++) if (fish[i].active) {
            if (idx2 == aqFishSel) { selFish = i; }
            idx2++; n2++;
        }
        char sellTxt[20];
        if (selFish >= 0) snprintf(sellTxt, sizeof(sellTxt), "holdR sell %dc", aqSellPrice(fish[selFish]));
        else              snprintf(sellTxt, sizeof(sellTxt), "holdR sell");
        (void)n2;
        fb.setTextDatum(MR_DATUM); fb.drawString(sellTxt, W-4, H-7, 1);
    }
}

static void drawShop() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("SHOP", 4, 9, 1);
    char c[16]; snprintf(c, sizeof(c), "%lu coins", (unsigned long)aqCoins);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME);
    fb.drawString(c, W - 4, 9, 1);

    int total = SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT + SHOP_BG_COUNT + 1;  // +1 pellets; bgs include Default
    // Scrolling window: keep the selected row visible so the list (species,
    // decor, backgrounds, then fish food) is fully reachable even though it's
    // taller than the screen.
    const int rowH = 24, top = 26, visible = 11;
    int first = aqShopSel - visible/2; if (first < 0) first = 0;
    if (first > total - visible) first = (total > visible) ? total - visible : 0;

    fb.setTextDatum(TL_DATUM);
    // section labels so it's obvious backgrounds live down here
    for (int r = 0; r < visible && first + r < total; r++) {
        int i = first + r;
        int y = top + r * rowH;
        bool sel = (i == aqShopSel);
        bool owned; int cost; const char* name;
        bool isFood = (i == total - 1);
        if (isFood) {
            owned = false; cost = 2; name = "5 Pellets";
        } else if (i < SHOP_SPECIES_COUNT) {
            owned = aqSpeciesUnlocked & (1 << i);
            cost = SPECIES[i].cost; name = SPECIES[i].name;
        } else if (i < SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT) {
            int d = i - SHOP_SPECIES_COUNT;
            owned = aqDecorUnlocked & (1 << d);
            cost = DECOR[d].cost; name = DECOR[d].name;
        } else {
            int b = i - SHOP_SPECIES_COUNT - SHOP_DECOR_COUNT;   // 0 = Default
            owned = aqBgUnlocked & (1 << b);
            cost = BACKGROUNDS[b].cost; name = BACKGROUNDS[b].name;
        }
        if (sel) fb.fillRect(2, y - 1, W - 4, 22, PURPLE_DARK);
        fb.setTextColor(sel ? LIME_PALE : TEAL_LIGHT);
        fb.drawString(name, 8, y + 2, 2);
        // tag rows so the player knows what section they're in
        bool isBg   = (!isFood && i >= SHOP_SPECIES_COUNT + SHOP_DECOR_COUNT);
        bool isDecor= (!isFood && !isBg && i >= SHOP_SPECIES_COUNT);
        int bgIdx = isBg ? (i - SHOP_SPECIES_COUNT - SHOP_DECOR_COUNT) : -1;
        fb.setTextColor(TEAL_DARK);
        if (isFood) { char have[20]; snprintf(have,sizeof(have),"food (have %u)",(unsigned)aqPellets);
            fb.drawString(have, 8, y + 15, 1); }
        else fb.drawString(isBg ? "bg" : (isDecor ? "decor" : "fish"), 8, y + 15, 1);
        char info[20];
        if (isFood)                          snprintf(info, sizeof(info), "2c");
        else if (owned && i < SHOP_SPECIES_COUNT) snprintf(info, sizeof(info), "+1 = 5c");
        else if (owned && isBg)  snprintf(info, sizeof(info), bgIdx==aqBgSelected ? "active" : "select");
        else if (owned)          snprintf(info, sizeof(info), "owned");
        else                     snprintf(info, sizeof(info), "%dc", cost);
        fb.setTextDatum(TR_DATUM);
        fb.setTextColor(owned ? LIME : (aqCoins >= (uint32_t)cost ? LIME_PALE : PURPLE_LIGHT));
        fb.drawString(info, W - 8, y + 2, 2);
        fb.setTextDatum(TL_DATUM);
    }
    // scroll arrows if there's more above/below
    if (first > 0) { fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK); fb.drawString("^", W/2, 22, 1); }
    if (first + visible < total) { fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK); fb.drawString("v", W/2, H-18, 1); }

    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L pick  R buy", 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("hold:fish", W - 4, H - 7, 1);
}

void aquariumDraw() {
    if (aqScreen == AQ_SHOP) {
        drawShop();
    } else if (aqScreen == AQ_FISHLIST) {
        drawFishList();
    } else if (aqScreen == AQ_SCREENSAVER) {
        fb.fillSprite(TEAL_DEEP);
        drawTank(false);   // pure ambient view - no HUD, no hints
    } else {
        fb.fillSprite(TEAL_DEEP);
        drawTank(true);
    }

    // One-time celebratory banner when the all-fish story unlocks.
    if (aqAllFishBanner) {
        fb.fillRoundRect(10, H/2 - 34, W - 20, 68, 6, PURPLE_DARK);
        fb.drawRoundRect(10, H/2 - 34, W - 20, 68, 6, LIME);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(LIME_PALE);
        fb.drawString("ALL FISH UNLOCKED!", W/2, H/2 - 14, 1);
        fb.setTextColor(LIME);
        fb.drawString("New CYOA story:", W/2, H/2 + 4, 1);
        fb.setTextColor(LIME_PALE);
        fb.drawString("The Drowned Reliquary", W/2, H/2 + 20, 1);
    }

    fb.pushSprite(0, 0);
}