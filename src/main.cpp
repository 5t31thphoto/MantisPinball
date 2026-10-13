// ============================================================================
//  Pinball-S3   |   LilyGo T-Display-S3 (ESP32-S3R8)
//
//  Real arcade pinball with rotating flippers, line-segment collision,
//  funnel walls, destructible targets, bumpers, multiball, projectile
//  powerups, power-flipper mode, procedural level generation, and a
//  cinematic intro screen.
//
//  Held in portrait. Buttons at the bottom ARE the flipper buttons.
//
//  Controls
//    Left  (GPIO 0)   left flipper / start game
//    Right (GPIO 14)  right flipper / start game
//    Hold BOTH 3 sec  deep sleep
//    Wake             press either button
//
//  Color palette - everything is a shade of one of these three:
//    Teal    #007373    background, walls, structure
//    Purple  #5D005D    targets, bumpers, intro title
//    Lime    #00FF00    flippers, ball, powerups, accents
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <math.h>

#include "intro_bitmaps.h"
#include "build_config.h"
#include "shared_buffers.h"
#include "miner/btc_miner.h"
#include "weather/weather_mode.h"
#include "space/space_mode.h"
#include "rabbit/rabbit_mode.h"
#include "extras/extras_mode.h"
#include "versus/versus_mode.h"
#include "content/guides_mode.h"
#include "content/guidebooks_mode.h"
#include "quests/quest_profile.h"
#include "sysclock/sysclock.h"
#include "power_guard.h"
#include "backlight/backlight.h"
#include "cyoa/cyoa_mode.h"
#include "quests/quests_mode.h"
#include "share/share_mode.h"
#include "pet/pet_mode.h"
#include "pet/pet_state.h"
#include "moon/moon_mode.h"
#include "neo/neo_mode.h"
#include "clicker/clicker_mode.h"
#include "dice/dice_mode.h"
#include "aquarium/aquarium_mode.h"
#include "fruit/fruit_mode.h"
#include "roadhunt/roadhunt_mode.h"
#include "sightings/sightings_mode.h"
#include "mantiscannon/mantiscannon_mode.h"
#include "mantisskate/mantisskate_mode.h"
#include "mantisfish/mantisfish_mode.h"
#include "dunerider/dunerider_mode.h"
#include "mantisputt/mantisputt_mode.h"
#include "mantisdrop/mantisdrop_mode.h"
#include "mantisvaders/mantisvaders_mode.h"
#include "mantis/mantis_psychic.h"
#include "rubiks/rubiks_mode.h"
#include "fireworks/fireworks_mode.h"
#include "mantischat/mantischat_mode.h"
#include "danceparty/danceparty_mode.h"
#include "ham/ham_mode.h"
#include "setup/setup_mode.h"
#include "party/party_net.h"
#include "party/party_hub.h"
#include "sdk/app_sdk.h"
#include <Preferences.h>      // for persistent high scores (NVS "pinball")

// ---------------- HARDWARE PINS ----------------
static constexpr int PIN_BTN_L   = 0;
static constexpr int PIN_BTN_R   = 14;
static constexpr int PIN_LCD_PWR = 15;
static constexpr int PIN_BAT_ADC = 4;     // T-Display-S3 battery sense (1:2 divider)

// ---------------- PLAYFIELD ----------------
static constexpr int W = 170;
static constexpr int H = 320;

// ---------------- COLOR PALETTE ----------------
// Three base colors; everything else is a darker/lighter shade.
//   TEAL   #007373  background, walls, structure
//   PURPLE #5D005D  targets, bumpers, title
//   LIME   #00FF00  flippers, ball, powerups, accents
#include "theme_colors.h"  // rgb565() defined inside

// Teal shades
//static constexpr uint16_t TEAL_DEEP    = rgb565(  0,  35,  35);
//static constexpr uint16_t TEAL_DARK    = rgb565(  0,  60,  60);
//static constexpr uint16_t TEAL         = rgb565(  0, 115, 115);
//static constexpr uint16_t TEAL_BRIGHT  = rgb565(  0, 180, 180);
//static constexpr uint16_t TEAL_LIGHT   = rgb565( 80, 220, 220);

// Purple shades
//static constexpr uint16_t PURPLE_DEEP  = rgb565( 25,   0,  25);
//static constexpr uint16_t PURPLE_DARK  = rgb565( 50,   0,  50);
//static constexpr uint16_t PURPLE       = rgb565( 93,   0,  93);
//static constexpr uint16_t PURPLE_BRIGHT= rgb565(150,   0, 150);
//static constexpr uint16_t PURPLE_LIGHT = rgb565(220,  60, 220);

// Lime shades
//static constexpr uint16_t LIME_DARK    = rgb565(  0,  80,   0);
//static constexpr uint16_t LIME_MED     = rgb565(  0, 160,   0);
//static constexpr uint16_t LIME         = rgb565(  0, 255,   0);
//static constexpr uint16_t LIME_PALE    = rgb565(180, 255, 180);

// Game-element aliases
#define C_BG (TEAL_DEEP)
#define C_BG_FLASH (PURPLE_DARK)
#define C_WALL (TEAL)
#define C_WALL_HI (TEAL_BRIGHT)
#define C_FUNNEL (TEAL_DARK)

#define C_TARGET (PURPLE)
#define C_TARGET_HI (PURPLE_BRIGHT)
#define C_TARGET_HVY (PURPLE_LIGHT)
#define C_TARGET_EXP (LIME)

#define C_BUMPER (PURPLE_BRIGHT)
#define C_BUMPER_RIM (PURPLE_LIGHT)
#define C_BUMPER_HIT (LIME_PALE)

#define C_FLIPPER (LIME)
#define C_FLIPPER_OFF (LIME_DARK)
#define C_FLIPPER_PWR (LIME_PALE)

#define C_BALL (LIME_PALE)
#define C_BALL_CORE (LIME)
#define C_TRAIL (LIME_DARK)

#define C_PWR_SLOW (TEAL_LIGHT)
#define C_PWR_MULT (PURPLE_LIGHT)
#define C_PWR_BST (LIME)
#define C_PWR_PROJ (LIME_PALE)
#define C_PWR_FLIP (LIME_PALE)

#define C_PROJ (LIME)
#define C_PROJ_TRAIL (LIME_DARK)

#define C_HUD (LIME_PALE)
#define C_HUD_DIM (TEAL_BRIGHT)
#define C_WARN (LIME)

// Intro
#define C_INTRO_BG (TEAL)  // exact #007373
#define C_INTRO_MAIN (PURPLE)  // exact #5D005D
#define C_INTRO_SUB (LIME)  // lime green
#define C_INTRO_DIM (TEAL_DEEP)  // back parallax layer
#define C_INTRO_BACK (TEAL_BRIGHT)  // mid parallax layer

// ---------------- ENTITY LIMITS ----------------
static constexpr int MAX_BALLS       = 3;
static constexpr int MAX_TARGETS     = 22;
static constexpr int MAX_BUMPERS     = 6;
static constexpr int MAX_POWERUPS    = 4;
static constexpr int MAX_PROJECTILES = 12;
static constexpr int TRAIL_LEN       = 8;

// ---------------- BALL ----------------
static constexpr float BALL_R    = 4.0f;
static constexpr float GRAVITY   = 0.18f;
static constexpr float MAX_VEL   = 8.5f;

// ---------------- FLIPPERS ----------------
static constexpr float FLIP_LEN        = 38.0f;
static constexpr float FLIP_HALF_THICK = 3.0f;
static constexpr float FLIP_REST_DEG   = 28.0f;
static constexpr float FLIP_ACT_DEG    = -32.0f;
static constexpr float FLIP_REST_RAD   = FLIP_REST_DEG * (float)M_PI / 180.0f;
static constexpr float FLIP_ACT_RAD    = FLIP_ACT_DEG  * (float)M_PI / 180.0f;
static constexpr float FLIP_LERP_UP    = 0.55f;
static constexpr float FLIP_LERP_DOWN  = 0.30f;

// Drain gap = 3/4 of one ball width VISIBLE between the flipper tip caps
// at rest. Ball diameter = 8, tip cap radius = 4. With DRAIN_HALF_GAP=7,
// each rest tip lands 7 px from center, so tip centers are 14 px apart;
// the cap edges are then 14-4-4 = 6 px apart, which is 3/4 of a ball
// width. The ball can't fall straight through that gap, but if a flipper
// is moving up it sweeps the ball aside and the ball can slip through.
static constexpr float FLIP_Y          = (float)H - 22.0f;
static constexpr float DRAIN_HALF_GAP  = 7.0f;
static constexpr float FLIP_REST_COS   = 0.88295f;        // cos(28 deg)
static constexpr float FLIP_L_PX       = (float)W * 0.5f - DRAIN_HALF_GAP
                                         - FLIP_LEN * FLIP_REST_COS;
static constexpr float FLIP_R_PX       = (float)W * 0.5f + DRAIN_HALF_GAP
                                         + FLIP_LEN * FLIP_REST_COS;

// ---------------- POWERUP DURATIONS ----------------
static constexpr int PWR_FLIP_FRAMES = 600;     // 10 s of buffed flippers
static constexpr int PROJ_LIFE       = 80;      // ~1.3 s

// ---------------- DEEP SLEEP ----------------
static constexpr uint32_t SLEEP_HOLD_MS = 3000;
static constexpr uint64_t WAKE_MASK =
        (1ULL << PIN_BTN_L) | (1ULL << PIN_BTN_R);

// ---------------- WALLS ----------------
struct Wall {
    float x1, y1, x2, y2;
    float nx, ny;
    float restitution;       // 0.85 outer, ~0.57 funnels (2/3 as bouncy)
};
static constexpr int WALL_COUNT = 5;
Wall walls[WALL_COUNT];

// ---------------- STRUCTS ----------------
struct Ball {
    float x, y, vx, vy;
    bool  on;
    float trailX[TRAIL_LEN];
    float trailY[TRAIL_LEN];
};

struct Flipper {
    float pivotX, pivotY;
    float angle, prevAngle;
    float restAngle, activeAngle;
    int   sign;
    bool  active;
};

struct Target {
    float   x, y;
    uint8_t hp;
    uint8_t kind;            // 0 normal, 1 heavy, 2 explosive
    bool    on;
    int8_t  flashFrames;
};

struct Bumper {
    float   x, y;
    float   r;
    int8_t  flashFrames;
};

struct Powerup {
    float   x, y;
    uint8_t kind;            // 0 slow, 1 multi, 2 boost, 3 projectile, 4 powerflip
    int16_t life;
    bool    on;
};

struct Projectile {
    float   x, y, vx, vy;
    int8_t  life;
    bool    on;
    float   prevX, prevY;
};

// ---------------- POOL BALLS (CORNER POCKET) ----------------
// Heavier than the cue ball (mass = 2 vs cue mass = 1), no gravity (the
// "table" is flat - gravity acts on the Z axis), more friction, less
// bouncy off walls. Just slightly larger than the cue so the eye reads
// them as the same family but distinct.
static constexpr int   POOL_COUNT  = 6;
static constexpr float POOL_R      = 6.0f;
static constexpr float POOL_FRICTION = 0.985f;     // velocity multiplier per frame
static constexpr float POOL_WALL_REST = 0.70f;     // wall bounciness
static constexpr float POOL_MASS   = 2.0f;         // cue mass = 1
struct PoolBall {
    float x, y, vx, vy;
    bool  on;
};

// ---------------- SLOTS ----------------
//
// 13 card values (2..A), 2 suits (red/black). 5 reels, 5 hold-targets,
// 1 moving target. Reels show a card-back symbol until the first deal.
//
//   value: 0=2, 1=3, ... 8=10, 9=J, 10=Q, 11=K, 12=A
//   suit:  0=red, 1=black
//
static constexpr int   SLOT_REELS  = 5;
static constexpr int   SLOT_VALUES = 13;
static constexpr int   SLOT_SUITS  = 2;

struct Card {
    int8_t value;        // 0..12, or -1 = card back (not yet dealt)
    int8_t suit;         // 0 red, 1 black
};

struct Reel {
    Card    shown;       // currently displayed card
    bool    held;        // player has chosen to keep this reel for next spin
    int16_t spinFrames;  // > 0 = currently spinning, counts down
    int16_t stopAt;      // when spinFrames hits 0 the card is `shown`
};

struct SlotTarget {       // a small reusable hit-box for slot play
    float   x, y;
    int8_t  flashFrames;
    bool    on;
};

struct MovingTarget {
    float x, y;
    float vx;            // horizontal speed
    int8_t flashFrames;
};

// ---------------- GLOBAL STATE ----------------
TFT_eSPI    tft;
TFT_eSprite fb(&tft);

// Shared 60KB image buffer — declared in shared_buffers.h. Allocated on demand
// by network image modes (rabbit, extras) so it isn't permanently in BSS.
uint8_t* g_jpegBuf = nullptr;
const size_t g_jpegBufLen = 61440;

bool jpegBufAcquire() {
    if (!g_jpegBuf) g_jpegBuf = (uint8_t*)malloc(g_jpegBufLen);
    return g_jpegBuf != nullptr;
}
void jpegBufRelease() {
    if (g_jpegBuf) { free(g_jpegBuf); g_jpegBuf = nullptr; }
}
bool        fbReady = false;

Ball        balls[MAX_BALLS];
Target      targets[MAX_TARGETS];
Bumper      bumpers[MAX_BUMPERS];
Powerup     powerups[MAX_POWERUPS];
Projectile  projs[MAX_PROJECTILES];
Flipper     flipL, flipR;

// Mode-specific entities
PoolBall      poolBalls[POOL_COUNT];
int           pocketLastDropSide = 1;  // 0=left, 1=right - default right
uint16_t      pocketRacks = 0;         // racks cleared this game

Reel          reels[SLOT_REELS];
SlotTarget    holdTargets[SLOT_REELS]; // one above each reel
MovingTarget  spinTarget;
bool          slotsAwaitingFirstDeal = true;
bool          slotsHandSettled = false; // current hand finalized (post-spin)
bool          slotsInsuranceArmed = false; // quest Insurance item: refund one bust
bool          slotsCurrentSpinIsFirstDeal = false;  // tracks across the spin
int16_t       slotsLastWinFrames = 0;   // pulse the win label briefly
uint16_t      slotsLastWinPay = 0;
char          slotsLastWinName[16] = "";

bool     btnL = false, btnR = false;
bool     btnL_prev = false, btnR_prev = false;
// Set by the menu when it hands off to an app; read by appInputGuardBegin().
static bool     g_entryLatch = false;
uint32_t bothHeldSince = 0;

uint32_t lastFrame = 0;
uint32_t score   = 0;

uint8_t  combo   = 0;
uint16_t comboCooldown = 0;
float    flash   = 0.0f;
uint16_t powerFlipFrames = 0;

uint8_t  ballsRemaining = 3;
uint16_t levelNum = 1;

// ---------------- BATTERY ----------------
// Sampled once per second; displayed on every screen.
float    batteryV = 4.10f;
uint32_t batteryLastSample = 0;
static constexpr float BAT_FULL    = 3.85f;   // >= this -> 3/3
static constexpr float BAT_MID     = 3.60f;   // >= this -> 2/3
static constexpr float BAT_LOW     = 3.30f;   // >= this -> 1/3
static constexpr float BAT_CUTOFF  = 3.20f;   // below -> shutdown to protect cell

// ---------------- DOUBLE-CLICK BOTH BUTTONS ----------------
// Detects "press both, release both, press both again" inside a window.
// Used to open the game-select menu from any active game state.
static constexpr uint32_t DC_WINDOW_MS = 400;
enum DCState { DC_IDLE, DC_FIRST_PRESS, DC_FIRST_RELEASE };
DCState  dcState = DC_IDLE;
uint32_t dcStateAt = 0;
bool     dcFiredThisFrame = false;

// ---------------- GAME STATE MACHINE ----------------
enum AppState {
    STATE_INTRO,
    STATE_MENU,
    STATE_PLAYING,        // PINBALL
    STATE_POCKET,         // CORNER POCKET
    STATE_SLOTS,          // SLOTS
    STATE_MINER,          // BTC MINER (lottery solo mining)
    STATE_WEATHER,        // WEATHER REPORT
    STATE_SPACE,          // SPACE WEATHER + SEISMIC
    STATE_RABBIT,         // RABBIT HOLE
    STATE_APOD,           // NASA ASTRONOMY PIC OF THE DAY
    STATE_FLIGHT,         // FLIGHTS OVERHEAD
    STATE_CANDLES,        // CRYPTO CANDLES
    STATE_VERSUS,         // MULTIPLAYER PINBALL
    STATE_SCOUT,          // SCOUT BOOK (offline guide)
    STATE_SURVIVAL,       // SURVIVAL GUIDE (offline guide)
    STATE_FIRSTAID,       // FIRST AID (offline guide; now reached via GUIDEBOOKS)
    STATE_CYOA,           // CHOOSE YOUR OWN ADVENTURE + dice
    STATE_QUESTS,         // gamified daily to-do / RPG profile
    STATE_SHARE,          // Pinball Sharing AP (hosts playable HTML pinball)
    STATE_PET,            // Mantis pet (tamagotchi + battle + trade)
    STATE_MOON,           // Moon phase dashboard (offline)
    STATE_NEO,            // Near-Earth asteroid dashboard
    STATE_CLICKER,        // Dual click counter
    STATE_DICE,           // 3D wireframe dice roller
    STATE_AQUARIUM,       // Living aquarium sim
    STATE_HAM,            // Ham radio propagation dashboard
    STATE_PARTY,          // Party Game mode (charades now; more later)
    STATE_SETUP,          // Central on-demand config portal
    STATE_FRUIT,          // Fruit Frenzy (PAJ7620 gesture slicing; menu item
                          // only shown when the sensor is detected on the JST)
    STATE_GUIDEBOOKS,     // GUIDEBOOKS: one menu listing all offline guides
    STATE_ROADHUNT,
    STATE_SIGHTINGS,
    STATE_MANTISCANNON,
    STATE_MANTISSKATE,
    STATE_MANTISFISH,
    STATE_DUNERIDER,
    STATE_MANTISPUTT,
    STATE_MANTISDROP,
    STATE_MANTISVADERS,
    STATE_MANTISPSYCHIC,     // Minigolf on the pinball table (procedural holes)
    STATE_RUBIKS,
    STATE_FIREWORKS,
    STATE_MANTISCHAT,
    STATE_DANCEPARTY,
    STATE_GAMEOVER
};
AppState appState = STATE_INTRO;
AppState gameOverMode = STATE_PLAYING;   // remembers which game ended

