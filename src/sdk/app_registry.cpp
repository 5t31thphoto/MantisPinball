// app_registry.cpp - the single registry of every App in the firmware.
//
// This is the canonical list the studio reads (via the generated
// firmware_api.json that mirrors it). Adding an App = adding one row here plus
// its four lifecycle functions. The stateId values match the AppState enum in
// main.cpp; they are passed as plain ints to keep this module decoupled.
#include "app_sdk.h"
#include "../build_config.h"

// ── Forward declarations of every modular App's lifecycle functions ───────────
// (Each is defined in its own module; signatures are uniform void f().)
extern void weatherEnter();  extern void weatherTick();  extern void weatherDraw();  extern void weatherShutdown();
extern void spaceEnter();    extern void spaceTick();    extern void spaceDraw();    extern void spaceShutdown();
extern void rabbitEnter();   extern void rabbitTick();   extern void rabbitDraw();   extern void rabbitShutdown();
extern void apodEnter();     extern void apodTick();     extern void apodDraw();     extern void apodShutdown();
extern void flightEnter();   extern void flightTick();   extern void flightDraw();   extern void flightShutdown();
extern void candlesEnter();  extern void candlesTick();  extern void candlesDraw();  extern void candlesShutdown();
extern void versusEnter();   extern void versusTick();   extern void versusDraw();   extern void versusShutdown();
extern void scoutEnter();    extern void scoutTick();    extern void scoutDraw();    extern void scoutShutdown();
extern void survivalEnter(); extern void survivalTick(); extern void survivalDraw(); extern void survivalShutdown();
extern void firstaidEnter(); extern void firstaidTick(); extern void firstaidDraw(); extern void firstaidShutdown();
extern void guidebooksEnter(); extern void guidebooksTick(); extern void guidebooksDraw(); extern void guidebooksShutdown();
extern void cyoaEnter();     extern void cyoaTick();     extern void cyoaDraw();     extern void cyoaShutdown();
extern void questsEnter();   extern void questsTick();   extern void questsDraw();   extern void questsShutdown();
extern void shareEnter();    extern void shareTick();    extern void shareDraw();    extern void shareShutdown();
extern void petEnter();      extern void petTick();      extern void petDraw();      extern void petShutdown();
extern void moonEnter();     extern void moonTick();     extern void moonDraw();     extern void moonShutdown();
extern void neoEnter();      extern void neoTick();      extern void neoDraw();      extern void neoShutdown();
extern void clickerEnter();  extern void clickerTick();  extern void clickerDraw();  extern void clickerShutdown();
extern void diceEnter();     extern void diceTick();     extern void diceDraw();     extern void diceShutdown();
extern void aquariumEnter(); extern void aquariumTick(); extern void aquariumDraw(); extern void aquariumShutdown();
extern void hamEnter();      extern void hamTick();      extern void hamDraw();      extern void hamShutdown();
extern void fruitEnter();    extern void fruitTick();    extern void fruitDraw();    extern void fruitShutdown();
extern void roadhuntEnter(); extern void roadhuntTick(); extern void roadhuntDraw(); extern void roadhuntShutdown();
extern void sightingsEnter(); extern void sightingsTick(); extern void sightingsDraw(); extern void sightingsShutdown();
extern void mantiscannonEnter(); extern void mantiscannonTick(); extern void mantiscannonDraw(); extern void mantiscannonShutdown();
extern void mantisskateEnter(); extern void mantisskateTick(); extern void mantisskateDraw(); extern void mantisskateShutdown();
extern void mantisfishEnter(); extern void mantisfishTick(); extern void mantisfishDraw(); extern void mantisfishShutdown();
extern void duneriderEnter(); extern void duneriderTick(); extern void duneriderDraw(); extern void duneriderShutdown();
extern void mantisputtEnter(); extern void mantisputtTick(); extern void mantisputtDraw(); extern void mantisputtShutdown();
extern void mantisdropEnter(); extern void mantisdropTick(); extern void mantisdropDraw(); extern void mantisdropShutdown();
extern void mantisvadersEnter(); extern void mantisvadersTick(); extern void mantisvadersDraw(); extern void mantisvadersShutdown();
extern void mantisPsychicEnter(); extern void mantisPsychicTick(); extern void mantisPsychicDraw(); extern void mantisPsychicShutdown();
extern void rubiksEnter(); extern void rubiksTick(); extern void rubiksDraw(); extern void rubiksShutdown();
extern void fireworksEnter(); extern void fireworksTick(); extern void fireworksDraw(); extern void fireworksShutdown();
extern void mantischatEnter(); extern void mantischatTick(); extern void mantischatDraw(); extern void mantischatShutdown();
extern void dancepartyEnter(); extern void dancepartyTick(); extern void dancepartyDraw(); extern void dancepartyShutdown();

