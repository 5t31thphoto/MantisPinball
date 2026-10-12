// app_sdk.h - The Pinball-S3 App SDK.
//
// This formalizes what the firmware already does informally: every "mode" is an
// App with a uniform lifecycle (enter / tick / draw / shutdown). Declaring that
// contract in one place turns "modes" into first-class Apps that tools (the
// installer/studio) can enumerate, scaffold, and reason about.
//
// An App is described by an AppDescriptor: a stable id, a display name, the
// build-flag that gates it, a category, and its four lifecycle functions. The
// registry (app_registry.cpp) is the single source of truth listing every App;
// the studio reads the generated firmware_api.json (mirrors this table) so its
// app list, scaffolder, and docs stay in sync with the firmware automatically.
//
// IMPORTANT: this is additive. It does not replace the existing dispatch; it
// wraps the same functions so behavior is identical, while giving tools a clean
// model to build on.
#pragma once
#include <stdint.h>

// Lifecycle function signature shared by every App.
typedef void (*AppFn)();

// Broad categories used for grouping in the studio / menu.
enum AppCategory : uint8_t {
    APPCAT_GAME = 0,     // pinball, versus, slots...
    APPCAT_LIVEDATA,     // weather, space, flights, candles, miner
    APPCAT_CREATIVE,     // rabbit hole, cyoa
    APPCAT_GUIDE,        // scout, survival, first aid (content reader)
    APPCAT_SYSTEM,       // quests, pet, pinball-sharing AP
    APPCAT_COUNT
};

// A first-class App. enter/tick/draw/shutdown may be null for apps whose
// lifecycle is handled inline (e.g. the core pinball game in main.cpp); such
// apps set inlineDispatch=1 so the studio knows not to scaffold them.
struct AppDescriptor {
    const char* id;          // stable identifier, e.g. "weather"
    const char* name;        // display name, e.g. "WEATHER REPORT"
    uint8_t     category;    // AppCategory
    int         stateId;     // AppState enum value this app maps to
    AppFn       enter;
    AppFn       tick;
    AppFn       draw;
    AppFn       shutdown;
    uint8_t     inlineDispatch;  // 1 = lifecycle handled inline in main.cpp
};

// Registry access (defined in app_registry.cpp).
const AppDescriptor* appRegistry();   // pointer to the descriptor array
int                  appCount();      // number of descriptors
const AppDescriptor* appByState(int stateId);  // lookup, or nullptr
const char*          appCategoryName(uint8_t cat);

// Print a capability manifest over Serial (called once at boot) so a connected
// studio can discover the firmware's apps and device facts at runtime.
void                 appEmitManifest();