// ── Persistent high scores (loaded/saved via NVS namespace "pinball") ─────────
static uint32_t hiScorePinball = 0;
static uint32_t hiScorePocket  = 0;
static uint32_t hiScoreSlots   = 0;

static void loadHighScores() {
    Preferences p;
    if (p.begin("pinball", true)) {
        hiScorePinball = p.getULong("hi_pinball", 0);
        hiScorePocket  = p.getULong("hi_pocket",  0);
        hiScoreSlots   = p.getULong("hi_slots",   0);
        p.end();
    }
}

// Seed installer-provided defaults into NVS on first boot. Each value is only
// written if the corresponding key is currently empty, so on-device setup is
// never clobbered. Empty DEFAULT_* strings are no-ops.
static void seedDefaultConfig() {
    const char* dSsid = DEFAULT_WIFI_SSID;
    const char* dPass = DEFAULT_WIFI_PASS;
    const char* dBtc  = DEFAULT_BTC_ADDR;
    const char* dZip  = DEFAULT_WX_ZIP;

    // WiFi + wallet live in the miner namespace ("btcminer")
    const char* dBch  = DEFAULT_BCH_ADDR;
    const char* dDgb  = DEFAULT_DGB_ADDR;
    const char* dPool = DEFAULT_BTC_POOL;
    if (dSsid[0] || dPass[0] || dBtc[0] || dBch[0] || dDgb[0] || dPool[0]) {
        Preferences p;
        if (p.begin("btcminer", false)) {
            if (dSsid[0] && p.getString("ssid", "").length() == 0)
                p.putString("ssid", dSsid);
            if (dPass[0] && p.getString("pass", "").length() == 0)
                p.putString("pass", dPass);
            if (dBtc[0]  && p.getString("btc_addr", "").length() == 0)
                p.putString("btc_addr", dBtc);
            if (dBch[0]  && p.getString("bch_addr", "").length() == 0)
                p.putString("bch_addr", dBch);
            if (dDgb[0]  && p.getString("dgb_addr", "").length() == 0)
                p.putString("dgb_addr", dDgb);
            if (dPool[0] && p.getString("btc_host", "").length() == 0)
                p.putString("btc_host", dPool);
            p.end();
        }
    }

    // Zip lives in the pinball namespace ("pinball" -> wx_zip)
    if (dZip[0]) {
        Preferences p;
        if (p.begin("pinball", false)) {
            if (p.getString("wx_zip", "").length() == 0)
                p.putString("wx_zip", dZip);
            p.end();
        }
    }
}
static void saveHighScore(AppState mode, uint32_t newScore) {
    if (mode == STATE_PLAYING && newScore <= hiScorePinball) return;
    if (mode == STATE_POCKET  && newScore <= hiScorePocket)  return;
    if (mode == STATE_SLOTS   && newScore <= hiScoreSlots)   return;
    Preferences p;
    if (!p.begin("pinball", false)) return;
    if (mode == STATE_PLAYING) { hiScorePinball = newScore; p.putULong("hi_pinball", newScore); }
    if (mode == STATE_POCKET)  { hiScorePocket  = newScore; p.putULong("hi_pocket",  newScore); }
    if (mode == STATE_SLOTS)   { hiScoreSlots   = newScore; p.putULong("hi_slots",   newScore); }
    p.end();
}
uint16_t introFrame = 0;

// ---------------- MENU ----------------
// Game-select overlay. Reached by double-clicking both buttons during
// any active game. PINBALL is always the first/default option.
struct MenuItem { const char* name; AppState target; };
#ifdef PINBALL_MENU_ORDER_DEFINED
// The installer generated the menu order; use it verbatim.
static const MenuItem MENU_ITEMS[] = {
    PINBALL_MENU_ITEMS
};
#else
// Hand-build fallback: include modes by their ENABLE_* flags in default order.
// Menu order per spec: pinball & games, then pet/RPG/stories, aquarium, small
// tools, offline guides, live-data dashboards, crypto, and Setup last.
static const MenuItem MENU_ITEMS[] = {
#if ENABLE_PINBALL
    { "PINBALL BLASTER", STATE_PLAYING },
#endif
#if ENABLE_POCKET
    { "CORNER POCKET",   STATE_POCKET  },
#endif
#if ENABLE_SLOTS
    { "JACKS OR BETTER", STATE_SLOTS   },
#endif
#if ENABLE_VERSUS
    { "VERSUS PINBALL",  STATE_VERSUS  },
#endif
#if ENABLE_SHARE
    { "PINBALL SHARING", STATE_SHARE   },
#endif
#if ENABLE_PARTY
    { "PARTY GAME",      STATE_PARTY   },
#endif
#if ENABLE_QUESTS
    { "QUESTS",          STATE_QUESTS  },
#endif
#if ENABLE_PET
    { "MANTIS PET",      STATE_PET     },
#endif
#if ENABLE_CYOA
    { "STORY MODE",      STATE_CYOA    },
#endif
#if ENABLE_AQUARIUM
    { "AQUARIUM",        STATE_AQUARIUM},
#endif
#if defined(ENABLE_FRUIT) && ENABLE_FRUIT
    { "FRUIT FRENZY",    STATE_FRUIT   },
#endif
#if ENABLE_DICE
    { "DICE",            STATE_DICE    },
#endif
#if ENABLE_CLICKER
    { "CLICK COUNTER",   STATE_CLICKER },
#endif
#if ENABLE_MOON
    { "MOON",            STATE_MOON    },
#endif
#if ENABLE_ROADHUNT
    { "ROAD HUNT", STATE_ROADHUNT },
#endif
#if ENABLE_SIGHTINGS
    { "SIGHTINGS", STATE_SIGHTINGS },
#endif
#if ENABLE_MANTISCANNON
    { "MANTIS CANNON", STATE_MANTISCANNON },
#endif
#if ENABLE_MANTISSKATE
    { "MANTIS SKATE", STATE_MANTISSKATE },
#endif
#if ENABLE_MANTISFISH
    { "MANTIS FISHING", STATE_MANTISFISH },
#endif
#if ENABLE_DUNERIDER
    { "DUNE RIDER", STATE_DUNERIDER },
#endif
#if ENABLE_MANTISPUTT
    { "MANTIS PUTT", STATE_MANTISPUTT },
#endif
#if ENABLE_MANTISDROP
    { "MANTIS DROP", STATE_MANTISDROP },
#endif
#if ENABLE_MANTISVADERS
    { "MANTIS VADERS", STATE_MANTISVADERS },
#endif
#if ENABLE_MANTISPSYCHIC
    { "MANTIS PSYCHIC", STATE_MANTISPSYCHIC },
#endif
#if ENABLE_RUBIKS
    { "CUBE SOLVER", STATE_RUBIKS },
#endif
#if ENABLE_FIREWORKS
    { "FIREWORKS", STATE_FIREWORKS },
#endif
#if ENABLE_MANTISCHAT
    { "MANTIS CHAT", STATE_MANTISCHAT },
#endif
#if ENABLE_DANCEPARTY
    { "DANCE PARTY", STATE_DANCEPARTY },
#endif
#if defined(ENABLE_GUIDEBOOKS) && ENABLE_GUIDEBOOKS
    { "GUIDEBOOKS",      STATE_GUIDEBOOKS},
#endif
#if ENABLE_RABBIT
    { "RABBIT HOLE",     STATE_RABBIT  },
#endif
#if ENABLE_WEATHER
    { "WEATHER REPORT",  STATE_WEATHER },
#endif
#if ENABLE_SPACE
    { "SPACE WEATHER",   STATE_SPACE   },
#endif
#if ENABLE_FLIGHT
    { "FLIGHTS OVERHEAD", STATE_FLIGHT },
#endif
#if ENABLE_NEO
    { "ASTEROIDS",       STATE_NEO     },
#endif
// Lightning + Wildfires combined into one "Storms/Fires" app.
#if ENABLE_HAM
    { "HAM RADIO",       STATE_HAM     },
#endif
#if ENABLE_CANDLES
    { "CRYPTO CANDLES",  STATE_CANDLES },
#endif
#if ENABLE_MINER
    { "BTC MINER",       STATE_MINER   },
#endif
    { "SETUP",           STATE_SETUP   },
};
#endif  // PINBALL_MENU_ORDER_DEFINED
static constexpr int MENU_COUNT = (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
static_assert(MENU_COUNT > 0,
    "At least one ENABLE_* menu mode must be set in build_config.h");

// ---- Runtime menu (hardware-gated filtering) --------------------------------
// MENU_ITEMS[] above is the compile-time source of truth (installer-defined),
// but some apps depend on hardware that may or may not be plugged in. We copy
// the list into menuItems[] at boot and every time the menu opens, dropping
// entries whose hardware is absent. Fruit Frenzy needs the PAJ7620 gesture
// sensor on the bottom JST, so it only shows up when the sensor answers on
// I2C - and because we re-probe on every menu open, hot-plugging the sensor
// makes the item appear the next time the menu is brought up.
static MenuItem menuItems[MENU_COUNT];
static int      menuCount = 0;
static void rebuildMenu() {
    menuCount = 0;
    for (int i = 0; i < MENU_COUNT; ++i) {
        if (MENU_ITEMS[i].target == STATE_FRUIT && !fruitSensorPresent())
            continue;   // sensor unplugged: hide the game
        menuItems[menuCount++] = MENU_ITEMS[i];
    }
}
int      menuIndex = 0;
AppState menuReturnTo = STATE_PLAYING;   // game we came from
uint32_t menuEnteredAt = 0;
static constexpr uint32_t MENU_COOLDOWN_MS = 500;

// ---------------- FORWARD DECLARATIONS ----------------
// A few functions reference each other across the file; declare them up
// front so we can lay out the actual definitions in a more readable order.
static void drawBatteryBar(int x, int y);

// ---------------- UTIL ----------------
static inline float clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Linear-congruential pseudo-random seeded per level. We avoid the global
// Arduino random() so seeding is reproducible regardless of other random
// calls happening (e.g. from powerup spawns during play).
static uint32_t lvlRng = 1;
static uint32_t lvlrand() { lvlRng = lvlRng * 1664525u + 1013904223u; return lvlRng; }
static int lvlrandRange(int lo, int hi) {       // hi exclusive
    if (hi <= lo) return lo;
    return lo + (int)(lvlrand() % (uint32_t)(hi - lo));
}
static float lvlrandF(float lo, float hi) {
    return lo + (hi - lo) * (float)(lvlrand() & 0xFFFF) / 65535.0f;
}

// ---------------- WALL CONSTRUCTION ----------------
static void buildWalls() {
    const float FUNNEL_TOP_Y = (float)H - 90.0f;

    // Outer rails - bouncy
    walls[0] = { 0.0f,        0.0f,        0.0f,    FUNNEL_TOP_Y, 1.0f,  0.0f, 0.85f };
    walls[1] = { (float)W,    0.0f,        (float)W, FUNNEL_TOP_Y,-1.0f, 0.0f, 0.85f };
    walls[2] = { 0.0f,        0.0f,        (float)W, 0.0f,         0.0f, 1.0f, 0.85f };

    // Funnel walls - LESS bouncy (2/3 of outer-wall restitution = ~0.57)
    walls[3] = { 0.0f,        FUNNEL_TOP_Y,
                 FLIP_L_PX - 4.0f, FLIP_Y - 2.0f, 0.0f, 0.0f, 0.57f };
    walls[4] = { (float)W,    FUNNEL_TOP_Y,
                 FLIP_R_PX + 4.0f, FLIP_Y - 2.0f, 0.0f, 0.0f, 0.57f };

    // Set funnel normals - hand-picked to point INTO the playfield.
    auto setNormal = [](Wall &w, bool leftFunnel) {
        float dx = w.x2 - w.x1, dy = w.y2 - w.y1;
        float len = sqrtf(dx*dx + dy*dy);
        if (len < 0.001f) { w.nx = 0; w.ny = -1; return; }
        if (leftFunnel) { w.nx =  dy / len; w.ny = -dx / len; }
        else            { w.nx = -dy / len; w.ny =  dx / len; }
    };
    setNormal(walls[3], true);
    setNormal(walls[4], false);
}

// ---------------- INPUT ----------------
static void readInput() {
    btnL_prev = btnL;
    btnR_prev = btnR;
    btnL = (digitalRead(PIN_BTN_L) == LOW);
    btnR = (digitalRead(PIN_BTN_R) == LOW);
}
static inline bool justPressed() {
    return (btnL && !btnL_prev) || (btnR && !btnR_prev);
}

// ---------------- DEEP SLEEP ----------------
static void goToSleep() {
    if (fbReady) {
        fb.fillSprite(C_BG);
        fb.setTextColor(C_HUD);
        fb.setTextDatum(MC_DATUM);
        fb.drawString("SLEEPING",     W / 2, H / 2 - 14, 4);
        fb.drawString("press to wake", W / 2, H / 2 + 14, 2);
        fb.pushSprite(0, 0);
    }
    delay(400);

    // --- Radio off: WiFi/BT can keep the RTC domain powered if left on ---
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop();
    esp_wifi_stop();

    // --- Put the ST7789 panel to sleep, not just the backlight ---
    // Killing only the backlight + power rail can leave the panel drawing
    // current; the SLPIN command drops it to microamps.
    tft.writecommand(0x28);   // DISPOFF - display off
    tft.writecommand(0x10);   // SLPIN   - enter sleep mode
    delay(120);               // ST7789 needs >=120ms to settle into sleep

    // --- Cut display power rail + backlight ---
    ledcDetachPin(TFT_BL);            // release PWM so the raw pin write takes effect
    digitalWrite(TFT_BL, LOW);
    pinMode(TFT_BL, INPUT);            // high-Z so no leak through the pin
    digitalWrite(PIN_LCD_PWR, LOW);    // drive the rail enable low (display off)

    // --- Configure the wake buttons as RTC inputs with pullups ---
    rtc_gpio_pullup_en((gpio_num_t) PIN_BTN_L);
    rtc_gpio_pulldown_dis((gpio_num_t) PIN_BTN_L);
    rtc_gpio_pullup_en((gpio_num_t) PIN_BTN_R);
    rtc_gpio_pulldown_dis((gpio_num_t) PIN_BTN_R);

    // --- Power down RTC peripherals we don't need; keep RTC IO for wake ---
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);

    esp_sleep_enable_ext1_wakeup(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    pgFlushAll();        // persist every dirty app before sleep. goToSleep()
                         // is reached by BOTH the manual 3 s hold and the
                         // low-voltage cutoff in tickBattery(), so this one
                         // call covers both shutdown paths.
    sysclockPersist();   // save current time so day tracking resumes after wake
    esp_deep_sleep_start();
}
static void handleSleepHold() {
    const uint32_t now = millis();
    if (btnL && btnR) {
        if (bothHeldSince == 0) {
            bothHeldSince = now;
            // The sleep-hold has begun and the "SLEEPING" bar is showing. This
            // is the one guaranteed shutdown moment, so persist every dirty app
            // NOW - deep sleep does not call the active app's Shutdown().
            pgFlushAll();
        } else if (now - bothHeldSince >= SLEEP_HOLD_MS) goToSleep();
    } else {
        bothHeldSince = 0;
    }
}

// ============================================================================
//  PROCEDURAL LEVEL GENERATION
//
//  Levels are built by composing PATTERNS. Each pattern is a function that
//  appends targets (or bumpers) to the global arrays. We seed lvlRng with
//  the level number (plus a salt), then pick which patterns to run from a
//  small library and tweak their parameters with more pseudo-random draws.
//
//  Same level number = same layout, every playthrough. Levels 1..N feel
//  distinct because the pattern combinations and parameters drift.
//
//  Entity placement helpers reject targets/bumpers that would overlap with
//  what's already been placed, so even with random parameter ranges we
//  never get a tangled mess.
// ============================================================================

static int gNumTargets = 0;
static int gNumBumpers = 0;

static bool collidesExisting(float x, float y, float r) {
    for (int i = 0; i < gNumTargets; ++i) {
        float dx = targets[i].x - x, dy = targets[i].y - y;
        float minD = r + 6.0f;
        if (dx*dx + dy*dy < minD*minD) return true;
    }
    for (int i = 0; i < gNumBumpers; ++i) {
        float dx = bumpers[i].x - x, dy = bumpers[i].y - y;
        float minD = r + bumpers[i].r + 4.0f;
        if (dx*dx + dy*dy < minD*minD) return true;
    }
    return false;
}

static void addTarget(float x, float y, uint8_t kind, uint8_t hp = 0) {
    if (gNumTargets >= MAX_TARGETS) return;
    if (x < 14 || x > W - 14 || y < 18 || y > H - 130) return;
    if (collidesExisting(x, y, 5.0f)) return;
    Target &t = targets[gNumTargets++];
    t.x = x; t.y = y;
    t.kind = kind;
    t.hp = hp ? hp : (kind == 1 ? 2 : 1);
    t.on = true;
    t.flashFrames = 0;
}

static void addBumper(float x, float y, float r) {
    if (gNumBumpers >= MAX_BUMPERS) return;
    if (x < 12 + r || x > W - 12 - r || y < 30 || y > H - 130) return;
    if (collidesExisting(x, y, r + 2.0f)) return;
    Bumper &m = bumpers[gNumBumpers++];
    m.x = x; m.y = y; m.r = r; m.flashFrames = 0;
}

// -------- TARGET PATTERNS --------

// Arc: N targets along a sinusoidal arc at the given y center
static void patArc(float yMid, int n, float amplitude, uint8_t kind) {
    if (n < 2) n = 2;
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)(n - 1);
        float x = 22.0f + t * (W - 44.0f);
        float y = yMid - sinf(t * (float)M_PI) * amplitude;
        addTarget(x, y, kind);
    }
}

// V-shape: 3 targets in a V centered at cx,cy, opening upward or downward
static void patV(float cx, float cy, float spread, uint8_t kind, bool inverted) {
    float dy = inverted ? -spread * 0.6f : spread * 0.6f;
    addTarget(cx - spread, cy - dy, kind);
    addTarget(cx,          cy + dy, kind);
    addTarget(cx + spread, cy - dy, kind);
}

// Diamond: 4 targets at NESW around (cx,cy)
static void patDiamond(float cx, float cy, float r, uint8_t kind) {
    addTarget(cx,     cy - r, kind);
    addTarget(cx + r, cy,     kind);
    addTarget(cx,     cy + r, kind);
    addTarget(cx - r, cy,     kind);
}