// AppState enum values (must match the enum order in main.cpp). Kept as named
// constants here so the table is readable; only the modular apps are listed.
enum {
    A_INTRO=0, A_MENU, A_PLAYING, A_POCKET, A_SLOTS, A_MINER, A_WEATHER, A_SPACE,
    A_RABBIT, A_APOD, A_FLIGHT, A_CANDLES, A_VERSUS, A_SCOUT, A_SURVIVAL,
    A_FIRSTAID, A_CYOA, A_QUESTS, A_SHARE, A_PET, A_MOON, A_NEO, A_CLICKER, A_DICE, A_AQUARIUM, A_HAM,
    A_PARTY, A_SETUP, A_FRUIT, A_GUIDEBOOKS,
    A_ROADHUNT, A_SIGHTINGS, A_MANTISCANNON, A_MANTISSKATE, A_MANTISFISH, A_DUNERIDER, A_MANTISPUTT, A_MANTISDROP, A_MANTISVADERS, A_MANTISPSYCHIC, A_RUBIKS, A_FIREWORKS, A_MANTISCHAT, A_DANCEPARTY, A_GAMEOVER
};

// The registry. Core games (pinball/pocket/slots) are inlineDispatch=1 because
// their lifecycle lives in main.cpp; they are listed so the studio knows they
// exist but won't try to scaffold or table-dispatch them.
static const AppDescriptor REGISTRY[] = {
    // id            name                cat           state       enter  tick  draw  shut   inline
    { "pinball",     "PINBALL BLASTER",  APPCAT_GAME,     A_PLAYING,  nullptr,nullptr,nullptr,nullptr, 1 },
    { "pocket",      "CORNER POCKET",    APPCAT_GAME,     A_POCKET,   nullptr,nullptr,nullptr,nullptr, 1 },
    { "slots",       "JACKS OR BETTER",  APPCAT_GAME,     A_SLOTS,    nullptr,nullptr,nullptr,nullptr, 1 },
    { "versus",      "VERSUS PINBALL",   APPCAT_GAME,     A_VERSUS,   versusEnter,  versusTick,  versusDraw,  versusShutdown,  0 },
    { "weather",     "WEATHER REPORT",   APPCAT_LIVEDATA, A_WEATHER,  weatherEnter, weatherTick, weatherDraw, weatherShutdown, 0 },
    { "space",       "SPACE WEATHER",    APPCAT_LIVEDATA, A_SPACE,    spaceEnter,   spaceTick,   spaceDraw,   spaceShutdown,   0 },
    { "flight",      "FLIGHTS OVERHEAD", APPCAT_LIVEDATA, A_FLIGHT,   flightEnter,  flightTick,  flightDraw,  flightShutdown,  0 },
    { "miner",       "BTC MINER",        APPCAT_LIVEDATA, A_MINER,    nullptr,nullptr,nullptr,nullptr, 1 },
    { "candles",     "CRYPTO CANDLES",   APPCAT_LIVEDATA, A_CANDLES,  candlesEnter, candlesTick, candlesDraw, candlesShutdown, 0 },
    { "apod",        "NASA APOD",        APPCAT_LIVEDATA, A_APOD,     apodEnter,    apodTick,    apodDraw,    apodShutdown,    0 },
    { "rabbit",      "RABBIT HOLE",      APPCAT_CREATIVE, A_RABBIT,   rabbitEnter,  rabbitTick,  rabbitDraw,  rabbitShutdown,  0 },
    { "cyoa",        "MANTIS TALE",      APPCAT_CREATIVE, A_CYOA,     cyoaEnter,    cyoaTick,    cyoaDraw,    cyoaShutdown,    0 },
    { "firstaid",    "FIRST AID",        APPCAT_GUIDE,    A_FIRSTAID, firstaidEnter,firstaidTick,firstaidDraw,firstaidShutdown,0 },
    { "survival",    "SURVIVAL GUIDE",   APPCAT_GUIDE,    A_SURVIVAL, survivalEnter,survivalTick,survivalDraw,survivalShutdown,0 },
    { "scout",       "SCOUT SKILLS",     APPCAT_GUIDE,    A_SCOUT,    scoutEnter,   scoutTick,   scoutDraw,   scoutShutdown,   0 },
    // GuideBooks is the menu-facing app; the three above are reached through it
    // and are no longer given their own menu slots (kept for state compat).
    { "guidebooks",  "GUIDEBOOKS",       APPCAT_GUIDE,    A_GUIDEBOOKS, guidebooksEnter, guidebooksTick, guidebooksDraw, guidebooksShutdown, 0 },
    { "quests",      "QUESTS",           APPCAT_SYSTEM,   A_QUESTS,   questsEnter,  questsTick,  questsDraw,  questsShutdown,  0 },
    { "pet",         "MANTIS PET",       APPCAT_SYSTEM,   A_PET,      petEnter,     petTick,     petDraw,     petShutdown,     0 },
    { "share",       "PINBALL SHARING",  APPCAT_SYSTEM,   A_SHARE,    shareEnter,   shareTick,   shareDraw,   shareShutdown,   0 },
    { "moon",        "MOON",             APPCAT_LIVEDATA, A_MOON,     moonEnter,    moonTick,    moonDraw,    moonShutdown,    0 },
    { "neo",         "ASTEROIDS",        APPCAT_LIVEDATA, A_NEO,      neoEnter,     neoTick,     neoDraw,     neoShutdown,     0 },
    { "clicker",     "CLICK COUNTER",    APPCAT_SYSTEM,   A_CLICKER,  clickerEnter, clickerTick, clickerDraw, clickerShutdown, 0 },
    { "dice",        "DICE",             APPCAT_GAME,     A_DICE,     diceEnter,    diceTick,    diceDraw,    diceShutdown,    0 },
    { "aquarium",    "AQUARIUM",         APPCAT_GAME,     A_AQUARIUM, aquariumEnter,aquariumTick,aquariumDraw,aquariumShutdown,0 },
    // Requires the PAJ7620 gesture sensor on the bottom JST; the firmware
    // hides its menu entry at runtime when the sensor isn't detected.
    { "fruit",       "FRUIT FRENZY",     APPCAT_GAME,     A_FRUIT,    fruitEnter,   fruitTick,   fruitDraw,   fruitShutdown,   0 },
    { "ham",         "HAM RADIO",        APPCAT_LIVEDATA, A_HAM,      hamEnter,     hamTick,     hamDraw,     hamShutdown,     0 },
    { "roadhunt", "ROAD HUNT", APPCAT_GAME, A_ROADHUNT, roadhuntEnter, roadhuntTick, roadhuntDraw, roadhuntShutdown, 0 },
    { "sightings", "SIGHTINGS", APPCAT_GAME, A_SIGHTINGS, sightingsEnter, sightingsTick, sightingsDraw, sightingsShutdown, 0 },
    { "mantiscannon", "MANTIS CANNON", APPCAT_GAME, A_MANTISCANNON, mantiscannonEnter, mantiscannonTick, mantiscannonDraw, mantiscannonShutdown, 0 },
    { "mantisskate", "MANTIS SKATE", APPCAT_GAME, A_MANTISSKATE, mantisskateEnter, mantisskateTick, mantisskateDraw, mantisskateShutdown, 0 },
    { "mantisfish", "MANTIS FISHING", APPCAT_GAME, A_MANTISFISH, mantisfishEnter, mantisfishTick, mantisfishDraw, mantisfishShutdown, 0 },
    { "dunerider", "DUNE RIDER", APPCAT_GAME, A_DUNERIDER, duneriderEnter, duneriderTick, duneriderDraw, duneriderShutdown, 0 },
    { "mantisputt",  "MANTIS PUTT",      APPCAT_GAME,     A_MANTISPUTT,  mantisputtEnter,  mantisputtTick,  mantisputtDraw,  mantisputtShutdown,  0 },
    { "mantisdrop",  "MANTIS DROP",      APPCAT_GAME,     A_MANTISDROP,  mantisdropEnter,  mantisdropTick,  mantisdropDraw,  mantisdropShutdown,  0 },
    { "mantisvaders","MANTIS VADERS",    APPCAT_GAME,     A_MANTISVADERS,mantisvadersEnter,mantisvadersTick,mantisvadersDraw,mantisvadersShutdown,0 },
    { "mantispsychic","MANTIS PSYCHIC",  APPCAT_GAME,     A_MANTISPSYCHIC,mantisPsychicEnter,mantisPsychicTick,mantisPsychicDraw,mantisPsychicShutdown,0 },
    { "rubiks",      "CUBE SOLVER",      APPCAT_SYSTEM,   A_RUBIKS,      rubiksEnter,      rubiksTick,      rubiksDraw,      rubiksShutdown,      0 },
    { "fireworks",   "FIREWORKS",        APPCAT_GAME,     A_FIREWORKS,   fireworksEnter,   fireworksTick,   fireworksDraw,   fireworksShutdown,   0 },
    { "mantischat",  "MANTIS CHAT",      APPCAT_SYSTEM,   A_MANTISCHAT,  mantischatEnter,  mantischatTick,  mantischatDraw,  mantischatShutdown,  0 },
    { "danceparty",  "DANCE PARTY",      APPCAT_GAME,     A_DANCEPARTY,  dancepartyEnter,  dancepartyTick,  dancepartyDraw,  dancepartyShutdown,  0 },
};
static const int REGISTRY_COUNT = (int)(sizeof(REGISTRY)/sizeof(REGISTRY[0]));

