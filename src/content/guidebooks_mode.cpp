// guidebooks_mode.cpp - GuideBooks: a select menu over the content reader.
//
// Shows a scrollable list of guides (Scout, Survival, First Aid, plus the new
// Campfire Cooking, Dutch Oven, and Fishing guides). Picking one loads its
// .pak into the shared content-reader engine; a "back to list" from the
// reader index returns here. Which guides are listed is set by GUIDE_MASK
// (installer-controlled), so unwanted guides never take a menu slot or flash
// space.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "guidebooks_mode.h"
#include "content_reader.h"
#include "content_format.h"
#include "../theme_colors.h"
#include "../build_config.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
#include "../chord_guard.h"
static constexpr int W = 170;
static constexpr int H = 320;

// If the installer didn't define a mask (older build_config.h), show all.
#ifndef GUIDE_MASK
#define GUIDE_MASK 0xFFFFu
#endif

// The guide library. bit = position in GUIDE_MASK (matches the installer's
// GUIDES list order). Each guide is a ContentMode descriptor for the reader.
struct GuideDef {
    uint8_t      bit;
    const char*  title;      // shown in the GuideBooks list
    ContentMode  mode;       // reader descriptor (pack path / kind / accent)
};
static const GuideDef GUIDES[] = {
    { 0, "Scout Skills",     { "/content/scout.pak",    "SCOUT SKILLS",    CK_TOPICS,   PURPLE      } },
    { 1, "Survival Guide",   { "/content/survival.pak", "SURVIVAL GUIDE",  CK_TOPICS,   TEAL_DARK   } },
    { 2, "First Aid",        { "/content/firstaid.pak", "FIRST AID",       CK_SEQUENCE, PURPLE_DARK } },
    { 3, "Campfire Cooking", { "/content/campfire.pak", "CAMPFIRE COOKING",CK_TOPICS,   LIME_DARK   } },
    { 4, "Dutch Oven",       { "/content/dutchoven.pak","DUTCH OVEN",      CK_TOPICS,   PURPLE      } },
    { 5, "Fishing",          { "/content/fishing.pak",  "FISHING GUIDE",   CK_TOPICS,   TEAL_DARK   } },
    { 6, "Gold & Rocks",     { "/content/goldrocks.pak","GOLD & ROCKS",    CK_TOPICS,   TEAL        } },
    { 7, "Foraging",         { "/content/foraging.pak", "WILD FORAGING",   CK_TOPICS,   LIME_DARK   } },
    { 8, "Orienteering",     { "/content/orienteering.pak","ORIENTEERING",  CK_TOPICS,   PURPLE_DARK } },
};
static const int GUIDE_COUNT = sizeof(GUIDES) / sizeof(GUIDES[0]);

// Enabled guides, filtered by GUIDE_MASK, resolved once on entry.
static int  gbEnabled[16];
static int  gbEnabledCount = 0;
static int  gbSel     = 0;
static int  gbTop     = 0;          // scroll window top
static bool gbInGuide = false;      // false = showing the list, true = reading

static void gbResolveEnabled() {
    gbEnabledCount = 0;
    for (int i = 0; i < GUIDE_COUNT; i++)
        if (GUIDE_MASK & (1u << GUIDES[i].bit))
            gbEnabled[gbEnabledCount++] = i;
    if (gbEnabledCount == 0) {           // installer somehow masked all: fail safe
        for (int i = 0; i < GUIDE_COUNT; i++) gbEnabled[gbEnabledCount++] = i;
    }
    if (gbSel >= gbEnabledCount) gbSel = 0;
}

void guidebooksEnter() {
    chordGuardReset();
    fb.setRotation(0);
    gbResolveEnabled();
    gbSel = 0; gbTop = 0;
    gbInGuide = false;
}

void guidebooksShutdown() {
    if (gbInGuide) contentShutdown();
    gbInGuide = false;
}