// Cross / plus: 5 targets - center + 4 cardinals
static void patCross(float cx, float cy, float r, uint8_t kind, uint8_t centerKind) {
    addTarget(cx, cy, centerKind);
    addTarget(cx,     cy - r, kind);
    addTarget(cx + r, cy,     kind);
    addTarget(cx,     cy + r, kind);
    addTarget(cx - r, cy,     kind);
}

// Grid: rows x cols compact rectangle
static void patGrid(float cx, float cy, int rows, int cols, float pitch, uint8_t kind) {
    float startX = cx - (cols - 1) * pitch * 0.5f;
    float startY = cy - (rows - 1) * pitch * 0.5f;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            addTarget(startX + c * pitch, startY + r * pitch, kind);
}

// Ring: 6-8 targets in a circle around (cx, cy)
static void patRing(float cx, float cy, float radius, int n, uint8_t kind) {
    for (int i = 0; i < n; ++i) {
        float a = (float)i / n * (float)M_PI * 2.0f;
        addTarget(cx + cosf(a) * radius, cy + sinf(a) * radius, kind);
    }
}

// Zigzag: alternating up/down across the screen
static void patZigzag(float yMid, int n, float amp, uint8_t kind) {
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)(n - 1);
        float x = 22.0f + t * (W - 44.0f);
        float y = yMid + ((i & 1) ? amp : -amp);
        addTarget(x, y, kind);
    }
}

// Column: vertical line of targets along the centerline
static void patColumn(float cx, float yTop, int n, float pitch, uint8_t kind) {
    for (int i = 0; i < n; ++i) addTarget(cx, yTop + i * pitch, kind);
}

// -------- BUMPER PATTERNS --------

static void bpatTriangle(float cx, float cy, float spread) {
    addBumper(cx,             cy - spread * 0.5f, 8.0f);
    addBumper(cx - spread,    cy + spread * 0.4f, 7.0f);
    addBumper(cx + spread,    cy + spread * 0.4f, 7.0f);
}
static void bpatLine(float yMid, int n) {
    for (int i = 0; i < n; ++i) {
        float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
        float x = 28.0f + t * (W - 56.0f);
        addBumper(x, yMid, 7.0f);
    }
}
static void bpatV(float cx, float cy, float spread) {
    addBumper(cx,          cy - spread * 0.4f, 8.0f);
    addBumper(cx - spread, cy + spread * 0.3f, 6.0f);
    addBumper(cx + spread, cy + spread * 0.3f, 6.0f);
}
static void bpatDiagonal(float cy) {
    addBumper(28.0f,        cy - 18.0f, 6.0f);
    addBumper((float)W/2.0f,cy,         8.0f);
    addBumper((float)W-28.0f,cy + 18.0f,6.0f);
}
static void bpatScattered(float cy) {
    addBumper(40.0f,        cy - 25.0f, 6.0f);
    addBumper(W - 40.0f,    cy - 25.0f, 6.0f);
    addBumper(W * 0.5f,     cy + 5.0f,  8.0f);
    addBumper(W * 0.3f,     cy + 30.0f, 5.0f);
    addBumper(W * 0.7f,     cy + 30.0f, 5.0f);
}

// -------- LEVEL ASSEMBLY --------

// Hand-crafted Level 1 - the canonical "first level" layout that we know
// plays well. Keeping this fixed gives players a familiar starting point
// every game.
static void seedLevelOne() {
    // Top arc - 7 normal targets
    for (int i = 0; i < 7; ++i) {
        float t = (float)i / 6.0f;
        float x = 22.0f + t * (W - 44.0f);
        float y = 38.0f + sinf(t * (float)M_PI) * 14.0f;
        addTarget(x, y, 0);
    }
    // Middle V - heavy targets
    addTarget(50.0f,  110.0f, 1);
    addTarget(85.0f,  130.0f, 1);
    addTarget(120.0f, 110.0f, 1);
    // Lower arc - center one explosive (lime)
    for (int i = 0; i < 5; ++i) {
        float t = (float)i / 4.0f;
        float x = 30.0f + t * (W - 60.0f);
        float y = 175.0f - sinf(t * (float)M_PI) * 12.0f;
        addTarget(x, y, (uint8_t)(i == 2 ? 2 : 0));
    }

    // Five bumpers - the original layout that lets balls reach the top
    addBumper( 35.0f,  80.0f, 7.0f);
    addBumper( 85.0f,  65.0f, 8.0f);
    addBumper(135.0f,  80.0f, 7.0f);
    addBumper( 25.0f, 155.0f, 6.0f);
    addBumper(145.0f, 155.0f, 6.0f);
}

// Level assembly entry point.
static void seedLevel(uint16_t lvl) {
    gNumTargets = 0;
    gNumBumpers = 0;
    for (int i = 0; i < MAX_TARGETS; ++i) targets[i].on = false;
    for (int i = 0; i < MAX_POWERUPS;    ++i) powerups[i].on = false;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projs[i].on    = false;

    if (lvl == 1) {
        seedLevelOne();
        return;
    }

    // ===== PROCEDURAL LEVELS (level >= 2) =====
    // Seed reproducibly so any given level number always plays out the same.
    lvlRng = (uint32_t)lvl * 2654435761u + 0x9E3779B9u;
    for (int i = 0; i < 4; ++i) lvlrand();

    // ---- TOP BAND (y ~ 30..60) ----
    int topPick = lvlrandRange(0, 4);
    switch (topPick) {
        case 0: patArc(50.0f, lvlrandRange(5, 8), lvlrandF(8.0f, 16.0f), 0); break;
        case 1: patZigzag(48.0f, lvlrandRange(5, 8), lvlrandF(6.0f, 10.0f), 0); break;
        case 2: patGrid(W*0.5f, 45.0f, 2, lvlrandRange(3, 5),
                        lvlrandF(18.0f, 24.0f), 0); break;
        case 3: patArc(55.0f, 7, lvlrandF(10.0f, 18.0f),
                       (lvl > 2 && lvlrandRange(0,2)) ? 1 : 0); break;
    }

    // ---- MIDDLE BAND (y ~ 95..135) ----
    int midPick = lvlrandRange(0, 5);
    float midY = 105.0f + lvlrandF(-8.0f, 12.0f);
    switch (midPick) {
        case 0: patV(W*0.5f, midY, lvlrandF(28.0f, 36.0f), 1, false); break;
        case 1: patDiamond(W*0.5f, midY, lvlrandF(20.0f, 28.0f), 1); break;
        case 2: patCross(W*0.5f, midY, lvlrandF(18.0f, 24.0f), 0,
                         (lvl > 1) ? 2 : 1); break;
        case 3: patRing(W*0.5f, midY, lvlrandF(22.0f, 28.0f), 6,
                        lvlrandRange(0, 2) ? 1 : 0); break;
        case 4: patV(W*0.5f, midY, lvlrandF(25.0f, 32.0f), 1, true); break;
    }

    // ---- LOWER BAND (y ~ 165..185) ----
    int lowPick = lvlrandRange(0, 4);
    float lowY = 175.0f + lvlrandF(-5.0f, 8.0f);
    switch (lowPick) {
        case 0: {
            int n = lvlrandRange(5, 8);
            for (int i = 0; i < n; ++i) {
                float t = (float)i / (float)(n - 1);
                float x = 30.0f + t * (W - 60.0f);
                float y = lowY - sinf(t * (float)M_PI) * 10.0f;
                bool isMid = (i == n / 2);
                addTarget(x, y, isMid ? 2 : 0);
            }
            break;
        }
        case 1: patZigzag(lowY, lvlrandRange(5, 7), 8.0f, 0); break;
        case 2: patArc(lowY, lvlrandRange(5, 7), 8.0f, 1); break;
        case 3: {
            float cx = W * 0.5f;
            patColumn(cx, lowY - 12.0f, 3, 12.0f, 0);
            addTarget(cx - 30.0f, lowY, 0);
            addTarget(cx + 30.0f, lowY, 0);
            break;
        }
    }

    // ---- BUMPERS ----
    // Density curve: levels 2..7 get a "full" bumper pattern (4-5 bumpers)
    // because the player NEEDS bumpers to keep the ball alive and reach
    // the top of the screen. From level 8 onward the count starts to thin
    // out as a difficulty bump.
    bool fullBumpers = (lvl < 8);
    bool sparseBumpers = (lvl >= 8 && lvlrandRange(0, 3) == 0);  // sometimes only 2
    int bPick = lvlrandRange(0, 5);
    float bY  = 80.0f + lvlrandF(-5.0f, 10.0f);

    if (fullBumpers) {
        // Full patterns - same as the L1 layout density
        switch (bPick) {
            case 0: bpatTriangle(W*0.5f, bY, lvlrandF(38.0f, 48.0f));
                    addBumper(25.0f, 155.0f, 6.0f);
                    addBumper(W - 25.0f, 155.0f, 6.0f);
                    break;
            case 1: bpatLine(bY, lvlrandRange(3, 5));
                    addBumper(W*0.5f, 155.0f, 7.0f);
                    break;
            case 2: bpatV(W*0.5f, bY, lvlrandF(35.0f, 45.0f));
                    addBumper(W*0.5f, 155.0f, 6.0f);
                    break;
            case 3: bpatDiagonal(bY);
                    addBumper(W*0.5f, 150.0f, 7.0f);
                    addBumper(W*0.3f, 165.0f, 5.0f);
                    break;
            case 4: bpatScattered(bY); break;       // already 5 bumpers
        }
    } else if (sparseBumpers) {
        // Just 2 - challenging
        addBumper(W*0.5f - 30.0f, bY, 7.0f);
        addBumper(W*0.5f + 30.0f, bY, 7.0f);
    } else {
        // 3 bumpers - moderate (default for 8+)
        switch (bPick) {
            case 0: bpatTriangle(W*0.5f, bY, lvlrandF(38.0f, 48.0f)); break;
            case 1: bpatV(W*0.5f, bY, lvlrandF(35.0f, 45.0f)); break;
            case 2: bpatDiagonal(bY); break;
            case 3: bpatLine(bY, 3); break;
            case 4: {
                addBumper(W*0.5f, bY, 8.0f);
                addBumper(35.0f, bY + 30.0f, 6.0f);
                addBumper(W - 35.0f, bY + 30.0f, 6.0f);
                break;
            }
        }
    }
}

// ---------------- BALL / GAME LIFECYCLE ----------------
static void resetBall(Ball &b) {
    b.x  = (float)W * 0.5f - 4.0f;
    b.y  = (float)H - 70.0f;
    b.vx = 1.6f;
    b.vy = -4.4f;
    b.on = true;
    for (int i = 0; i < TRAIL_LEN; ++i) {
        b.trailX[i] = b.x;
        b.trailY[i] = b.y;
    }
}

static void spawnPowerup() {
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        if (!powerups[i].on) {
            powerups[i].x    = 20.0f + (float)random(0, W - 40);
            powerups[i].y    = 50.0f + (float)random(0, 130);
            powerups[i].kind = (uint8_t)random(0, 5);     // 0..4
            powerups[i].life = 540;
            powerups[i].on   = true;
            return;
        }
    }
}

static void spawnProjectiles(float fromX, float fromY) {
    int spawned = 0;
    for (int i = 0; i < MAX_PROJECTILES && spawned < 8; ++i) {
        if (projs[i].on) continue;
        float a = ((float)spawned / 8.0f) * (float)M_PI * 2.0f
                + (float)(random(-30, 31)) * 0.001f;
        const float SPD = 4.5f;
        projs[i].x  = fromX;
        projs[i].y  = fromY;
        projs[i].prevX = fromX;
        projs[i].prevY = fromY;
        projs[i].vx = cosf(a) * SPD;
        projs[i].vy = sinf(a) * SPD;
        projs[i].life = PROJ_LIFE;
        projs[i].on = true;
        spawned++;
    }
}

// ---------------- COLLISION HELPERS ----------------
static void reflectVelocity(float &vx, float &vy, float nx, float ny,
                            float restitution, float friction) {
    float vn = vx * nx + vy * ny;
    if (vn > 0) return;
    float vt_x = vx - vn * nx;
    float vt_y = vy - vn * ny;
    vx = vt_x * friction - vn * restitution * nx;
    vy = vt_y * friction - vn * restitution * ny;
}

static float closestPointOnSegment(float x1, float y1, float x2, float y2,
                                   float px, float py,
                                   float &cx, float &cy) {
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx*dx + dy*dy;
    if (len2 < 0.0001f) { cx = x1; cy = y1; return 0.0f; }
    float t = ((px - x1) * dx + (py - y1) * dy) / len2;
    t = clampf(t, 0.0f, 1.0f);
    cx = x1 + t * dx;
    cy = y1 + t * dy;
    return t;
}

// ---------------- BALL COLLISION ROUTINES ----------------
static void collideWalls(Ball &b) {
    for (int i = 0; i < WALL_COUNT; ++i) {
        const Wall &w = walls[i];
        float dx = w.x2 - w.x1, dy = w.y2 - w.y1;
        if (dx*dx + dy*dy < 0.001f) continue;
        float cx, cy;
        closestPointOnSegment(w.x1, w.y1, w.x2, w.y2, b.x, b.y, cx, cy);
        float pdx = b.x - cx, pdy = b.y - cy;
        float d2  = pdx*pdx + pdy*pdy;
        if (d2 < BALL_R * BALL_R) {
            float d = sqrtf(d2);
            float nx, ny;
            if (d > 0.001f) { nx = pdx / d; ny = pdy / d; }
            else            { nx = w.nx;    ny = w.ny;    }
            float pen = BALL_R - d;
            b.x += nx * pen;
            b.y += ny * pen;
            // Outer rails 0.85, funnels 0.57 (~2/3 as bouncy)
            reflectVelocity(b.vx, b.vy, nx, ny, w.restitution, 0.96f);
        }
    }
}

static void hitTarget(Ball &b, Target &t, float nx, float ny) {
    reflectVelocity(b.vx, b.vy, nx, ny, 0.78f, 0.95f);
    if (t.hp > 0) t.hp--;
    flash = 1.0f;
    t.flashFrames = 3;
    combo++;
    comboCooldown = 0;
    score += 10u * combo;

    if (t.kind == 2) {
        spawnPowerup();
        for (int i = 0; i < gNumTargets; ++i) {
            if (!targets[i].on || &targets[i] == &t) continue;
            float ddx = targets[i].x - t.x, ddy = targets[i].y - t.y;
            if (ddx*ddx + ddy*ddy < 35.0f * 35.0f) {
                if (targets[i].hp > 0) targets[i].hp--;
                if (targets[i].hp == 0) targets[i].on = false;
                targets[i].flashFrames = 3;
                score += 15;
            }
        }
    }
    if (t.hp == 0) {
        t.on = false;
        score += 25;
        if (random(0, 4) == 0) spawnPowerup();
    }
}

static void collideTargets(Ball &b) {
    for (int i = 0; i < gNumTargets; ++i) {
        Target &t = targets[i];
        if (!t.on) continue;
        float dx = b.x - t.x, dy = b.y - t.y;
        float r  = BALL_R + 4.5f;
        if (dx*dx + dy*dy < r*r) {
            float d = sqrtf(dx*dx + dy*dy);
            float nx = (d > 0.001f) ? dx / d : 0.0f;
            float ny = (d > 0.001f) ? dy / d : -1.0f;
            float pen = r - d;
            b.x += nx * pen;
            b.y += ny * pen;
            hitTarget(b, t, nx, ny);
            return;
        }
    }
}

static void collideBumpers(Ball &b) {
    for (int i = 0; i < gNumBumpers; ++i) {
        Bumper &m = bumpers[i];
        float dx = b.x - m.x, dy = b.y - m.y;
        float r  = BALL_R + m.r;
        if (dx*dx + dy*dy < r*r) {
            float d = sqrtf(dx*dx + dy*dy);
            if (d < 0.001f) d = 0.001f;
            float nx = dx / d, ny = dy / d;
            float pen = r - d;
            b.x += nx * pen;
            b.y += ny * pen;
            reflectVelocity(b.vx, b.vy, nx, ny, 0.95f, 0.98f);
            const float KICK = 2.6f;
            b.vx += nx * KICK;
            b.vy += ny * KICK;
            m.flashFrames = 4;
            flash = fmaxf(flash, 0.7f);
            combo++;
            comboCooldown = 0;
            score += 5u * combo;
        }
    }
}

// ---------------- FLIPPER UPDATE ----------------
static void initFlippers() {
    flipL = { FLIP_L_PX, FLIP_Y, FLIP_REST_RAD, FLIP_REST_RAD,
              FLIP_REST_RAD, FLIP_ACT_RAD, +1, false };
    flipR = { FLIP_R_PX, FLIP_Y,
              (float)M_PI - FLIP_REST_RAD, (float)M_PI - FLIP_REST_RAD,
              (float)M_PI - FLIP_REST_RAD, (float)M_PI - FLIP_ACT_RAD,
              -1, false };
}

static void stepFlipper(Flipper &f, bool pressed) {
    f.prevAngle = f.angle;
    f.active    = pressed;
    float target  = pressed ? f.activeAngle : f.restAngle;
    float lerpAmt = pressed ? FLIP_LERP_UP  : FLIP_LERP_DOWN;
    f.angle = lerp(f.angle, target, lerpAmt);
}

static inline void flipperEndpoints(const Flipper &f, float &tipX, float &tipY) {
    tipX = f.pivotX + cosf(f.angle) * FLIP_LEN;
    tipY = f.pivotY + sinf(f.angle) * FLIP_LEN;
}