const AppDescriptor* appRegistry() { return REGISTRY; }
int                  appCount()    { return REGISTRY_COUNT; }

const AppDescriptor* appByState(int stateId) {
    for (int i = 0; i < REGISTRY_COUNT; i++)
        if (REGISTRY[i].stateId == stateId) return &REGISTRY[i];
    return nullptr;
}

const char* appCategoryName(uint8_t cat) {
    switch (cat) {
        case APPCAT_GAME:     return "Game";
        case APPCAT_LIVEDATA: return "Live Data";
        case APPCAT_CREATIVE: return "Creative";
        case APPCAT_GUIDE:    return "Guide";
        case APPCAT_SYSTEM:   return "System";
        default:           return "Other";
    }
}

// Emit a capability manifest over Serial so the studio, when connected to COMx,
// can discover the firmware's apps and device facts at runtime (matches the
// generated firmware_api.json). Printed once at boot; harmless if no host.
#include <Arduino.h>
#include <esp_heap_caps.h>
void appEmitManifest() {
    Serial.println();
    Serial.println("{\"manifest\":\"pinball-s3\",\"schema\":1,");
    // Live memory truth at boot: total/free heap and the largest single block
    // an app could allocate right now (fragmentation-aware). These are the
    // numbers an app author must budget against on this no-PSRAM device.
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t totalHeap= heap_caps_get_total_size(MALLOC_CAP_8BIT);
    Serial.print("\"mem\":{\"totalHeap\":"); Serial.print((uint32_t)totalHeap);
    Serial.print(",\"freeHeapAtBoot\":"); Serial.print((uint32_t)freeHeap);
    Serial.print(",\"largestFreeBlock\":"); Serial.print((uint32_t)largest);
    Serial.print(",\"psram\":false},");
    Serial.println();
    Serial.println("\"device\":{\"board\":\"T-Display-S3\",\"display\":\"170x320\",\"psram\":false,\"flash\":\"16MB\"},");
    Serial.print("\"apps\":[");
    for (int i = 0; i < REGISTRY_COUNT; i++) {
        const AppDescriptor& a = REGISTRY[i];
        Serial.print("{\"id\":\""); Serial.print(a.id);
        Serial.print("\",\"name\":\""); Serial.print(a.name);
        Serial.print("\",\"cat\":\""); Serial.print(appCategoryName(a.category));
        Serial.print("\",\"inline\":"); Serial.print(a.inlineDispatch ? "true" : "false");
        Serial.print("}");
        if (i < REGISTRY_COUNT - 1) Serial.print(",");
    }
    Serial.println("]}");
}