// The content reader owns navigation once a guide is open. We detect the
// "back out of the guide" gesture (L held / tapped while on the reader's own
// index screen) by giving the reader a chance to run, then checking whether it
// wants to hand control back. The reader doesn't expose that directly, so we
// use a simpler contract: while in a guide, L from the reader's index returns
// to the GuideBooks list. contentTick handles all in-guide navigation; here we
// only intercept the case where the user wants to leave the guide entirely.
//
// Practically: the reader already treats L as "back to index" inside a guide.
// To exit the guide back to the GuideBooks list, the user double-taps L on the
// index (first tap = already at index / no-op scroll, second = leave). To keep
// it dead simple and predictable we instead reserve a dedicated gesture: hold
// L for ~600ms while in a guide returns to the GuideBooks list. This never
// conflicts with the reader (which uses short taps).
static uint32_t gbLHoldStart = 0;
static bool     gbLHolding   = false;

void guidebooksTick() {
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();

    if (!gbInGuide) {
        // ---- GuideBooks list navigation ----
        if (lEdge) {
            gbSel = (gbSel + 1) % gbEnabledCount;
            // keep selection visible in the scroll window
            const int visible = 5;
            if (gbSel < gbTop) gbTop = gbSel;
            if (gbSel >= gbTop + visible) gbTop = gbSel - visible + 1;
        }
        if (rEdge) {
            const GuideDef& g = GUIDES[gbEnabled[gbSel]];
            contentEnter(&g.mode);
            gbInGuide = true;
            gbLHolding = false; gbLHoldStart = 0;
        }
        return;
    }

    // ---- Inside a guide: reader handles taps; hold-L exits to the list ----
    uint32_t now = millis();
    if (btnL && !gbLHolding) { gbLHolding = true; gbLHoldStart = now; }
    if (!btnL && gbLHolding) { gbLHolding = false; }
    if (gbLHolding && (now - gbLHoldStart) >= 600) {
        // Leave the guide, back to the GuideBooks list. Swallow the release so
        // the reader doesn't also act on it.
        contentShutdown();
        gbInGuide = false;
        gbLHolding = false; gbLHoldStart = 0;
        // Consume this frame's L so the list doesn't immediately advance.
        return;
    }

    contentTick();
}

void guidebooksDraw() {
    if (gbInGuide) { contentDraw(); return; }

    // ---- GuideBooks list ----
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("GUIDEBOOKS", 4, 9, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT);
    char cnt[12]; snprintf(cnt, sizeof(cnt), "%d/%d", gbSel + 1, gbEnabledCount);
    fb.drawString(cnt, W - 4, 9, 1);

    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("CHOOSE A GUIDE", W / 2, 30, 1);

    const int visible = 5, rowH = 44, top = 44;
    if (gbSel < gbTop) gbTop = gbSel;
    if (gbSel >= gbTop + visible) gbTop = gbSel - visible + 1;

    for (int r = 0; r < visible && gbTop + r < gbEnabledCount; r++) {
        int gi = gbEnabled[gbTop + r];
        const GuideDef& g = GUIDES[gi];
        int y = top + r * rowH;
        bool sel = (gbTop + r == gbSel);
        if (sel) fb.fillRoundRect(6, y - 2, W - 12, rowH - 6, 4, PURPLE_DARK);
        // accent chip
        fb.fillRoundRect(12, y + 6, 6, rowH - 18, 2, g.mode.accent);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(sel ? LIME_PALE : LIME);
        fb.drawString(g.title, 26, y + 12, 2);
        fb.setTextColor(sel ? LIME : TEAL_DARK);
        fb.drawString(sel ? "> open" : "read", 26, y + 28, 1);
    }

    if (gbTop > 0) { fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("^", W / 2, 40, 1); }
    if (gbTop + visible < gbEnabledCount) { fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_DARK); fb.drawString("v", W / 2, top + visible * rowH - 4, 1); }

    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L next", 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM);
    fb.drawString("R open  holdL exits", W - 4, H - 7, 1);

    fb.pushSprite(0, 0);
}