static void collideFlipper(Ball &b, Flipper &f) {
    float tipX, tipY;
    flipperEndpoints(f, tipX, tipY);

    float cx, cy;
    closestPointOnSegment(f.pivotX, f.pivotY, tipX, tipY, b.x, b.y, cx, cy);

    float pdx = b.x - cx, pdy = b.y - cy;
    float minDist = BALL_R + FLIP_HALF_THICK;
    float d2 = pdx*pdx + pdy*pdy;
    if (d2 >= minDist * minDist) return;

    float d = sqrtf(d2);
    float nx, ny;
    if (d > 0.001f) { nx = pdx / d; ny = pdy / d; }
    else {
        float dx = tipX - f.pivotX, dy = tipY - f.pivotY;
        float len = sqrtf(dx*dx + dy*dy);
        nx = -dy / len; ny =  dx / len;
    }

    // Anti-trap: don't let an active/extended flipper trap a ball under it
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

    reflectVelocity(b.vx, b.vy, nx, ny, 0.92f, 0.97f);

    // Tangential kick at contact radius
    float omega = f.angle - f.prevAngle;
    float rx = cx - f.pivotX;
    float ry = cy - f.pivotY;
    float tx = -ry;
    float ty =  rx;
    // Power Flippers powerup doubles the gain
    // Boost the kick in CORNER POCKET and SLOTS so the ball can reach the
    // top of the screen (those modes have no bumpers helping it along).
    // PINBALL keeps its original tuning.
    bool boosted = (appState == STATE_POCKET || appState == STATE_SLOTS);
    float baseGain = boosted ? 130.0f : 18.0f;
    float kickGain = (powerFlipFrames > 0) ? baseGain * (32.0f / 18.0f) : baseGain;
    b.vx += tx * omega * kickGain;
    b.vy += ty * omega * kickGain;

    // Speed cap
    float sp2 = b.vx * b.vx + b.vy * b.vy;
    float capMul = boosted ? 14.0f : 1.4f;
    float capSp = MAX_VEL * (powerFlipFrames > 0 ? 1.7f : capMul);
    if (sp2 > capSp * capSp) {
        float s = capSp / sqrtf(sp2);
        b.vx *= s; b.vy *= s;
    }
}

// ---------------- POWERUPS ----------------
static void collidePowerups(Ball &b) {
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        Powerup &p = powerups[i];
        if (!p.on) continue;
        float dx = b.x - p.x, dy = b.y - p.y;
        float r  = BALL_R + 5.0f;
        if (dx*dx + dy*dy < r*r) {
            switch (p.kind) {
                case 0: b.vx *= 0.55f; b.vy *= 0.55f; break;
                case 1: {
                    for (int j = 0; j < MAX_BALLS; ++j) {
                        if (!balls[j].on) {
                            balls[j] = b;
                            balls[j].vx += (float)random(-200, 201) / 100.0f;
                            balls[j].vy -= 2.0f;
                            balls[j].on = true;
                            break;
                        }
                    }
                    break;
                }
                case 2: b.vy = -fabsf(b.vy) - 4.0f; break;
                case 3: spawnProjectiles(p.x, p.y); break;
                case 4: powerFlipFrames = PWR_FLIP_FRAMES; break;
            }
            score += 50;
            p.on = false;
            flash = fmaxf(flash, 0.8f);
        }
    }
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        if (powerups[i].on && --powerups[i].life <= 0) powerups[i].on = false;
    }
}

// ---------------- PROJECTILES ----------------
static void stepProjectiles() {
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        Projectile &p = projs[i];
        if (!p.on) continue;
        p.prevX = p.x; p.prevY = p.y;
        p.x += p.vx;
        p.y += p.vy;
        if (--p.life <= 0 ||
            p.x < -2 || p.x > W + 2 || p.y < -2 || p.y > H + 2) {
            p.on = false;
            continue;
        }
        // Check target collisions
        for (int j = 0; j < gNumTargets; ++j) {
            Target &t = targets[j];
            if (!t.on) continue;
            float dx = p.x - t.x, dy = p.y - t.y;
            if (dx*dx + dy*dy < 6.0f * 6.0f) {
                if (t.hp > 0) t.hp--;
                t.flashFrames = 3;
                score += 8;
                if (t.hp == 0) {
                    t.on = false;
                    score += 15;
                }
                p.on = false;
                break;
            }
        }
        if (!p.on) continue;
        // Hit bumper -> bounce off (don't destroy bumper)
        for (int j = 0; j < gNumBumpers; ++j) {
            Bumper &m = bumpers[j];
            float dx = p.x - m.x, dy = p.y - m.y;
            float r  = m.r + 1.0f;
            if (dx*dx + dy*dy < r*r) {
                float d = sqrtf(dx*dx + dy*dy);
                if (d < 0.001f) d = 0.001f;
                float nx = dx / d, ny = dy / d;
                float vn = p.vx * nx + p.vy * ny;
                if (vn < 0) { p.vx -= 2 * vn * nx; p.vy -= 2 * vn * ny; }
                m.flashFrames = 2;
                break;
            }
        }
    }
}

// ---------------- BALL STEP ----------------
static void stepBall(Ball &b) {
    b.vy += GRAVITY;
    b.vx = clampf(b.vx, -MAX_VEL, MAX_VEL);
    b.vy = clampf(b.vy, -MAX_VEL, MAX_VEL);

    float speed = sqrtf(b.vx * b.vx + b.vy * b.vy);
    int   steps = (int)ceilf(speed / (BALL_R * 0.6f));
    if (steps < 1) steps = 1;
    if (steps > 4) steps = 4;
    float dt = 1.0f / steps;

    for (int s = 0; s < steps; ++s) {
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        collideWalls(b);
        collideTargets(b);
        collideBumpers(b);
        collideFlipper(b, flipL);
        collideFlipper(b, flipR);
    }
    collidePowerups(b);

    if (b.y > (float)H + 6.0f) b.on = false;

    for (int i = TRAIL_LEN - 1; i > 0; --i) {
        b.trailX[i] = b.trailX[i - 1];
        b.trailY[i] = b.trailY[i - 1];
    }
    b.trailX[0] = b.x;
    b.trailY[0] = b.y;
}

// ---------------- LEVEL / GAME STATE ----------------
static int aliveTargets() {
    int n = 0;
    for (int i = 0; i < gNumTargets; ++i) if (targets[i].on) n++;
    return n;
}

static void resetGame() {
    score = 0;
    combo = 0;
    levelNum = 1;
    ballsRemaining = 3;
    powerFlipFrames = 0;
    seedLevel(levelNum);
    for (int i = 0; i < MAX_BALLS; ++i) balls[i].on = false;
    resetBall(balls[0]);

    // Apply items the player ARMED from the Quest inventory ("use" there, and
    // the effect lands at the start of the next pinball game - here).
    if (qpTakePending(ITEM_PB_POWERFLIP))  powerFlipFrames = PWR_FLIP_FRAMES;
    if (qpTakePending(ITEM_PB_SLOWBALL))   { balls[0].vx *= 0.55f; balls[0].vy *= 0.55f; }
    if (qpTakePending(ITEM_PB_BOOST))      { balls[0].vy = -fabsf(balls[0].vy) - 4.0f; }
    if (qpTakePending(ITEM_PB_PROJECTILE)) { spawnProjectiles(balls[0].x, balls[0].y); }
    uint16_t xb = qpTakePending(ITEM_PB_EXTRABALL);
    if (xb) { ballsRemaining += xb; if (ballsRemaining > 6) ballsRemaining = 6; }
    uint16_t mb = qpTakePending(ITEM_PB_MULTIBALL);
    for (uint16_t k = 0; k < mb; ++k) {
        // Launch an extra ball alongside the first (one per armed Multiball).
        for (int j = 1; j < MAX_BALLS; ++j) {
            if (!balls[j].on) {
                balls[j] = balls[0];
                balls[j].vx += (float)random(-200, 201) / 100.0f;
                balls[j].vy -= 2.0f;
                balls[j].on = true;
                break;
            }
        }
    }
}

// ---------------- TICK ----------------
static void tickPlaying() {
    stepFlipper(flipL, btnL);
    stepFlipper(flipR, btnR);

    for (int i = 0; i < MAX_BALLS; ++i)
        if (balls[i].on) stepBall(balls[i]);

    stepProjectiles();

    if (++comboCooldown > 120) combo = 0;
    if (powerFlipFrames > 0) powerFlipFrames--;

    for (int i = 0; i < gNumTargets; ++i)
        if (targets[i].flashFrames > 0) targets[i].flashFrames--;
    for (int i = 0; i < gNumBumpers; ++i)
        if (bumpers[i].flashFrames > 0) bumpers[i].flashFrames--;
    if (flash > 0.05f) flash *= 0.85f; else flash = 0.0f;

    if (aliveTargets() == 0) {
        levelNum++;
        seedLevel(levelNum);
        flash = 1.0f;
        score += 200;
    }

    bool any = false;
    for (int i = 0; i < MAX_BALLS; ++i) if (balls[i].on) { any = true; break; }
    if (!any) {
        if (ballsRemaining > 1) {
            ballsRemaining--;
            combo = 0;
            resetBall(balls[0]);
        } else {
            saveHighScore(STATE_PLAYING, score);
            gameOverMode = STATE_PLAYING;
            appState = STATE_GAMEOVER;
        }
    }
}

// ============================================================================
//  INTRO SCREEN
//
//  Parallax orbs roll around the screen behind the title. The three title
//  lines are pre-rendered bitmap sprites (see tools/gen_fonts.py) that
//  slide in with a stacked-shadow parallax effect:
//
//    - Back shadow layer (deepest teal) lags behind the most
//    - Mid layer (bright teal) lags slightly
//    - Main layer (purple #5D005D) lands on target
//
//  Each line slides in with its own delay so the screen reveals
//  cinematically rather than landing all at once.
// ============================================================================

// -- Easing helpers --
static float easeOutCubic(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    float u = 1 - t;
    return 1 - u * u * u;
}
static float easeOutBack(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// Blit a 1-bit packed bitmap onto the framebuffer in a single color.
// (Bit format matches what gen_fonts.py emits: MSB-first, row-major.)
static void blitBitmap(const IntroBitmap &b, int x, int y, uint16_t color) {
    const uint8_t* d = b.data;
    int stride = b.stride;
    for (int by = 0; by < b.height; ++by) {
        int dy = y + by;
        if (dy < 0 || dy >= H) continue;
        const uint8_t* row = d + by * stride;
        for (int bx = 0; bx < b.width; ++bx) {
            int dx = x + bx;
            if (dx < 0 || dx >= W) continue;
            uint8_t bits = row[bx >> 3];
            if (bits & (0x80 >> (bx & 7))) {
                fb.drawPixel(dx, dy, color);
            }
        }
    }
}

// Stacked-shadow blit: three offset blits in dark/mid/main colors. Gives
// the plain bitmap a chunky 3D feel like screen-printed type.
static void blitStacked(const IntroBitmap &b, int x, int y,
                        uint16_t shadow, uint16_t mid, uint16_t main) {
    blitBitmap(b, x + 2, y + 2, shadow);
    blitBitmap(b, x + 1, y + 1, mid);
    blitBitmap(b, x,     y,     main);
}

// One parallax orb. Different layers move at different speeds so the
// background has depth.
struct IntroOrb {
    float baseX, baseY;
    float radiusX, radiusY;
    float angSpeed;
    float angOffset;
    int   size;
    uint16_t color;
};
static const IntroOrb INTRO_ORBS[] = {
    // Background layer (slow, big, dim)
    {  85.0f, 240.0f,  60.0f, 30.0f, 0.008f, 0.00f, 8, TH_PURPLE_DARK },
    {  85.0f, 280.0f,  70.0f, 20.0f, 0.011f, 2.10f, 7, TH_TEAL_DARK   },
    // Mid layer
    {  60.0f,  60.0f,  35.0f, 18.0f, 0.018f, 1.20f, 5, TH_PURPLE      },
    { 120.0f, 220.0f,  40.0f, 25.0f, 0.020f, 3.50f, 5, TH_LIME_DARK   },
    // Front layer (faster, small, brighter)
    {  90.0f, 200.0f,  55.0f, 35.0f, 0.030f, 0.80f, 3, TH_LIME_MED    },
    {  85.0f, 260.0f,  45.0f, 20.0f, 0.035f, 4.20f, 3, TH_PURPLE_LIGHT},
};
static const int INTRO_ORB_COUNT = sizeof(INTRO_ORBS) / sizeof(INTRO_ORBS[0]);

static void drawIntro(int frame) {
    fb.fillSprite(C_INTRO_BG);                       // solid #007373

    // ---------- Parallax orbs rolling around the background ----------
    for (int i = 0; i < INTRO_ORB_COUNT; ++i) {
        const IntroOrb &o = INTRO_ORBS[i];
        float a = (float)frame * o.angSpeed + o.angOffset;
        int ox = (int)(o.baseX + cosf(a) * o.radiusX);
        int oy = (int)(o.baseY + sinf(a) * o.radiusY);
        // Trail dot one step behind for a subtle motion smear
        int tx = (int)(o.baseX + cosf(a - 0.15f) * o.radiusX);
        int ty = (int)(o.baseY + sinf(a - 0.15f) * o.radiusY);
        fb.fillCircle(tx, ty, o.size > 4 ? o.size - 2 : 1, TEAL_DARK);
        fb.fillCircle(ox, oy, o.size, thc(o.color));
    }

    // ---------- Soft horizontal band behind the title block ----------
    fb.fillRect(0, 90, W, 92, TEAL_DARK);
    fb.drawFastHLine(0,  90,    W, TEAL);
    fb.drawFastHLine(0,  90 + 91, W, TEAL);

    // ---------- Title lines: parallax bitmap slide-ins ----------
    //
    // Each line has a start frame and duration. The MAIN purple layer eases
    // straight to its target. The MID and BACK layers lag behind in time
    // (so they appear OFFSET from the main layer mid-flight, then snap into
    // their stacked-shadow position when the main layer settles). That's
    // the parallax effect - the deeper a layer is, the more it drags.

    auto layeredSlide = [&](const IntroBitmap &b, int targetX, int targetY,
                            int startFrame, int duration,
                            int slideFromX, bool overshoot) {
        float t      = clampf((float)(frame - startFrame) / (float)duration,
                              0.0f, 1.0f);
        float tBack  = clampf((float)(frame - startFrame - 6) / (float)duration,
                              0.0f, 1.0f);
        float tMid   = clampf((float)(frame - startFrame - 3) / (float)duration,
                              0.0f, 1.0f);
        float eMain  = overshoot ? easeOutBack(t)    : easeOutCubic(t);
        float eMid   = overshoot ? easeOutBack(tMid) : easeOutCubic(tMid);
        float eBack  = overshoot ? easeOutBack(tBack): easeOutCubic(tBack);
        int xMain = (int)(slideFromX + (targetX - slideFromX) * eMain);
        int xMid  = (int)(slideFromX + (targetX - slideFromX) * eMid);
        int xBack = (int)(slideFromX + (targetX - slideFromX) * eBack);
        // Draw deepest layer first
        blitBitmap(b, xBack + 2, targetY + 2, TEAL_DEEP);
        blitBitmap(b, xMid  + 1, targetY + 1, TEAL_BRIGHT);
        blitBitmap(b, xMain,     targetY,     C_INTRO_MAIN);
    };

    // KYLER - drops from above (use Y motion instead of X)
    {
        const IntroBitmap &b = INTRO_BMP_KYLER;
        int targetX = (W - b.width) / 2;
        int targetY = 100;
        float t      = clampf((float)(frame - 4)  / 18.0f, 0.0f, 1.0f);
        float tMid   = clampf((float)(frame - 7)  / 18.0f, 0.0f, 1.0f);
        float tBack  = clampf((float)(frame - 10) / 18.0f, 0.0f, 1.0f);
        float eMain = easeOutBack(t);
        float eMid  = easeOutCubic(tMid);
        float eBack = easeOutCubic(tBack);
        int yMain = (int)(-b.height + (targetY + b.height) * eMain);
        int yMid  = (int)(-b.height + (targetY + b.height) * eMid);
        int yBack = (int)(-b.height + (targetY + b.height) * eBack);
        blitBitmap(b, targetX + 2, yBack + 2, TEAL_DEEP);
        blitBitmap(b, targetX + 1, yMid  + 1, TEAL_BRIGHT);
        blitBitmap(b, targetX,     yMain,     C_INTRO_MAIN);
    }

    // MICHAELSON - slides in from the right
    {
        const IntroBitmap &b = INTRO_BMP_MICHAELSON;
        int targetX = (W - b.width) / 2;
        int targetY = 138;
        layeredSlide(b, targetX, targetY,
                     /*startFrame=*/14, /*duration=*/18,
                     /*from=*/W + 30, /*overshoot=*/false);
    }

    // STUDIOS - slides in from the left, lands inside a lime ribbon
    {
        const IntroBitmap &b = INTRO_BMP_STUDIOS;
        int targetX = (W - b.width) / 2;
        int targetY = 162;
        // Lime ribbon underneath - wipes in alongside the text
        float ribT = clampf((float)(frame - 22) / 18.0f, 0.0f, 1.0f);
        float ribE = easeOutCubic(ribT);
        int wipeW = (int)(W * ribE);
        int ribY = targetY - 2;
        int ribH = b.height + 5;
        if (wipeW > 0) {
            fb.fillRect(0, ribY, wipeW, ribH, LIME);
            fb.drawFastHLine(0, ribY,           wipeW, LIME_PALE);
            fb.drawFastHLine(0, ribY + ribH - 1, wipeW, LIME_DARK);
        }
        // Letter slides in from left, with stacked shadow for parallax depth
        layeredSlide(b, targetX, targetY,
                     /*startFrame=*/22, /*duration=*/18,
                     /*from=*/-b.width - 20, /*overshoot=*/false);
    }

    // ---------- Bottom URL & "PRESS TO PLAY" ----------
    if (frame > 42) {
        int pulse = (frame >> 4) & 1;
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(pulse ? LIME : LIME_MED);
        fb.drawString("PRESS  TO  PLAY", W / 2, H - 36, 2);
        fb.setTextColor(LIME);
        fb.drawString(INTRO_URL, W / 2, H - 12, 1);
    }

    // ---------- Corner ticks ----------
    fb.drawFastHLine(0, 0, 14, LIME);
    fb.drawFastVLine(0, 0, 14, LIME);
    fb.drawFastHLine(W - 14, 0, 14, LIME);
    fb.drawFastVLine(W - 1,  0, 14, LIME);
    fb.drawFastHLine(0, H - 1, 14, LIME);
    fb.drawFastVLine(0, H - 14, 14, LIME);
    fb.drawFastHLine(W - 14, H - 1, 14, LIME);
    fb.drawFastVLine(W - 1,  H - 14, 14, LIME);

    fb.pushSprite(0, 0);
}

static void tickIntro() {
    introFrame++;
    if (introFrame > 42 && justPressed()) {
        appState = STATE_PLAYING;
        resetGame();
    }
}

// ---------------- DRAW ----------------
static void drawBall(const Ball &b) {
    for (int i = TRAIL_LEN - 1; i > 0; --i) {
        int sz = (i < 3) ? 2 : 1;
        fb.fillCircle((int) b.trailX[i], (int) b.trailY[i], sz, C_TRAIL);
    }
    fb.fillCircle((int) b.x, (int) b.y, (int)(BALL_R + 1), C_BALL);
    fb.fillCircle((int) b.x, (int) b.y, (int) BALL_R,      C_BALL_CORE);
}

static void drawFlipper(const Flipper &f) {
    float tipX, tipY;
    flipperEndpoints(f, tipX, tipY);

    float dx = tipX - f.pivotX, dy = tipY - f.pivotY;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;
    float nx = -dy / len, ny = dx / len;
    float t  = FLIP_HALF_THICK + 1.0f;

    int x0 = (int)(f.pivotX + nx * t), y0 = (int)(f.pivotY + ny * t);
    int x1 = (int)(f.pivotX - nx * t), y1 = (int)(f.pivotY - ny * t);
    int x2 = (int)(tipX     - nx * t), y2 = (int)(tipY     - ny * t);
    int x3 = (int)(tipX     + nx * t), y3 = (int)(tipY     + ny * t);

    uint16_t col = (powerFlipFrames > 0) ? C_FLIPPER_PWR
                 : (f.active ? C_FLIPPER : C_FLIPPER_OFF);
    fb.fillTriangle(x0, y0, x1, y1, x2, y2, col);
    fb.fillTriangle(x0, y0, x2, y2, x3, y3, col);
    fb.drawLine(x0, y0, x3, y3, LIME_PALE);
    fb.drawLine(x1, y1, x2, y2, LIME_PALE);
    fb.fillCircle((int)f.pivotX, (int)f.pivotY, (int)FLIP_HALF_THICK + 1, col);
    fb.fillCircle((int)tipX,     (int)tipY,     (int)FLIP_HALF_THICK + 1, col);
    fb.fillCircle((int)f.pivotX, (int)f.pivotY, 2, TEAL_BRIGHT);
}

static void composeScene() {
    fb.fillSprite(flash > 0.4f ? C_BG_FLASH : C_BG);

    fb.drawFastHLine(0, 0,     W, C_WALL);
    fb.drawFastVLine(0, 0,     H, C_WALL);
    fb.drawFastVLine(W - 1, 0, H, C_WALL);

    fb.drawLine((int)walls[3].x1, (int)walls[3].y1,
                (int)walls[3].x2, (int)walls[3].y2, C_WALL_HI);
    fb.drawLine((int)walls[4].x1, (int)walls[4].y1,
                (int)walls[4].x2, (int)walls[4].y2, C_WALL_HI);
    fb.drawLine((int)walls[3].x1 + 1, (int)walls[3].y1,
                (int)walls[3].x2,     (int)walls[3].y2 - 1, C_FUNNEL);
    fb.drawLine((int)walls[4].x1 - 1, (int)walls[4].y1,
                (int)walls[4].x2,     (int)walls[4].y2 - 1, C_FUNNEL);

    int gapL = (int)(W * 0.5f - DRAIN_HALF_GAP);
    int gapR = (int)(W * 0.5f + DRAIN_HALF_GAP);
    for (int x = gapL; x <= gapR; x += 2) fb.drawPixel(x, H - 3, PURPLE_DARK);

    for (int i = 0; i < gNumBumpers; ++i) {
        const Bumper &m = bumpers[i];
        uint16_t inner = (m.flashFrames > 0) ? C_BUMPER_HIT : C_BUMPER;
        fb.fillCircle((int)m.x, (int)m.y, (int)m.r,           inner);
        fb.drawCircle((int)m.x, (int)m.y, (int)m.r,           C_BUMPER_RIM);
        fb.drawCircle((int)m.x, (int)m.y, (int)m.r + 2,       C_BUMPER_RIM);
        fb.fillCircle((int)m.x, (int)m.y, 2, LIME_PALE);
    }

    for (int i = 0; i < gNumTargets; ++i) {
        const Target &t = targets[i];
        if (!t.on) continue;
        uint16_t col;
        if (t.flashFrames > 0)            col = LIME_PALE;
        else if (t.kind == 2)             col = C_TARGET_EXP;
        else if (t.kind == 1)             col = C_TARGET_HVY;
        else                              col = C_TARGET;
        fb.fillRect((int)t.x - 4, (int)t.y - 4, 9, 9, col);
        fb.drawRect((int)t.x - 4, (int)t.y - 4, 9, 9, C_TARGET_HI);
        if (t.kind == 1 && t.hp > 1)
            fb.drawRect((int)t.x - 5, (int)t.y - 5, 11, 11, PURPLE_LIGHT);
    }

    for (int i = 0; i < MAX_POWERUPS; ++i) {
        const Powerup &p = powerups[i];
        if (!p.on) continue;
        if (p.life < 80 && ((p.life >> 3) & 1)) continue;
        uint16_t col;
        char     ch;
        switch (p.kind) {
            case 0: col = C_PWR_SLOW; ch = 'S'; break;
            case 1: col = C_PWR_MULT; ch = 'M'; break;
            case 2: col = C_PWR_BST;  ch = 'B'; break;
            case 3: col = C_PWR_PROJ; ch = 'P'; break;
            default: col = C_PWR_FLIP; ch = 'F'; break;
        }
        fb.fillCircle((int)p.x, (int)p.y, 5, col);
        fb.drawCircle((int)p.x, (int)p.y, 6, LIME_PALE);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(C_BG);
        fb.drawChar(ch, (int)p.x - 3, (int)p.y - 3, 1);
    }

    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        const Projectile &p = projs[i];
        if (!p.on) continue;
        fb.drawLine((int)p.prevX, (int)p.prevY,
                    (int)p.x,     (int)p.y, C_PROJ_TRAIL);
        fb.fillCircle((int)p.x, (int)p.y, 1, C_PROJ);
    }

    for (int i = 0; i < MAX_BALLS; ++i)
        if (balls[i].on) drawBall(balls[i]);

    drawFlipper(flipL);
    drawFlipper(flipR);

    char buf[24];
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(C_HUD, C_BG);
    fb.drawNumber((long)score, 4, 3, 2);

    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(C_HUD_DIM, C_BG);
    snprintf(buf, sizeof(buf), "BALLS %u", (unsigned)ballsRemaining);
    fb.drawString(buf, W - 32, 3, 1);

    fb.setTextDatum(TC_DATUM);
    snprintf(buf, sizeof(buf), "L%u", (unsigned)levelNum);
    fb.drawString(buf, W / 2, 3, 1);

    drawBatteryBar(W - 28, 17);

    if (combo > 1) {
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(C_HUD, C_BG);
        snprintf(buf, sizeof(buf), "x%u", (unsigned)combo);
        fb.drawString(buf, W / 2, 18, 2);
    }

    if (powerFlipFrames > 0) {
        int barW = (powerFlipFrames * (W - 20)) / PWR_FLIP_FRAMES;
        fb.drawRect(10, 30, W - 20, 3, LIME_DARK);
        fb.fillRect(10, 30, barW,  3, LIME);
    }

    if (bothHeldSince) {
        uint32_t held = millis() - bothHeldSince;
        if (held > 500) {
            int barW = (int)(((float) held / SLEEP_HOLD_MS) * (W - 40));
            if (barW > W - 40) barW = W - 40;
            fb.drawRect(20, H - 6, W - 40, 3, C_WALL);
            fb.fillRect(20, H - 6, barW,  3, C_WARN);
        }
    }
}

