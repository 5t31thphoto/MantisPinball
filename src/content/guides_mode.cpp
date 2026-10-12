// guides_mode.cpp - Offline guide modes over the content-reader engine.
#include <Arduino.h>
#include "guides_mode.h"
#include "content_reader.h"
#include "../theme_colors.h"

// Each guide is just a ContentMode descriptor pointing at its .pak file.
// Scout and Survival are topic indexes; First Aid is step-by-step sequences.
static const ContentMode MODE_SCOUT = {
    "/content/scout.pak", "SCOUT SKILLS", CK_TOPICS, PURPLE
};
static const ContentMode MODE_SURVIVAL = {
    "/content/survival.pak", "SURVIVAL GUIDE", CK_TOPICS, TEAL_DARK
};
static const ContentMode MODE_FIRSTAID = {
    "/content/firstaid.pak", "FIRST AID", CK_SEQUENCE, PURPLE_DARK
};

void scoutEnter()    { contentEnter(&MODE_SCOUT); }
void scoutTick()     { contentTick(); }
void scoutDraw()     { contentDraw(); }
void scoutShutdown() { contentShutdown(); }

void survivalEnter()    { contentEnter(&MODE_SURVIVAL); }
void survivalTick()     { contentTick(); }
void survivalDraw()     { contentDraw(); }
void survivalShutdown() { contentShutdown(); }

void firstaidEnter()    { contentEnter(&MODE_FIRSTAID); }
void firstaidTick()     { contentTick(); }
void firstaidDraw()     { contentDraw(); }
void firstaidShutdown() { contentShutdown(); }