static void drawScene() {
    composeScene();
    fb.pushSprite(0, 0);
}

// Forward declarations of compose functions for the new game modes.
// We need these before drawGameOver because game-over composes whichever
// playfield the player just lost on.
static void composePocketScene();
static void composeSlotsScene();

static void drawGameOver() {
    // Compose the playfield of the game that ended, so the user sees what
    // they were playing dimmed behind the GAME OVER overlay.
    switch (gameOverMode) {
        case STATE_POCKET: composePocketScene(); break;
        case STATE_SLOTS:  composeSlotsScene();  break;
        default:           composeScene();       break;
    }
    // Dim
    for (int y = 30; y < H - 30; y += 2) fb.drawFastHLine(0, y, W, C_BG);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(C_HUD);
    fb.drawString("GAME OVER", W / 2, H / 2 - 30, 4);
    fb.setTextColor(C_HUD_DIM);
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)score);
    fb.drawString(buf, W / 2, H / 2 - 6, 4);
    // Show high score for the game mode that ended
    uint32_t hi = (gameOverMode == STATE_POCKET) ? hiScorePocket :
                  (gameOverMode == STATE_SLOTS)  ? hiScoreSlots  :
                                                   hiScorePinball;
    bool newRecord = (score > 0 && score >= hi);
    if (newRecord) {
        fb.setTextColor(LIME);
        fb.drawString("NEW RECORD!", W / 2, H / 2 + 18, 2);
    } else {
        fb.setTextColor(TEAL);
        char hibuf[24]; snprintf(hibuf, sizeof(hibuf), "BEST %lu", (unsigned long)hi);
        fb.drawString(hibuf, W / 2, H / 2 + 18, 2);
    }
    fb.setTextColor(C_HUD);
    fb.drawString("press to restart", W / 2, H / 2 + 36, 2);
    fb.pushSprite(0, 0);
}

// ============================================================================
//  BATTERY MONITOR
//
//  T-Display-S3 routes the battery through a 1:2 voltage divider to GPIO 4.
//  We sample at 12-bit resolution once per second and convert to volts:
//      V = adc_raw / 4095 * 3.3 * 2
//
//  Three-segment color-coded bar:
//    >= 3.85V  three lime cells
//    >= 3.60V  two lime cells (third is dim)
//    >= 3.30V  one purple cell (others empty)
//    <  3.20V  blink purple briefly, then deep-sleep to protect the LiPo
// ============================================================================

static void batterySample() {
    int raw = analogRead(PIN_BAT_ADC);
    float v = (float)raw / 4095.0f * 3.3f * 2.0f;
    // Rolling average to smooth ADC noise
    batteryV = batteryV * 0.7f + v * 0.3f;
}

static void tickBattery() {
    uint32_t now = millis();
    if (now - batteryLastSample > 1000) {
        batteryLastSample = now;
        batterySample();
        if (batteryV > 2.5f && batteryV < BAT_CUTOFF) {
            // Critical - shut down to protect the cell
            goToSleep();
        }
    }
}

// Draw the 3-segment battery bar at (x, y). Each segment 6 wide, 7 tall.
static void drawBatteryBar(int x, int y) {
    int filled;
    if      (batteryV >= BAT_FULL) filled = 3;
    else if (batteryV >= BAT_MID)  filled = 2;
    else if (batteryV >= BAT_LOW)  filled = 1;
    else                           filled = 0;

    // Tip cap on the right (battery icon nub)
    fb.drawRect(x, y, 20, 7, C_HUD_DIM);
    fb.fillRect(x + 20, y + 2, 2, 3, C_HUD_DIM);

    // Three cells inside
    bool blinkLow = (filled == 0) && (((millis() / 250) & 1) == 0);
    for (int i = 0; i < 3; ++i) {
        int cx = x + 1 + i * 6;
        if (i < filled) {
            uint16_t col;
            if (filled == 3) col = LIME;
            else if (filled == 2) col = (i < 2) ? LIME : C_BG;
            else col = PURPLE;
            fb.fillRect(cx + 1, y + 1, 5, 5, col);
        } else if (filled == 0 && blinkLow && i == 0) {
            fb.fillRect(cx + 1, y + 1, 5, 5, PURPLE);
        }
    }
}


// ============================================================================
//  DOUBLE-CLICK DETECTION (both buttons)
//
//  Fires when the player presses both buttons, releases both, then presses
//  both again - all within DC_WINDOW_MS. Used to open the game-select menu.
// ============================================================================

static void tickDoubleClick() {
    dcFiredThisFrame = false;
    bool both     = btnL && btnR;
    bool both_prev = btnL_prev && btnR_prev;
    bool justBoth     = both && !both_prev;
    bool justReleased = !both && both_prev;
    uint32_t now = millis();

    // Timeout the state machine
    if (dcState != DC_IDLE && now - dcStateAt > DC_WINDOW_MS) {
        dcState = DC_IDLE;
    }

    switch (dcState) {
        case DC_IDLE:
            if (justBoth) { dcState = DC_FIRST_PRESS; dcStateAt = now; }
            break;
        case DC_FIRST_PRESS:
            if (justReleased) { dcState = DC_FIRST_RELEASE; dcStateAt = now; }
            break;
        case DC_FIRST_RELEASE:
            if (justBoth) {
                dcFiredThisFrame = true;
                dcState = DC_IDLE;
            }
            break;
    }
}

// ── Network yield: keep the both-button double-click alive during blocking
// fetches. Several dashboard apps (Flights, Candles, Rabbit Hole, Weather...)
// do multi-second synchronous HTTP work inside their tick; while that runs,
// loop() never samples the buttons, so the 400ms double-click window either
// times out mid-sequence or misses the edges entirely - the user is trapped.
//
// The fix: those wait loops call sysNetYield(), which runs the SAME
// readInput() + tickDoubleClick() at full rate (self-throttled to ~3ms). A
// detected double-click is LATCHED here; the fetch loop sees sysNetAborted()
// and bails early, and loop() then routes to the menu exactly as if the
// double-click had landed on a normal frame. Pinball is untouched - its tick
// never blocks, so its flipper timing and the DC window are byte-identical.
static bool dcLatchedFromYield = false;
void sysNetYield() {
    static uint32_t lastYield = 0;
    uint32_t now = millis();
    if (now - lastYield < 3) return;
    lastYield = now;
    readInput();
    tickDoubleClick();
    if (dcFiredThisFrame) dcLatchedFromYield = true;
}
bool sysNetAborted() { return dcLatchedFromYield; }


// ============================================================================
//  MENU SCREEN
//
//  Game-select overlay. Triggered by double-click both buttons.
//  Navigation:  LEFT button = step through options
//               RIGHT button = select highlighted option
//  PINBALL is always the first/default option.
// ============================================================================

static void resetGame();         // forward
static void resetPocket();
static void resetSlots();

static void enterMenu() {
    menuReturnTo = appState;
    appState = STATE_MENU;
    // Cheap (~1 ms) I2C probe for the gesture sensor, then rebuild the
    // filtered menu so hardware-gated items track what's actually plugged in.
    fruitProbeSensor();
    rebuildMenu();
    menuIndex = 0;
    menuEnteredAt = millis();
}

static void tickMenu() {
    // Cooldown after opening - prevents the same press that opened the
    // menu (or a quick follow-up tap) from instantly selecting an item.
    if (millis() - menuEnteredAt < MENU_COOLDOWN_MS) return;

    // LEFT steps, RIGHT selects. Use just-pressed edges so a held button
    // doesn't auto-scroll.
    bool pressedL = btnL && !btnL_prev;
    bool pressedR = btnR && !btnR_prev;

    if (pressedL) {
        menuIndex = (menuIndex + 1) % menuCount;
    }
    if (pressedR) {
        g_entryLatch = true;     // the app must not see this press - see appInputGuardBegin()
        AppState target = menuItems[menuIndex].target;
        switch (target) {
            case STATE_PLAYING: resetGame();   break;
            case STATE_POCKET:  resetPocket(); break;
            case STATE_SLOTS:   resetSlots();  break;
            case STATE_MINER:   minerEnter();   break;
            case STATE_WEATHER: weatherEnter(); break;
            case STATE_SPACE:   spaceEnter();    break;
            case STATE_RABBIT: rabbitEnter();  break;
            case STATE_APOD:    apodEnter();    break;
            case STATE_FLIGHT:  flightEnter();  break;
            case STATE_CANDLES: candlesEnter(); break;
            case STATE_VERSUS:  versusEnter();  break;
            case STATE_SCOUT:    scoutEnter();    break;
            case STATE_SURVIVAL: survivalEnter(); break;
            case STATE_FIRSTAID: firstaidEnter(); break;
            case STATE_GUIDEBOOKS: guidebooksEnter(); break;
            case STATE_CYOA:     cyoaEnter();     break;
            case STATE_QUESTS:   questsEnter();   break;
            case STATE_SHARE:    shareEnter();    break;
            case STATE_PET:      petEnter();      break;
            case STATE_MOON:     moonEnter();     break;
            case STATE_NEO:      neoEnter();      break;
            case STATE_CLICKER:  clickerEnter();  break;
            case STATE_DICE:     diceEnter();     break;
            case STATE_AQUARIUM: aquariumEnter(); break;
            case STATE_FRUIT:    fruitEnter();    break;
            case STATE_ROADHUNT: roadhuntEnter(); break;
            case STATE_SIGHTINGS: sightingsEnter(); break;
            case STATE_MANTISCANNON: mantiscannonEnter(); break;
            case STATE_MANTISSKATE: mantisskateEnter(); break;
            case STATE_MANTISFISH: mantisfishEnter(); break;
            case STATE_DUNERIDER: duneriderEnter(); break;
            case STATE_MANTISPUTT: mantisputtEnter(); break;
            case STATE_MANTISDROP: mantisdropEnter(); break;
            case STATE_MANTISVADERS: mantisvadersEnter(); break;
            case STATE_MANTISPSYCHIC: mantisPsychicEnter(); break;
            case STATE_RUBIKS: rubiksEnter(); break;
            case STATE_FIREWORKS: fireworksEnter(); break;
            case STATE_MANTISCHAT: mantischatEnter(); break;
            case STATE_DANCEPARTY: dancepartyEnter(); break;
            case STATE_HAM:      hamEnter();      break;
            case STATE_PARTY:    partyHubEnter();  break;
            case STATE_SETUP:    setupModeEnter(); break;
            default: break;
        }
        appState = target;
    }
}

static void drawMenu() {
    fb.fillSprite(C_BG);
    fb.drawRect(0, 0, W, H, C_WALL);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(C_INTRO_MAIN);
    fb.drawString("SELECT MODE", W / 2, 26, 4);

    fb.setTextColor(C_HUD_DIM);
    fb.drawString("LEFT step   RIGHT select", W / 2, 50, 1);

    // --- Scrolling list window: always keep the selected item visible ---
    // The list area runs from listTop_y to just above the bottom hint.
    const int areaTop = 66;
    const int areaBot = H - 22;
    const int rowH    = 26;
    const int visible = (areaBot - areaTop) / rowH;   // rows that fit

    // Compute the first visible index so menuIndex stays on screen, and
    // bias toward centering the selection when there's room to scroll.
    int firstVis = menuIndex - visible / 2;
    if (firstVis < 0) firstVis = 0;
    if (firstVis > menuCount - visible) firstVis = menuCount - visible;
    if (firstVis < 0) firstVis = 0;   // fewer items than the window

    int rowY = areaTop + rowH / 2;
    for (int i = firstVis; i < menuCount && i < firstVis + visible; ++i) {
        bool sel = (i == menuIndex);
        if (sel) {
            fb.fillRect(8, rowY - 12, W - 16, 24, LIME_DARK);
            fb.drawRect(8, rowY - 12, W - 16, 24, LIME);
            fb.fillTriangle(14, rowY - 5, 14, rowY + 5, 19, rowY, LIME);
            fb.fillTriangle(W - 14, rowY - 5, W - 14, rowY + 5, W - 19, rowY, LIME);
        }
        fb.setTextColor(sel ? LIME_PALE : C_HUD_DIM);
        fb.setTextDatum(MC_DATUM);
        fb.drawString(menuItems[i].name, W / 2, rowY, 2);
        rowY += rowH;
    }

    // --- Scroll indicators: up/down chevrons + position bar ---
    if (firstVis > 0) {
        fb.fillTriangle(W / 2 - 5, areaTop - 4, W / 2 + 5, areaTop - 4,
                        W / 2, areaTop - 9, C_HUD_DIM);
    }
    if (firstVis + visible < menuCount) {
        fb.fillTriangle(W / 2 - 5, areaBot + 4, W / 2 + 5, areaBot + 4,
                        W / 2, areaBot + 9, C_HUD_DIM);
    }
    if (menuCount > visible) {
        int trackH = areaBot - areaTop;
        int barH = trackH * visible / menuCount;
        if (barH < 8) barH = 8;
        int barY = areaTop + (trackH - barH) * menuIndex /
                   (menuCount > 1 ? (menuCount - 1) : 1);
        fb.fillRect(W - 4, barY, 2, barH, LIME);
    }

    // Position counter (e.g. "3 / 18")
    char pos[12];
    snprintf(pos, sizeof(pos), "%d / %d", menuIndex + 1, menuCount);
    fb.setTextColor(C_HUD_DIM);
    fb.setTextDatum(MC_DATUM);
    fb.drawString(pos, W / 2, H - 12, 1);

    // Battery + corner ticks
    drawBatteryBar(W - 28, 5);
    fb.drawFastHLine(0, 0, 14, LIME);
    fb.drawFastVLine(0, 0, 14, LIME);
    fb.drawFastHLine(W - 14, H - 1, 14, LIME);
    fb.drawFastVLine(W - 1,  H - 14, 14, LIME);

    fb.pushSprite(0, 0);
}


// ============================================================================
//  COMMON HELPERS for POCKET / SLOTS
// ============================================================================

// Drop a "cue" ball above a flipper as the launch animation. Side: 0=L, 1=R.
static void cueDropOnFlipper(Ball &b, int side) {
    if (side == 0) {
        b.x  = FLIP_L_PX + 10.0f;
        b.y  = FLIP_Y - 60.0f;
        b.vx = 0.5f;
    } else {
        b.x  = FLIP_R_PX - 10.0f;
        b.y  = FLIP_Y - 60.0f;
        b.vx = -0.5f;
    }
    b.vy = 1.5f;       // gentle gravity-assisted drop onto the flipper
    b.on = true;
    for (int i = 0; i < TRAIL_LEN; ++i) {
        b.trailX[i] = b.x;
        b.trailY[i] = b.y;
    }
}


// ============================================================================
//  CORNER POCKET
//
//  Same flippers, walls, drain, and physics as PINBALL. The cue ball plays
//  exactly like the pinball ball. The 6 pool balls live "on the table" -
//  no gravity (gravity is in the Z axis, as if the table were flat), just
//  friction and reduced bounciness.
//
//  Two corner pockets at the top of the screen catch any ball (cue or pool)
//  that reaches them. Bottom drain: cue ball lost = lose a life; pool ball
//  drained = counts as pocketed.
//
//  When all 6 pool balls are gone the rack is automatically reset and a
//  fresh cue is dropped in - score keeps rolling, racks-cleared counter
//  goes up.
// ============================================================================

static constexpr float POCKET_R   = 14.0f;          // catch radius at each top corner
static constexpr float POCKET_LX  = 0.0f;
static constexpr float POCKET_RX  = (float)W;
static constexpr float POCKET_Y   = 0.0f;

static void pocketRackBalls() {
    // 6 balls in a triangle: rows of 1, 2, 3 from top.
    // Centered horizontally at W/2, top row at y=72, spaced just over 2*POOL_R.
    float cx = (float)W * 0.5f;
    float topY = 70.0f;
    float spacing = POOL_R * 2.2f;
    int idx = 0;
    for (int row = 0; row < 3; ++row) {
        int n = row + 1;
        float y = topY + row * spacing * 0.92f;     // tighter vertical for triangle
        for (int c = 0; c < n; ++c) {
            float x = cx + ((float)c - (n - 1) * 0.5f) * spacing;
            poolBalls[idx].x  = x;
            poolBalls[idx].y  = y;
            poolBalls[idx].vx = 0;
            poolBalls[idx].vy = 0;
            poolBalls[idx].on = true;
            idx++;
        }
    }
    while (idx < POOL_COUNT) poolBalls[idx++].on = false;
}

static bool pocketChalkArmed = false;   // "Chalk Up": final score x2 this game

static void resetPocket() {
    score = 0;
    combo = 0;
    ballsRemaining = 3;
    // Armed Quest-inventory items land here, at the start of the next game.
    uint16_t lb = qpTakePending(ITEM_PK_LUCKYBREAK);
    if (lb) { ballsRemaining += lb; if (ballsRemaining > 6) ballsRemaining = 6; }
    pocketChalkArmed = qpTakePending(ITEM_PK_CHALK) > 0;
    pocketRacks = 0;
    powerFlipFrames = 0;
    pocketLastDropSide = 1;
    // Wipe any leftover pinball entities so they can't phantom-collide
    gNumTargets = 0;
    gNumBumpers = 0;
    for (int i = 0; i < MAX_TARGETS; ++i) targets[i].on = false;
    for (int i = 0; i < MAX_POWERUPS;    ++i) powerups[i].on = false;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projs[i].on    = false;
    for (int i = 0; i < MAX_BALLS; ++i) balls[i].on = false;
    cueDropOnFlipper(balls[0], pocketLastDropSide);
    pocketRackBalls();
}

// Flipper-vs-pool-ball: pool balls bounce off the flipper segment with the
// same swept reflection as the cue, but no big tangential kick (they're
// heavy and the flipper just deflects them).
static void collidePoolFlipper(PoolBall &b, Flipper &f) {
    if (!b.on) return;
    float tipX, tipY;
    flipperEndpoints(f, tipX, tipY);
    float cx, cy;
    closestPointOnSegment(f.pivotX, f.pivotY, tipX, tipY, b.x, b.y, cx, cy);
    float pdx = b.x - cx, pdy = b.y - cy;
    float minDist = POOL_R + FLIP_HALF_THICK;
    float d2 = pdx*pdx + pdy*pdy;
    if (d2 >= minDist * minDist) return;
    float d = sqrtf(d2);
    float nx, ny;
    if (d > 0.001f) { nx = pdx / d; ny = pdy / d; }
    else            { nx = 0; ny = -1; }
    // Push out
    float pen = minDist - d;
    b.x += nx * pen;
    b.y += ny * pen;
    // Reflect with pool's softer restitution
    float vn = b.vx * nx + b.vy * ny;
    if (vn < 0) {
        b.vx -= (1 + POOL_WALL_REST) * vn * nx;
        b.vy -= (1 + POOL_WALL_REST) * vn * ny;
    }
    // Small kick from flipper motion (much smaller than cue gets)
    float omega = f.angle - f.prevAngle;
    float rx = cx - f.pivotX;
    float ry = cy - f.pivotY;
    b.vx += -ry * omega * 4.0f;
    b.vy +=  rx * omega * 4.0f;
}

// Pool ball stepping: no gravity, friction, walls less bouncy.
static void stepPoolBall(PoolBall &b) {
    if (!b.on) return;
    // Friction
    b.vx *= POOL_FRICTION;
    b.vy *= POOL_FRICTION;
    // Stop if very slow (avoids endless jitter)
    if (fabsf(b.vx) < 0.01f) b.vx = 0;
    if (fabsf(b.vy) < 0.01f) b.vy = 0;

    // Cap velocity
    const float MAX_POOL_V = 6.5f;
    if (b.vx >  MAX_POOL_V) b.vx =  MAX_POOL_V;
    if (b.vx < -MAX_POOL_V) b.vx = -MAX_POOL_V;
    if (b.vy >  MAX_POOL_V) b.vy =  MAX_POOL_V;
    if (b.vy < -MAX_POOL_V) b.vy = -MAX_POOL_V;

    b.x += b.vx;
    b.y += b.vy;

    // Collide with the same outer + funnel walls the cue uses, but with
    // lower restitution (less bouncy felt cushions).
    for (int i = 0; i < WALL_COUNT; ++i) {
        const Wall &w = walls[i];
        float dx = w.x2 - w.x1, dy = w.y2 - w.y1;
        if (dx*dx + dy*dy < 0.001f) continue;
        float cx, cy;
        closestPointOnSegment(w.x1, w.y1, w.x2, w.y2, b.x, b.y, cx, cy);
        float pdx = b.x - cx, pdy = b.y - cy;
        float d2 = pdx*pdx + pdy*pdy;
        if (d2 < POOL_R * POOL_R) {
            float d = sqrtf(d2);
            float nx, ny;
            if (d > 0.001f) { nx = pdx / d; ny = pdy / d; }
            else            { nx = w.nx;    ny = w.ny;    }
            float pen = POOL_R - d;
            b.x += nx * pen;
            b.y += ny * pen;
            float vn = b.vx * nx + b.vy * ny;
            if (vn < 0) {
                b.vx -= (1 + POOL_WALL_REST) * vn * nx;
                b.vy -= (1 + POOL_WALL_REST) * vn * ny;
            }
        }
    }

    // Top corner pockets - radial catch matching the visible black holes
    {
        float dxL = b.x - 0.0f,         dyL = b.y - 0.0f;
        float dxR = b.x - (float)(W-1), dyR = b.y - 0.0f;
        if (dxL*dxL + dyL*dyL < 11.0f * 11.0f ||
            dxR*dxR + dyR*dyR < 11.0f * 11.0f) {
            b.on = false;
            score += 100;
            flash = fmaxf(flash, 0.7f);
            return;
        }
    }
    // Bottom drain on a pool ball: counts as a corner-pocket per spec
    if (b.y > (float)H + 4.0f) {
        b.on = false;
        score += 100;
        flash = fmaxf(flash, 0.5f);
    }
}

// Elastic collision between two circular bodies.
// Masses m1 (cue / pool 1) and m2 (pool 2). Restitution e.
static void collideCirclePair(float &x1, float &y1, float &vx1, float &vy1, float r1, float m1,
                              float &x2, float &y2, float &vx2, float &vy2, float r2, float m2,
                              float e) {
    float dx = x2 - x1, dy = y2 - y1;
    float dist2 = dx*dx + dy*dy;
    float minD = r1 + r2;
    if (dist2 >= minD * minD) return;
    float dist = sqrtf(dist2);
    if (dist < 0.0001f) { dx = 1; dy = 0; dist = 1; }
    float nx = dx / dist, ny = dy / dist;
    // Separate so they don't keep penetrating
    float pen = (minD - dist) * 0.5f;
    x1 -= nx * pen; y1 -= ny * pen;
    x2 += nx * pen; y2 += ny * pen;
    // Relative velocity along normal
    float dvx = vx1 - vx2, dvy = vy1 - vy2;
    float vn  = dvx * nx + dvy * ny;
    if (vn <= 0) return;            // separating
    float J = (1 + e) * vn / (1.0f / m1 + 1.0f / m2);
    vx1 -= J * nx / m1;
    vy1 -= J * ny / m1;
    vx2 += J * nx / m2;
    vy2 += J * ny / m2;
}

static void pocketCollideAll() {
    // Cue vs each pool ball
    Ball &cue = balls[0];
    if (cue.on) {
        for (int i = 0; i < POOL_COUNT; ++i) {
            PoolBall &p = poolBalls[i];
            if (!p.on) continue;
            collideCirclePair(cue.x, cue.y, cue.vx, cue.vy, BALL_R, 1.0f,
                              p.x, p.y, p.vx, p.vy, POOL_R, POOL_MASS,
                              0.85f);     // cue/pool restitution
        }
    }
    // Pool vs pool (each pair once)
    for (int i = 0; i < POOL_COUNT; ++i) {
        if (!poolBalls[i].on) continue;
        for (int j = i + 1; j < POOL_COUNT; ++j) {
            if (!poolBalls[j].on) continue;
            PoolBall &a = poolBalls[i];
            PoolBall &b = poolBalls[j];
            collideCirclePair(a.x, a.y, a.vx, a.vy, POOL_R, POOL_MASS,
                              b.x, b.y, b.vx, b.vy, POOL_R, POOL_MASS,
                              0.65f);     // pool/pool less bouncy
        }
    }
}

static void tickPocket() {
    stepFlipper(flipL, btnL);
    stepFlipper(flipR, btnR);

    // Cue ball physics (uses all the existing pinball collisions)
    if (balls[0].on) stepBall(balls[0]);

    // Pool balls (with their own walls + flipper collisions)
    for (int i = 0; i < POOL_COUNT; ++i) {
        stepPoolBall(poolBalls[i]);
        collidePoolFlipper(poolBalls[i], flipL);
        collidePoolFlipper(poolBalls[i], flipR);
    }

    // Pairwise collisions
    pocketCollideAll();

    if (flash > 0.05f) flash *= 0.85f; else flash = 0.0f;

    // Cue corner-pocketed: -50, immediate respawn (no ball lost)
    if (balls[0].on) {
        float dxL = balls[0].x - 0.0f,         dyL = balls[0].y - 0.0f;
        float dxR = balls[0].x - (float)(W-1), dyR = balls[0].y - 0.0f;
        if (dxL*dxL + dyL*dyL < 11.0f * 11.0f ||
            dxR*dxR + dyR*dyR < 11.0f * 11.0f) {
            balls[0].on = false;
            int newScore = (int)score - 50;
            score = (newScore < 0) ? 0 : (uint32_t)newScore;
            cueDropOnFlipper(balls[0], pocketLastDropSide);
            flash = 0.8f;
        }
    }

    // Cue drained at the bottom: lose a life
    if (!balls[0].on && balls[0].y > (float)H) {
        if (ballsRemaining > 1) {
            ballsRemaining--;
            cueDropOnFlipper(balls[0], pocketLastDropSide);
        } else {
            if (pocketChalkArmed) { score *= 2; pocketChalkArmed = false; }  // Chalk Up
            saveHighScore(STATE_POCKET, score);
            gameOverMode = STATE_POCKET;
            appState = STATE_GAMEOVER;
            return;
        }
    }

    // All pool balls cleared -> rack again, keep cue
    bool anyPool = false;
    for (int i = 0; i < POOL_COUNT; ++i) if (poolBalls[i].on) { anyPool = true; break; }
    if (!anyPool) {
        pocketRacks++;
        score += 500;
        pocketRackBalls();
        // Auto-respawn cue if it's gone too
        if (!balls[0].on) cueDropOnFlipper(balls[0], pocketLastDropSide);
        flash = 1.0f;
    }
}

static void drawPoolBall(const PoolBall &p) {
    if (!p.on) return;
    fb.fillCircle((int)p.x, (int)p.y, (int)POOL_R, PURPLE);
    fb.drawCircle((int)p.x, (int)p.y, (int)POOL_R, PURPLE_LIGHT);
    // Highlight dot for billiard look
    fb.fillCircle((int)(p.x - 1), (int)(p.y - 1), 1, LIME_PALE);
}

static void drawCornerPockets() {
    // Two black circular pockets at the top corners. The black pixel value
    // (0x0000) is darker than any palette color, reading clearly as "hole".
    const int pr = 11;
    // Left pocket
    fb.fillCircle(0, 0, pr + 2, TEAL_DARK);            // soft surround
    fb.fillCircle(0, 0, pr,     0x0000);               // black hole
    fb.drawCircle(0, 0, pr,     LIME);                 // lime rim
    // Right pocket
    fb.fillCircle(W - 1, 0, pr + 2, TEAL_DARK);
    fb.fillCircle(W - 1, 0, pr,     0x0000);
    fb.drawCircle(W - 1, 0, pr,     LIME);
}

static void composePocketScene() {
    fb.fillSprite(flash > 0.4f ? C_BG_FLASH : C_BG);

    // Outer rails
    fb.drawFastHLine(0, 0,     W, C_WALL);
    fb.drawFastVLine(0, 0,     H, C_WALL);
    fb.drawFastVLine(W - 1, 0, H, C_WALL);

    // Funnel walls (same as pinball)
    fb.drawLine((int)walls[3].x1, (int)walls[3].y1,
                (int)walls[3].x2, (int)walls[3].y2, C_WALL_HI);
    fb.drawLine((int)walls[4].x1, (int)walls[4].y1,
                (int)walls[4].x2, (int)walls[4].y2, C_WALL_HI);

    // Drain marker at the bottom
    int gapL = (int)(W * 0.5f - DRAIN_HALF_GAP);
    int gapR = (int)(W * 0.5f + DRAIN_HALF_GAP);
    for (int x = gapL; x <= gapR; x += 2) fb.drawPixel(x, H - 3, PURPLE_DARK);

    // Top corner pockets
    drawCornerPockets();

    // Pool balls
    for (int i = 0; i < POOL_COUNT; ++i) drawPoolBall(poolBalls[i]);

    // Cue ball
    if (balls[0].on) drawBall(balls[0]);

    // Flippers
    drawFlipper(flipL);
    drawFlipper(flipR);

    // HUD
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(C_HUD, C_BG);
    fb.drawNumber((long)score, 4, 3, 2);

    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(C_HUD_DIM, C_BG);
    char buf[24];
    snprintf(buf, sizeof(buf), "BALLS %u", (unsigned)ballsRemaining);
    fb.drawString(buf, W - 32, 3, 1);

    fb.setTextDatum(TC_DATUM);
    snprintf(buf, sizeof(buf), "RACK %u", (unsigned)pocketRacks);
    fb.drawString(buf, W / 2, 3, 1);
    if (pocketChalkArmed) {
        fb.setTextColor(LIME, C_BG);
        fb.drawString("CHALK x2", W / 2, 13, 1);
    }

    drawBatteryBar(W - 28, 17);

    // Sleep-hold progress
    if (bothHeldSince) {
        uint32_t held = millis() - bothHeldSince;
        if (held > 500) {
            int barW = (int)(((float) held / SLEEP_HOLD_MS) * (W - 40));
            if (barW > W - 40) barW = W - 40;
            fb.drawRect(20, H - 6, W - 40, 3, C_WALL);
            fb.fillRect(20, H - 6, barW,  3, C_WARN);
        }
    }
}

static void drawPocketScene() {
    composePocketScene();
    fb.pushSprite(0, 0);
}


// ============================================================================
//  SLOTS
//
//  Same flippers, walls, drain, and physics as PINBALL for the cue ball.
//  No bumpers, no destructible targets. Instead:
//
//    - 5 reels in the middle of the table show video-poker cards
//    - 5 hold-toggle targets directly above each reel - hit one to mark
//      that reel as "held" so it doesn't reroll on the next spin
//    - 1 moving target slides left-right near the top - hitting it spins
//      the reels and settles the hand for scoring
//
//  The first deal is automatic when the ball is launched. The score for
//  that deal is NOT applied until the player triggers a real spin via
//  the moving target. Lose 3 balls and it's game over.
// ============================================================================

// ----- Card / hand evaluation -----
//
// 13 values, 2 suits. Hand = 5 cards (one per reel). Standard ranks but
// flush is easy with only 2 suits, so we keep the flush above two pair
// (per spec) but pay it modestly.
//
// Ranking values (higher = better):
//   9 ROYAL FLUSH   pay 1000
//   8 STR FLUSH     pay  500
//   7 FOUR KIND     pay  200
//   6 FULL HOUSE    pay  100
//   5 FLUSH         pay   50      <-- placed above TWO PAIR per spec
//   4 STRAIGHT      pay   30
//   3 THREE KIND    pay   20
//   2 TWO PAIR      pay   15
//   1 J/Q/K/A PAIR  pay   10
//   0 NOTHING       pay  -10
//
// Card values: 0=2, 1=3, ... 8=10, 9=J, 10=Q, 11=K, 12=A
//
struct HandResult {
    int rank;
    uint16_t pay;
    char name[16];
};

static HandResult evaluateHand(const Card cards[SLOT_REELS]) {
    HandResult r = {0, 0, "NO HAND"};

    // Tally counts per value and per suit
    int valCount[SLOT_VALUES] = {0};
    int suitCount[SLOT_SUITS] = {0};
    for (int i = 0; i < SLOT_REELS; ++i) {
        if (cards[i].value < 0) return r;     // un-dealt -> nothing
        valCount[cards[i].value]++;
        suitCount[cards[i].suit]++;
    }

    // Pair detection
    int pairs = 0, threes = 0, fours = 0;
    int highPairValue = -1;
    for (int v = 0; v < SLOT_VALUES; ++v) {
        if (valCount[v] == 4) fours++;
        else if (valCount[v] == 3) threes++;
        else if (valCount[v] == 2) {
            pairs++;
            if (v >= 9) highPairValue = v;    // J or higher
        }
    }

    // Flush?
    bool flush = (suitCount[0] == 5) || (suitCount[1] == 5);

    // Straight? Ace can be high (A-K-Q-J-10) OR low (A-2-3-4-5).
    bool straight = false;
    bool royalCandidate = false;
    {
        bool present[SLOT_VALUES] = {false};
        for (int i = 0; i < SLOT_REELS; ++i) present[cards[i].value] = true;
        // Look for 5 consecutive starting at v
        for (int v = 0; v <= SLOT_VALUES - 5; ++v) {
            bool run = true;
            for (int k = 0; k < 5; ++k) if (!present[v + k]) { run = false; break; }
            if (run) {
                straight = true;
                if (v == 8) royalCandidate = true;     // 10-J-Q-K-A
            }
        }
        // Wheel: A-2-3-4-5
        if (present[12] && present[0] && present[1] && present[2] && present[3]) {
            straight = true;
        }
    }

    if (flush && royalCandidate)        { r.rank=9; r.pay=1000; strncpy(r.name,"ROYAL!",16);    return r; }
    if (flush && straight)              { r.rank=8; r.pay=500;  strncpy(r.name,"STR FLUSH",16); return r; }
    if (fours)                          { r.rank=7; r.pay=200;  strncpy(r.name,"FOUR KIND",16); return r; }
    if (threes && pairs)                { r.rank=6; r.pay=100;  strncpy(r.name,"FULL HOUSE",16);return r; }
    if (flush)                          { r.rank=5; r.pay=50;   strncpy(r.name,"FLUSH",16);     return r; }
    if (straight)                       { r.rank=4; r.pay=30;   strncpy(r.name,"STRAIGHT",16);  return r; }
    if (threes)                         { r.rank=3; r.pay=20;   strncpy(r.name,"THREE KIND",16);return r; }
    if (pairs >= 2)                     { r.rank=2; r.pay=15;   strncpy(r.name,"TWO PAIR",16);  return r; }
    if (pairs == 1 && highPairValue >= 9){r.rank=1; r.pay=10;   strncpy(r.name,"JACKS+",16);    return r; }

    r.rank = 0; r.pay = 0; strncpy(r.name,"NO HAND",16);
    return r;
}

// Card geometry on the playfield
static constexpr int CARD_W = 24;
static constexpr int CARD_H = 30;
static constexpr int CARD_GAP = 4;
static constexpr int REEL_Y = 142;
static constexpr int HOLD_TARGET_Y = 110;
static constexpr int SPIN_TARGET_Y = 50;

static int reelX(int i) {
    int totalW = SLOT_REELS * CARD_W + (SLOT_REELS - 1) * CARD_GAP;
    int startX = (W - totalW) / 2;
    return startX + i * (CARD_W + CARD_GAP);
}

static void slotsResetReels() {
    for (int i = 0; i < SLOT_REELS; ++i) {
        reels[i].shown.value = -1;       // card-back
        reels[i].shown.suit  = 0;
        reels[i].held = false;
        reels[i].spinFrames = 0;
        reels[i].stopAt = 0;
    }
    for (int i = 0; i < SLOT_REELS; ++i) {
        holdTargets[i].x = reelX(i) + CARD_W / 2;
        holdTargets[i].y = HOLD_TARGET_Y;
        holdTargets[i].on = true;
        holdTargets[i].flashFrames = 0;
    }
    spinTarget.x = (float)W * 0.5f;
    spinTarget.y = (float)SPIN_TARGET_Y;
    spinTarget.vx = 1.4f;
    spinTarget.flashFrames = 0;

    slotsAwaitingFirstDeal = true;
    slotsHandSettled = false;
    slotsCurrentSpinIsFirstDeal = false;
    slotsLastWinFrames = 0;
    slotsLastWinPay = 0;
    slotsLastWinName[0] = '\0';
}

static void resetSlots() {
    score = 0;
    ballsRemaining = 3;
    pocketLastDropSide = 1;
    powerFlipFrames = 0;
    // Wipe any leftover pinball entities so they can't phantom-collide
    gNumTargets = 0;
    gNumBumpers = 0;
    for (int i = 0; i < MAX_TARGETS; ++i) targets[i].on = false;
    for (int i = 0; i < MAX_POWERUPS;    ++i) powerups[i].on = false;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projs[i].on    = false;
    for (int i = 0; i < MAX_BALLS; ++i) balls[i].on = false;
    cueDropOnFlipper(balls[0], pocketLastDropSide);
    slotsResetReels();

    // Quest reward items: Chip Stack starts you with extra balance; Insurance
    // arms a one-time refund of a busted hand. Consumed at game start.
    // Armed Quest-inventory items land here, at the start of the next game.
    score += 250 * (int)qpTakePending(ITEM_SL_BALANCE);
    slotsInsuranceArmed = qpTakePending(ITEM_SL_INSURANCE) > 0;
}

static Card randomCard() {
    Card c;
    c.value = (int8_t)random(0, SLOT_VALUES);
    c.suit  = (int8_t)random(0, SLOT_SUITS);
    return c;
}

// Trigger a spin. Reels not held will spin for ~30..50 frames staggered,
// then settle on a fresh random card.
static void slotsSpin(bool isFirstDeal) {
    int baseDur = 30;
    for (int i = 0; i < SLOT_REELS; ++i) {
        bool willSpin = !reels[i].held || isFirstDeal;
        if (willSpin) {
            reels[i].spinFrames = baseDur + i * 7;     // staggered stop
        }
    }
    slotsHandSettled = false;
    slotsCurrentSpinIsFirstDeal = isFirstDeal;
    if (isFirstDeal) slotsAwaitingFirstDeal = false;
}

static void slotsTickReels() {
    bool anySpinning = false;
    for (int i = 0; i < SLOT_REELS; ++i) {
        if (reels[i].spinFrames > 0) {
            reels[i].spinFrames--;
            // While spinning, randomize the displayed card every few frames
            if ((reels[i].spinFrames & 1) == 0) reels[i].shown = randomCard();
            anySpinning = true;
        }
    }
    if (!anySpinning && !slotsHandSettled) {
        // Just finished spinning - settle the hand.
        // For the first deal, we DON'T apply the score (per spec).
        Card hand[SLOT_REELS];
        for (int i = 0; i < SLOT_REELS; ++i) hand[i] = reels[i].shown;
        HandResult h = evaluateHand(hand);
        slotsLastWinPay = h.pay;
        strncpy(slotsLastWinName, h.name, 16);
        slotsLastWinFrames = 90;
        slotsHandSettled = true;
        if (!slotsCurrentSpinIsFirstDeal) {
            if (h.pay > 0) score += h.pay;
            else if (slotsInsuranceArmed) {
                slotsInsuranceArmed = false;   // refund this one bust, then spent
            }
            else           score = (score >= 10) ? score - 10 : 0;
            if (h.pay > 0) flash = 1.0f;
            // Holds reset between scoring spins
            for (int i = 0; i < SLOT_REELS; ++i) reels[i].held = false;
        }
        slotsCurrentSpinIsFirstDeal = false;
    }
}

// AABB-vs-circle collision check; returns true and writes normal/penetration.
static bool circleVsRect(float bx, float by, float br,
                         int rx, int ry, int rw, int rh,
                         float &nx, float &ny, float &pen) {
    // Closest point on the rect to the circle center
    float cx = bx, cy = by;
    if (cx < rx)        cx = rx;
    if (cx > rx + rw)   cx = rx + rw;
    if (cy < ry)        cy = ry;
    if (cy > ry + rh)   cy = ry + rh;
    float dx = bx - cx, dy = by - cy;
    float d2 = dx*dx + dy*dy;
    if (d2 >= br * br) return false;
    float d = sqrtf(d2);
    if (d > 0.001f) { nx = dx / d; ny = dy / d; pen = br - d; }
    else            { nx = 0; ny = -1; pen = br; }
    return true;
}

static void slotsCollideEverything(Ball &b) {
    if (!b.on) return;

    // NOTE: reels themselves are visual-only. If they collided, the ball
    // could never reach the hold-toggle targets above them.

    // Hold-toggle targets (5 small squares above each reel)
    for (int i = 0; i < SLOT_REELS; ++i) {
        SlotTarget &h = holdTargets[i];
        float dx = b.x - h.x, dy = b.y - h.y;
        if (dx*dx + dy*dy < (BALL_R + 5.0f) * (BALL_R + 5.0f)) {
            float d = sqrtf(dx*dx + dy*dy);
            if (d < 0.001f) d = 0.001f;
            float nx = dx / d, ny = dy / d;
            float pen = (BALL_R + 5.0f) - d;
            b.x += nx * pen;
            b.y += ny * pen;
            reflectVelocity(b.vx, b.vy, nx, ny, 0.85f, 0.96f);
            // Toggle hold (only meaningful after the first deal)
            if (!slotsAwaitingFirstDeal && reels[i].spinFrames == 0) {
                reels[i].held = !reels[i].held;
            }
            h.flashFrames = 4;
        }
    }

    // Moving spin target
    {
        int rx = (int)(spinTarget.x - 12);
        int ry = (int)(spinTarget.y - 4);
        float nx, ny, pen;
        if (circleVsRect(b.x, b.y, BALL_R, rx, ry, 24, 8, nx, ny, pen)) {
            b.x += nx * pen;
            b.y += ny * pen;
            reflectVelocity(b.vx, b.vy, nx, ny, 0.92f, 0.96f);
            spinTarget.flashFrames = 5;
            // Trigger a real spin (not the first deal)
            bool anySpinning = false;
            for (int i = 0; i < SLOT_REELS; ++i)
                if (reels[i].spinFrames > 0) { anySpinning = true; break; }
            if (!anySpinning) slotsSpin(/*isFirstDeal=*/false);
        }
    }
}

static void tickSlots() {
    stepFlipper(flipL, btnL);
    stepFlipper(flipR, btnR);

    // Cue ball
    if (balls[0].on) stepBall(balls[0]);

    // Custom collisions for slots (reels, hold targets, spin target)
    slotsCollideEverything(balls[0]);

    // Auto-deal first hand once the cue ball is in the upper half of the table
    if (slotsAwaitingFirstDeal && balls[0].on && balls[0].y < (float)H * 0.7f) {
        slotsSpin(/*isFirstDeal=*/true);
    }

    // Move the spin target back and forth
    spinTarget.x += spinTarget.vx;
    if (spinTarget.x < 18.0f) { spinTarget.x = 18.0f; spinTarget.vx = -spinTarget.vx; }
    if (spinTarget.x > (float)W - 18.0f) { spinTarget.x = (float)W - 18.0f; spinTarget.vx = -spinTarget.vx; }

    // Reels animate / settle
    slotsTickReels();

    // Flash decay
    for (int i = 0; i < SLOT_REELS; ++i)
        if (holdTargets[i].flashFrames > 0) holdTargets[i].flashFrames--;
    if (spinTarget.flashFrames > 0) spinTarget.flashFrames--;
    if (slotsLastWinFrames > 0) slotsLastWinFrames--;
    if (flash > 0.05f) flash *= 0.85f; else flash = 0.0f;

    // Cue ball drained
    if (!balls[0].on || balls[0].y > (float)H + 6.0f) {
        balls[0].on = false;
        if (ballsRemaining > 1) {
            ballsRemaining--;
            cueDropOnFlipper(balls[0], pocketLastDropSide);
        } else {
            saveHighScore(STATE_SLOTS, score);
            gameOverMode = STATE_SLOTS;
            appState = STATE_GAMEOVER;
        }
    }
}

// Card rendering helpers
static const char* CARD_VALUE_STR(int v) {
    static const char* s[] = {"2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    if (v < 0 || v >= SLOT_VALUES) return "?";
    return s[v];
}

static void drawCard(int x, int y, const Card &c, bool held) {
    // Card body
    fb.fillRoundRect(x, y, CARD_W, CARD_H, 3, LIME_PALE);
    fb.drawRoundRect(x, y, CARD_W, CARD_H, 3, held ? LIME : C_WALL);
    if (held) {
        // Held cards get a lime border ring
        fb.drawRoundRect(x - 1, y - 1, CARD_W + 2, CARD_H + 2, 4, LIME);
    }
    if (c.value < 0) {
        // Card back - purple diagonal pattern
        fb.fillRoundRect(x + 2, y + 2, CARD_W - 4, CARD_H - 4, 2, PURPLE);
        for (int dx = 2; dx < CARD_W - 2; dx += 4)
            fb.drawLine(x + dx, y + 2, x + dx + 4, y + CARD_H - 2, LIME_DARK);
        return;
    }
    // Value text - red (purple_bright stand-in) or black (very dark teal)
    uint16_t inkCol = (c.suit == 0) ? PURPLE_BRIGHT : TEAL_DEEP;
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(inkCol);
    fb.drawString(CARD_VALUE_STR(c.value), x + CARD_W / 2, y + CARD_H / 2 - 2, 4);
    // Tiny suit indicator (a filled square in the corner)
    fb.fillRect(x + 2, y + 2, 3, 3, inkCol);
    fb.fillRect(x + CARD_W - 5, y + CARD_H - 5, 3, 3, inkCol);
}

static void composeSlotsScene() {
    fb.fillSprite(flash > 0.4f ? C_BG_FLASH : C_BG);

    // Outer rails
    fb.drawFastHLine(0, 0,     W, C_WALL);
    fb.drawFastVLine(0, 0,     H, C_WALL);
    fb.drawFastVLine(W - 1, 0, H, C_WALL);

    // Funnel walls
    fb.drawLine((int)walls[3].x1, (int)walls[3].y1,
                (int)walls[3].x2, (int)walls[3].y2, C_WALL_HI);
    fb.drawLine((int)walls[4].x1, (int)walls[4].y1,
                (int)walls[4].x2, (int)walls[4].y2, C_WALL_HI);

    // Drain marker
    int gapL = (int)(W * 0.5f - DRAIN_HALF_GAP);
    int gapR = (int)(W * 0.5f + DRAIN_HALF_GAP);
    for (int x = gapL; x <= gapR; x += 2) fb.drawPixel(x, H - 3, PURPLE_DARK);

    // ---- Spin target (moving) ----
    {
        uint16_t col = (spinTarget.flashFrames > 0) ? LIME_PALE : LIME;
        int sx = (int)(spinTarget.x - 12);
        int sy = (int)(spinTarget.y - 4);
        fb.fillRoundRect(sx, sy, 24, 8, 2, col);
        fb.drawRoundRect(sx, sy, 24, 8, 2, LIME_PALE);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(C_BG);
        fb.drawString("SPIN", (int)spinTarget.x, (int)spinTarget.y, 1);
    }

    // ---- Hold-toggle targets ----
    for (int i = 0; i < SLOT_REELS; ++i) {
        const SlotTarget &h = holdTargets[i];
        uint16_t col = (h.flashFrames > 0) ? LIME_PALE
                     : (reels[i].held ? LIME : PURPLE);
        fb.fillRect((int)h.x - 4, (int)h.y - 4, 9, 9, col);
        fb.drawRect((int)h.x - 4, (int)h.y - 4, 9, 9, LIME_PALE);
        if (reels[i].held) {
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(C_BG);
            fb.drawString("H", (int)h.x, (int)h.y, 1);
        }
    }

    // ---- Reels ----
    int reelTopY = REEL_Y - CARD_H / 2;
    fb.drawFastHLine(0, reelTopY - 3,         W, TEAL_DARK);
    fb.drawFastHLine(0, reelTopY + CARD_H + 2, W, TEAL_DARK);
    for (int i = 0; i < SLOT_REELS; ++i) {
        drawCard(reelX(i), reelTopY, reels[i].shown, reels[i].held);
    }

    // Cue ball
    if (balls[0].on) drawBall(balls[0]);

    // Flippers
    drawFlipper(flipL);
    drawFlipper(flipR);

    // HUD
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(C_HUD, C_BG);
    fb.drawNumber((long)score, 4, 3, 2);

    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(C_HUD_DIM, C_BG);
    char buf[24];
    snprintf(buf, sizeof(buf), "BALLS %u", (unsigned)ballsRemaining);
    fb.drawString(buf, W - 32, 3, 1);

    drawBatteryBar(W - 28, 17);

    // Last hand result label
    if (slotsLastWinFrames > 0) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(slotsLastWinPay > 0 ? LIME : PURPLE_LIGHT);
        fb.drawString(slotsLastWinName, W / 2, 23, 2);
        if (slotsLastWinPay > 0) {
            snprintf(buf, sizeof(buf), "+%u", (unsigned)slotsLastWinPay);
            fb.setTextColor(LIME_PALE);
            fb.drawString(buf, W / 2, 36, 1);
        }
    } else if (slotsAwaitingFirstDeal) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(C_HUD_DIM);
        fb.drawString("LAUNCH TO DEAL", W / 2, 90, 1);
    } else {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(C_HUD_DIM);
        fb.drawString("HIT SPIN  HOLD CARDS", W / 2, 90, 1);
    }

    // Sleep-hold progress
    if (bothHeldSince) {
        uint32_t held = millis() - bothHeldSince;
        if (held > 500) {
            int barW = (int)(((float) held / SLEEP_HOLD_MS) * (W - 40));
            if (barW > W - 40) barW = W - 40;
            fb.drawRect(20, H - 6, W - 40, 3, C_WALL);
            fb.fillRect(20, H - 6, barW,  3, C_WARN);
        }
    }
}

static void drawSlotsScene() {
    composeSlotsScene();
    fb.pushSprite(0, 0);
}

// ---------------- SETUP / LOOP ----------------
void setup() {
    themeInit();          // colour scheme first: everything below may draw
    pinMode(PIN_LCD_PWR, OUTPUT);
    digitalWrite(PIN_LCD_PWR, HIGH);

    // If we just woke from deep sleep, release the RTC holds placed on the
    // wake buttons so they behave as normal GPIO inputs again.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        rtc_gpio_deinit((gpio_num_t) PIN_BTN_L);
        rtc_gpio_deinit((gpio_num_t) PIN_BTN_R);
    }

    pinMode(PIN_BTN_L, INPUT_PULLUP);
    pinMode(PIN_BTN_R, INPUT_PULLUP);

    // Battery ADC: 12-bit resolution, 11 dB attenuation gives ~0..3.3 V range,
    // good for the divided LiPo voltage on GPIO 4.
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(C_BG);

    fb.setColorDepth(16);
    if (!fb.createSprite(W, H)) {
        fb.setColorDepth(8);
        if (fb.createSprite(W, H)) fbReady = true;
    } else {
        fbReady = true;
    }

    randomSeed(esp_random());
    buildWalls();
    initFlippers();

    // Prime battery reading so the bar shows a sane value from the first frame
    for (int i = 0; i < 4; ++i) batterySample();
    batteryLastSample = millis();

    // Load persisted high scores
    loadHighScores();

    // Seed installer-provided default config into NVS on first boot.
    // Only writes a key if it's currently empty, so a user who sets up
    // on-device is never overwritten. Empty defaults are skipped entirely.
    seedDefaultConfig();

    // Load the shared RPG profile (level, dice, unlocks) and quest config so
    // CYOA, QUESTS, and Pet Mode all share one progression from boot.
    qpLoad();
    questsConfigLoad();
    petLoad();
    sysclockBegin();   // restore last-known time so day tracking survives reboot
    backlightBegin();  // PWM brightness control (remembers level across reboots)

    // Probe the bottom-JST gesture sensor once at boot so the menu can be
    // built with Fruit Frenzy included/excluded from the very first open.
    // (enterMenu() re-probes on every open, so hot-plug still works.)
    fruitProbeSensor();
    rebuildMenu();

    // Emit the app capability manifest over Serial for a connected studio.
    Serial.begin(115200);
    appEmitManifest();

    // Start in intro
    appState   = STATE_INTRO;
    introFrame = 0;
    lastFrame  = millis();
}

// Menu-driven clock sync, spread across ticks so it NEVER blocks the UI (the
// old blocking version stalled the menu for seconds and forced weather mode to
// cold-start WiFi, which made weather feel slow). Non-blocking: kick off a
// connection, poll it over frames, sync, tear down - a few ms per frame.
enum MenuClockPhase { MC_IDLE, MC_CONNECTING, MC_DONE };
static MenuClockPhase menuClockPhase = MC_IDLE;
static uint32_t menuClockStart = 0;

static void menuClockTickNonBlocking() {
    switch (menuClockPhase) {
        case MC_DONE: return;
        case MC_IDLE: {
            if (sysclockValid() && !sysclockNeedsConfirm()) { menuClockPhase = MC_DONE; return; }
            String ssid, pass;
            { Preferences p; if (p.begin("btcminer", true)) {
                ssid = p.getString("ssid",""); pass = p.getString("pass",""); p.end(); } }
            if (ssid.length() == 0) { menuClockPhase = MC_DONE; return; }
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());
            menuClockStart = millis();
            menuClockPhase = MC_CONNECTING;
            return;
        }
        case MC_CONNECTING: {
            if (WiFi.status() == WL_CONNECTED) {
                sysclockSyncNTP(3000);
                WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
                menuClockPhase = MC_DONE;
            } else if (millis() - menuClockStart > 8000) {
                WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
                menuClockPhase = MC_DONE;
            }
            return;
        }
    }
}

// ---------------- APP INPUT GUARD ----------------
// Wrapped around the app dispatch ONLY. The exit double-click and the sleep
// hold above still read the raw buttons; the raw values are restored after
// the app's tick, so those detectors never see the mask.
//
//  * ENTRY LATCH (every app): the menu selects on PRESS, so an app starts with
//    R still held. Until both buttons have been released once, the app sees
//    neither - the selecting press can never leak into it.
//  * CHORD GUARD (apps that never need both buttons and have no guard of their
//    own): a both-buttons chord - the menu double-click - is hidden from the
//    app, plus 350 ms after release to swallow the second click. Same rule as
//    chord_guard.cpp. Pinball and the other both-button games are not listed,
//    so both buttons always work there right up until you leave.
static bool     g_cgPoison = false;
static uint32_t g_cgReleasedAt = 0;
static bool     g_rawL, g_rawR, g_rawLp, g_rawRp, g_appL, g_appR;

static bool centralChordGuard(AppState s) {
    switch (s) {
    case STATE_WEATHER: case STATE_SPACE: case STATE_RABBIT: case STATE_MINER:
    case STATE_APOD:    case STATE_MOON:  case STATE_DICE:   case STATE_CLICKER:
    case STATE_SETUP:   case STATE_SHARE: case STATE_RUBIKS: case STATE_SCOUT:
    case STATE_SURVIVAL: case STATE_FIRSTAID: case STATE_FRUIT: case STATE_PARTY:
        return true;
    default:
        return false;      // pinball family, action games, and apps with their own guard
    }
}

static void appInputGuardBegin() {
    g_rawL = btnL; g_rawR = btnR; g_rawLp = btnL_prev; g_rawRp = btnR_prev;
    bool vl = btnL, vr = btnR;
    if (g_entryLatch) {
        if (!g_rawL && !g_rawR) g_entryLatch = false;
        else vl = vr = false;
    }
    if (centralChordGuard(appState)) {
        const uint32_t now = millis();
        if (g_rawL && g_rawR) g_cgPoison = true;
        if (g_cgPoison) {
            if (!g_rawL && !g_rawR) {
                if (!g_cgReleasedAt) g_cgReleasedAt = now;
                if (now - g_cgReleasedAt > 350) { g_cgPoison = false; g_cgReleasedAt = 0; }
            } else g_cgReleasedAt = 0;
            vl = vr = false;
        }
    } else { g_cgPoison = false; g_cgReleasedAt = 0; }
    btnL_prev = g_appL; btnR_prev = g_appR;   // the app's edges come from what IT saw
    btnL = vl; btnR = vr;
    g_appL = vl; g_appR = vr;
}

static void appInputGuardEnd() {
    btnL = g_rawL; btnR = g_rawR; btnL_prev = g_rawLp; btnR_prev = g_rawRp;
}

void loop() {
    const uint32_t now = millis();
    if (now - lastFrame < 16) return;
    lastFrame = now;

    readInput();
    handleSleepHold();
    tickBattery();

    // Double-click both buttons to open the game-select menu (only from
    // an active game state, not from intro/menu/game-over).
    tickDoubleClick();
    if (dcLatchedFromYield) { dcFiredThisFrame = true; dcLatchedFromYield = false; }
    if (dcFiredThisFrame &&
        (appState == STATE_PLAYING || appState == STATE_POCKET ||
         appState == STATE_SLOTS   || appState == STATE_MINER  ||
         appState == STATE_WEATHER ||
         appState == STATE_SPACE  ||
         appState == STATE_RABBIT  ||
         appState == STATE_APOD   ||
         appState == STATE_FLIGHT ||
         appState == STATE_CANDLES ||
         appState == STATE_VERSUS  ||
         appState == STATE_SCOUT   ||
         appState == STATE_SURVIVAL||
         appState == STATE_FIRSTAID ||
         appState == STATE_GUIDEBOOKS ||
         appState == STATE_CYOA    ||
         appState == STATE_QUESTS  ||
         appState == STATE_SHARE   ||
         appState == STATE_PET    ||
         appState == STATE_MOON   ||
         appState == STATE_NEO    ||
         appState == STATE_CLICKER ||
         appState == STATE_DICE    ||
         appState == STATE_AQUARIUM ||
         appState == STATE_FRUIT   ||
         appState == STATE_ROADHUNT ||
         appState == STATE_SIGHTINGS ||
         appState == STATE_MANTISCANNON ||
         appState == STATE_MANTISSKATE ||
         appState == STATE_MANTISFISH ||
         appState == STATE_DUNERIDER ||
         appState == STATE_MANTISPUTT ||
         appState == STATE_MANTISDROP ||
         appState == STATE_MANTISVADERS ||
         appState == STATE_MANTISPSYCHIC ||
         appState == STATE_RUBIKS ||
         appState == STATE_FIREWORKS ||
         appState == STATE_MANTISCHAT ||
         appState == STATE_DANCEPARTY ||
         appState == STATE_HAM     ||
         appState == STATE_PARTY   ||
         appState == STATE_SETUP)) {
        // Release resources before returning to portrait menu.
        if (appState == STATE_MINER)   minerShutdown();
        if (appState == STATE_WEATHER) weatherShutdown();
        if (appState == STATE_SPACE)   spaceShutdown();
        if (appState == STATE_RABBIT)  rabbitShutdown();
        if (appState == STATE_APOD)    apodShutdown();
        if (appState == STATE_FLIGHT)  flightShutdown();
        if (appState == STATE_CANDLES) candlesShutdown();
        if (appState == STATE_VERSUS)  versusShutdown();
        if (appState == STATE_SCOUT)    scoutShutdown();
        if (appState == STATE_SURVIVAL) survivalShutdown();
        if (appState == STATE_FIRSTAID) firstaidShutdown();
        if (appState == STATE_GUIDEBOOKS) guidebooksShutdown();
        if (appState == STATE_CYOA)     cyoaShutdown();
        if (appState == STATE_QUESTS)   questsShutdown();
        if (appState == STATE_SHARE)    shareShutdown();
        if (appState == STATE_PET)      petShutdown();
        if (appState == STATE_MOON)     moonShutdown();
        if (appState == STATE_NEO)      neoShutdown();
        if (appState == STATE_CLICKER)  clickerShutdown();
        if (appState == STATE_DICE)     diceShutdown();
        if (appState == STATE_AQUARIUM) aquariumShutdown();
        if (appState == STATE_FRUIT)    fruitShutdown();
        if (appState == STATE_ROADHUNT) roadhuntShutdown();
        if (appState == STATE_SIGHTINGS) sightingsShutdown();
        if (appState == STATE_MANTISCANNON) mantiscannonShutdown();
        if (appState == STATE_MANTISSKATE) mantisskateShutdown();
        if (appState == STATE_MANTISFISH) mantisfishShutdown();
        if (appState == STATE_DUNERIDER) duneriderShutdown();
        if (appState == STATE_MANTISPUTT) mantisputtShutdown();
        if (appState == STATE_MANTISDROP) mantisdropShutdown();
        if (appState == STATE_MANTISVADERS) mantisvadersShutdown();
        if (appState == STATE_MANTISPSYCHIC) mantisPsychicShutdown();
        if (appState == STATE_RUBIKS) rubiksShutdown();
        if (appState == STATE_FIREWORKS) fireworksShutdown();
        if (appState == STATE_MANTISCHAT) mantischatShutdown();
        if (appState == STATE_DANCEPARTY) dancepartyShutdown();
        if (appState == STATE_HAM)      hamShutdown();
        if (appState == STATE_PARTY)    partyHubShutdown();
        if (appState == STATE_SETUP)    setupModeShutdown();
        enterMenu();
    }

    appInputGuardBegin();
    switch (appState) {
        case STATE_INTRO:
            tickIntro();
            if (fbReady) drawIntro(introFrame);
            break;

        case STATE_MENU:
            tickMenu();
            if (fbReady) drawMenu();
            // Non-blocking clock sync (spread across frames; never stalls the
            // menu or forces weather to cold-start WiFi).
            menuClockTickNonBlocking();
            break;

        case STATE_PLAYING:
            tickPlaying();
            if (fbReady) drawScene();
            break;

        case STATE_POCKET:
            tickPocket();
            if (fbReady) drawPocketScene();
            break;

        case STATE_SLOTS:
            tickSlots();
            if (fbReady) drawSlotsScene();
            break;

        case STATE_MINER:
            // Mining mode owns its own framebuffer and rotation. main.cpp's
            // portrait sprite is left alone; we just delegate.
            minerTick();
            minerDraw();
            break;

        case STATE_WEATHER:
            weatherTick();
            weatherDraw();
            break;

        case STATE_SPACE:
            spaceTick();
            spaceDraw();
            break;

        case STATE_RABBIT:
            rabbitTick();
            rabbitDraw();
            break;

        case STATE_APOD:
            apodTick();
            apodDraw();
            break;

        case STATE_FLIGHT:
            flightTick();
            flightDraw();
            break;

        case STATE_CANDLES:
            candlesTick();
            candlesDraw();
            break;

        case STATE_VERSUS:
            versusTick();
            versusDraw();
            break;

        case STATE_SCOUT:
            scoutTick();
            scoutDraw();
            break;

        case STATE_SURVIVAL:
            survivalTick();
            survivalDraw();
            break;

        case STATE_FIRSTAID:
            firstaidTick();
            firstaidDraw();
            break;

        case STATE_GUIDEBOOKS:
            guidebooksTick();
            guidebooksDraw();
            break;

        case STATE_CYOA:
            cyoaTick();
            cyoaDraw();
            break;

        case STATE_QUESTS:
            questsTick();
            questsDraw();
            break;

        case STATE_SHARE:
            shareTick();
            shareDraw();
            break;

        case STATE_PET:
            petTick();
            petDraw();
            break;

        case STATE_MOON:
            moonTick();
            moonDraw();
            break;

        case STATE_NEO:
            neoTick();
            neoDraw();
            break;

        case STATE_CLICKER:
            clickerTick();
            clickerDraw();
            break;

        case STATE_DICE:
            diceTick();
            diceDraw();
            break;

        case STATE_AQUARIUM:
            aquariumTick();
            aquariumDraw();
            break;

        case STATE_FRUIT:
            fruitTick();
            fruitDraw();
            break;

        case STATE_ROADHUNT:
            roadhuntTick();
            roadhuntDraw();
            break;

        case STATE_SIGHTINGS:
            sightingsTick();
            sightingsDraw();
            break;

        case STATE_MANTISCANNON:
            mantiscannonTick();
            mantiscannonDraw();
            break;

        case STATE_MANTISSKATE:
            mantisskateTick();
            mantisskateDraw();
            break;

        case STATE_MANTISFISH:
            mantisfishTick();
            mantisfishDraw();
            break;

        case STATE_DUNERIDER:
            duneriderTick();
            duneriderDraw();
            break;

        case STATE_MANTISPUTT:
            mantisputtTick();
            mantisputtDraw();
            break;

        case STATE_MANTISDROP:
            mantisdropTick();
            mantisdropDraw();
            break;

        case STATE_MANTISVADERS:
            mantisvadersTick();
            mantisvadersDraw();
            break;

        case STATE_MANTISPSYCHIC:
            mantisPsychicTick();
            mantisPsychicDraw();
            break;

        case STATE_RUBIKS:
            rubiksTick();
            rubiksDraw();
            break;

        case STATE_FIREWORKS:
            fireworksTick();
            fireworksDraw();
            break;

        case STATE_MANTISCHAT:
            mantischatTick();
            mantischatDraw();
            break;

        case STATE_DANCEPARTY:
            dancepartyTick();
            dancepartyDraw();
            break;

        case STATE_HAM:
            hamTick();
            hamDraw();
            break;

        case STATE_PARTY:
            partyHubTick();
            partyHubDraw();
            break;

        case STATE_SETUP:
            setupModeTick();
            setupModeDraw();
            break;

        case STATE_GAMEOVER:
            // Animate flippers even while waiting on the game-over screen
            stepFlipper(flipL, btnL);
            stepFlipper(flipR, btnR);
            if (justPressed()) {
                // Restart whichever game ended
                switch (gameOverMode) {
                    case STATE_POCKET: resetPocket(); appState = STATE_POCKET; break;
                    case STATE_SLOTS:  resetSlots();  appState = STATE_SLOTS;  break;
                    default:           resetGame();   appState = STATE_PLAYING; break;
                }
            }
            if (fbReady) drawGameOver();
            break;
    }
    appInputGuardEnd();
}
