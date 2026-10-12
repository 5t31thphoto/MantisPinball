// FILE: src/sightings/sightings_mode.cpp
// SIGHTINGS - the life list. A field naturalist's collection for the whole
// western outdoors: 150 species across 7 categories, each with real rendered
// pixel art streamed from a packed atlas on the content partition.
//
// ART PIPELINE: tools/gen_sightings.py renders every species as authored
// 24x24 pixel art (silhouette + shading + outline pass), quantizes each
// category to a shared 15-color palette, and packs /content/sightings.sbi:
//     "SBI1" u8 catCount
//     per category: u8 palCount, u16 pal[palCount] (rgb565),
//                   u8 iconCount, iconCount * 288 bytes (24x24 @ 4bpp)
// It also regenerates sightings_data.h (the species table).
//
// MEMORY: one category's icon block is malloc'd when you open it and freed
// when you leave it (max 31 icons * 288 B = 8.9 KB, inside the no-network
// budget). Palettes are tiny and load once. Nothing networked, radio never
// touched: a life list is a solo pursuit and the battery agrees.
//
// PERSISTENCE: one packed blob in NVS "sightings" behind a dirty flag with a
// 10 s minimum write interval + forced flush on shutdown. Checking off a
// sighting is a discrete event, so a day in the field costs a handful of
// writes.
//
// QUEST PROFILE: each check-off pays that species' XP (a discrete milestone,
// same shape as CYOA page grants / the aquarium's new-species grant). Life
// list milestones and category completions pay bonus XP, pet XP, and items.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "sightings_mode.h"
#include "sightings_data.h"
#include "../power_guard.h"
#include "../chord_guard.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

static constexpr int  ICON_SZ    = 24;
static constexpr int  ICON_BYTES = 288;             // 24*24 / 2 (4bpp)
static constexpr int  MAX_PAL    = 15;
static const char*    SBI_PATH   = "/content/sightings.sbi";
static constexpr uint32_t SG_LONG_MS     = 420;
static constexpr uint32_t SG_ENTER_GUARD = 250;

// ─────────────────────────────────────────────────────────────────────────────
//  MILESTONES  (the life list is the point)
// ─────────────────────────────────────────────────────────────────────────────
struct SgMilestone { uint8_t at; const char* name; const char* reward; };
static const SgMilestone MILES[] = {
    {  10, "FIRST STEPS",   "+50 XP"            },
    {  25, "FIELD HAND",    "Aquarium coins"    },
    {  50, "NATURALIST",    "+150 XP, decor"    },
    {  75, "TRACKER",       "Luck charm"        },
    { 100, "EXPERT EYE",    "Rare fish"         },
    { 125, "SAGE",          "+200 XP"           },
    { 150, "LIFE LIST",     "Legend status"     },
};
static constexpr int NUM_MILES = sizeof(MILES) / sizeof(MILES[0]);

// ─────────────────────────────────────────────────────────────────────────────
//  PERSISTENCE
// ─────────────────────────────────────────────────────────────────────────────
static const char* NVS_SG = "sightings";
static constexpr uint16_t SG_MAGIC = 0x4753;   // 'SG'
static constexpr uint8_t  SG_VER   = 1;

struct __attribute__((packed)) SgSave {
    uint16_t magic;
    uint8_t  version;
    uint8_t  seen[(SG_COUNT + 7) / 8];   // 19 bytes for 150 species
    uint8_t  milesDone;                  // bitmask of paid milestones
    uint8_t  catsDone;                   // bitmask of paid category bonuses
    uint32_t firstSeenDay;               // reserved for future streaks
};
static SgSave sv;
static bool     svDirty     = false;
static uint32_t svLastWrite = 0;

static void sgDefaults() {
    memset(&sv, 0, sizeof(sv));
    sv.magic = SG_MAGIC;
    sv.version = SG_VER;
}

static void sgLoad() {
    sgDefaults();
    Preferences p;
    if (!p.begin(NVS_SG, true)) return;
    SgSave tmp;
    size_t n = p.getBytes("blob", &tmp, sizeof(tmp));
    p.end();
    if (n == sizeof(tmp) && tmp.magic == SG_MAGIC && tmp.version == SG_VER) sv = tmp;
}

static void sgFlush() {
    Preferences p;
    if (!p.begin(NVS_SG, false)) return;
    p.putBytes("blob", &sv, sizeof(sv));
    p.end();
    svDirty = false;
    svLastWrite = millis();
}


static bool sgSeen(int i)   { return (sv.seen[i >> 3] >> (i & 7)) & 1; }
static void sgSetSeen(int i){ sv.seen[i >> 3] |= (uint8_t)(1 << (i & 7)); }

static int sgSeenTotal() {
    int n = 0;
    for (int i = 0; i < SG_COUNT; i++) if (sgSeen(i)) n++;
    return n;
}
static int sgSeenInCat(int c) {
    int n = 0;
    for (int i = 0; i < SG_COUNT; i++) if (SG[i].cat == c && sgSeen(i)) n++;
    return n;
}
static int sgTotalInCat(int c) {
    int n = 0;
    for (int i = 0; i < SG_COUNT; i++) if (SG[i].cat == c) n++;
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ATLAS  (palettes once; one category's icons cached on demand)
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t sgPal[SG_CAT_COUNT][MAX_PAL];
static uint8_t  sgPalCount[SG_CAT_COUNT];
static uint8_t  sgIconCount[SG_CAT_COUNT];
static uint32_t sgCatOffset[SG_CAT_COUNT];   // byte offset of each icon block
static bool     sgAtlasOk = false;

static uint8_t* sgCache    = nullptr;        // current category's icon block
static int      sgCacheCat = -1;

static bool sgFsMount() {
    static bool mounted = false;
    if (mounted) return true;
    // The FS partition is named "content" (see partitions_16MB_pinball.csv),
    // NOT the LittleFS default label "spiffs" - mount it exactly the way
    // content_reader.cpp and the aquarium do, or the atlas is never found and
    // every species falls back to a letter chip.
    mounted = LittleFS.begin(true, "/littlefs", 10, "content");
    return mounted;
}

static bool sgAtlasIndex() {
    if (!sgFsMount()) return false;
    fs::File f = LittleFS.open(SBI_PATH, "r");
    if (!f) return false;
    uint8_t hdr[5];
    if (f.read(hdr, 5) != 5 || memcmp(hdr, "SBI1", 4) != 0) { f.close(); return false; }
    int cats = hdr[4];
    if (cats != SG_CAT_COUNT) { f.close(); return false; }
    for (int c = 0; c < cats; c++) {
        int pc = f.read();
        if (pc < 0 || pc > MAX_PAL) { f.close(); return false; }
        sgPalCount[c] = (uint8_t)pc;
        for (int i = 0; i < pc; i++) {
            int lo = f.read(), hi = f.read();
            sgPal[c][i] = (uint16_t)lo | ((uint16_t)hi << 8);
        }
        int ic = f.read();
        if (ic < 0) { f.close(); return false; }
        sgIconCount[c] = (uint8_t)ic;
        sgCatOffset[c] = f.position();
        if (!f.seek(sgCatOffset[c] + (uint32_t)ic * ICON_BYTES)) { f.close(); return false; }
    }
    f.close();
    return true;
}

static void sgCacheFree() {
    if (sgCache) { free(sgCache); sgCache = nullptr; }
    sgCacheCat = -1;
}

static bool sgCacheLoad(int cat) {
    if (cat == sgCacheCat && sgCache) return true;
    sgCacheFree();
    if (!sgAtlasOk || cat < 0 || cat >= SG_CAT_COUNT) return false;
    fs::File f = LittleFS.open(SBI_PATH, "r");
    if (!f) return false;
    size_t need = (size_t)sgIconCount[cat] * ICON_BYTES;
    sgCache = (uint8_t*)malloc(need);
    if (!sgCache) { f.close(); return false; }
    bool ok = f.seek(sgCatOffset[cat]) && f.read(sgCache, need) == need;
    f.close();
    if (!ok) { sgCacheFree(); return false; }
    sgCacheCat = cat;
    return true;
}

// Draw an icon from the cache at integer scale 1 or 2. Falls back to a badge
// (category initial in a rounded chip) if the atlas is missing.
static void sgDrawIcon(int cat, int icon, int x, int y, int scale, bool dim) {
    if (sgCache && sgCacheCat == cat && icon < sgIconCount[cat]) {
        const uint8_t* p = sgCache + (size_t)icon * ICON_BYTES;
        const uint16_t* pal = sgPal[cat];
        int pc = sgPalCount[cat];
        for (int py = 0; py < ICON_SZ; py++) {
            for (int px = 0; px < ICON_SZ; px += 2) {
                uint8_t b = *p++;
                uint8_t v0 = b & 0x0F, v1 = b >> 4;
                for (int k = 0; k < 2; k++) {
                    uint8_t v = k ? v1 : v0;
                    if (!v || v > pc) continue;
                    uint16_t col = pal[v - 1];
                    if (dim) {
                        uint8_t r=(col>>11)&0x1F, g=(col>>5)&0x3F, bl=col&0x1F;
                        col = (uint16_t)(((r/3)<<11)|((g/3)<<5)|(bl/3));
                    }
                    int dx = x + (px + k) * scale;
                    int dy = y + py * scale;
                    if (scale == 1) fb.drawPixel(dx, dy, col);
                    else            fb.fillRect(dx, dy, scale, scale, col);
                }
            }
        }
    } else {
        // graceful fallback: colored chip with the category initial
        int s = ICON_SZ * scale;
        uint16_t c = dim ? TEAL_DARK : TEAL_BRIGHT;
        fb.drawRoundRect(x, y, s, s, 4, c);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(c);
        char b[2] = { SG_CAT_NAMES[cat][0], 0 };
        fb.drawString(b, x + s / 2, y + s / 2, scale == 1 ? 1 : 2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI STATE
// ─────────────────────────────────────────────────────────────────────────────
enum SgScreen : uint8_t { SC_HOME = 0, SC_LIST, SC_DETAIL, SC_CELEB, SC_MILES };
static SgScreen scr = SC_HOME;

static int      homeSel   = 0;                 // 0..SG_CAT_COUNT-1 cats, then MILESTONES tile
static int      curCat    = 0;
static int      listSel   = 0;                 // index within category
static int      listTop   = 0;
static uint32_t animT     = 0;
static uint32_t enterAt   = 0;
static int      celebIdx  = -1;                // species just checked
static int      celebXp   = 0;
static int      celebMile = -1;                // milestone hit during this check
static bool     celebCat  = false;             // category completed?
static uint32_t idleT     = 0;                 // for the home screen ambience

// species indices for the open category (built on open; max 31)
static uint8_t  catList[40];
static int      catListN = 0;

static void sgBuildCatList(int cat) {
    catListN = 0;
    for (int i = 0; i < SG_COUNT && catListN < 40; i++)
        if (SG[i].cat == cat) catList[catListN++] = (uint8_t)i;
}

// ─────────────────────────────────────────────────────────────────────────────
//  INPUT (tap on release; long fires once while held)
// ─────────────────────────────────────────────────────────────────────────────
static bool lTap, rTap, lHoldFire, rHoldFire;
static bool lDown, rDown, lLongDone, rLongDone;
static uint32_t lDownAt, rDownAt;

static void sgInput() {
    chordGuardTick();
    lTap = rTap = lHoldFire = rHoldFire = false;
    uint32_t now = millis();
    if (chordLEdge()) { lDown = true; lDownAt = now; lLongDone = false; }
    if (chordLHeld() && lDown && !lLongDone && now - lDownAt > SG_LONG_MS) { lLongDone = true; lHoldFire = true; }
    if (!btnL && btnL_prev) { if (lDown && !lLongDone) lTap = true; lDown = false; }
    if (chordREdge()) { rDown = true; rDownAt = now; rLongDone = false; }
    if (chordRHeld() && rDown && !rLongDone && now - rDownAt > SG_LONG_MS) { rLongDone = true; rHoldFire = true; }
    if (!btnR && btnR_prev) { if (rDown && !rLongDone) rTap = true; rDown = false; }
    if (now - enterAt < SG_ENTER_GUARD) { lTap = rTap = lHoldFire = rHoldFire = false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  REWARD LOGIC
// ─────────────────────────────────────────────────────────────────────────────
static const char* sgHabName(uint8_t h) {
    switch (h) {
        case SG_MTN: return "MOUNTAINS";
        case SG_FOR: return "FOREST";
        case SG_RIV: return "RIVER & LAKE";
        case SG_DES: return "DESERT";
        case SG_CST: return "COAST";
        case SG_TID: return "TIDE POOL";
        case SG_MDW: return "MEADOW";
        default:     return "ANYWHERE";
    }
}

static void sgPayMilestone(int m) {
    switch (MILES[m].at) {
        case 10:  qpAddXp(50); break;
        case 25:  qpAddItem(ITEM_AQ_COINPACK, 1); qpAddXp(25); break;
        case 50:  qpAddXp(150); qpAddItem(ITEM_AQ_DECOR, 1); break;
        case 75:  qpAddItem(ITEM_CY_LUCKCHARM, 1); qpAddXp(50); break;
        case 100: qpAddItem(ITEM_AQ_RAREFISH, 1); qpAddXp(100); break;
        case 125: qpAddXp(200); break;
        case 150: qpAddXp(400); qpGrantPetXp(100); break;
    }
    qpGrantPetXp(15);
}

// Check off the selected species. Pays XP, tests milestones + category
// completion, and stages the celebration screen.
static void sgCheckOff(int idx) {
    if (idx < 0 || idx >= SG_COUNT || sgSeen(idx)) return;
    sgSetSeen(idx);
    svDirty = true;
    sgFlush();          // a new sighting is a discrete checkpoint: write once

    const SgSpecies& sp = SG[idx];
    celebIdx = idx;
    celebXp = sp.xp;
    celebMile = -1;
    celebCat = false;
    qpAddXp(sp.xp);
    if (sp.rarity >= 4) qpGrantPetXp(10);

    int total = sgSeenTotal();
    for (int m = 0; m < NUM_MILES; m++) {
        if (total >= MILES[m].at && !((sv.milesDone >> m) & 1)) {
            sv.milesDone |= (uint8_t)(1 << m);
            sgPayMilestone(m);
            celebMile = m;
            break;                               // one fanfare per check-off
        }
    }
    int c = sp.cat;
    if (sgSeenInCat(c) == sgTotalInCat(c) && !((sv.catsDone >> c) & 1)) {
        sv.catsDone |= (uint8_t)(1 << c);
        qpAddXp(100);
        qpGrantPetXp(25);
        celebCat = true;
    }

    animT = 0;
    scr = SC_CELEB;
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

static void drawBar(int x, int y, int w, int h, int val, int max, uint16_t fg) {
    fb.fillRect(x, y, w, h, TEAL_DARK);
    if (max > 0) {
        int pw = (val * w) / max;
        if (pw > w) pw = w;
        if (pw > 0) fb.fillRect(x, y, pw, h, fg);
    }
    fb.drawRect(x, y, w, h, TEAL);
}

static void drawRarity(int x, int y, int rarity, uint16_t col) {
    for (int i = 0; i < rarity; i++) fb.fillCircle(x + i * 7, y, 2, col);
}

// The home-screen ambience: a slow mountain ridge with drifting birds. This is
// scenery, not iconography — the species art all comes from the atlas.
static void drawRidge(uint32_t t) {
    int base = 66;
    for (int x = 0; x < W; x++) {
        float fx = x * 0.055f;
        int y = base - (int)(sinf(fx) * 9 + sinf(fx * 2.3f + 1.2f) * 5);
        fb.drawPixel(x, y, TEAL);
        for (int yy = y + 1; yy < base + 12; yy++) fb.drawPixel(x, yy, TEAL_DARK);
    }
    // sun
    fb.fillCircle(W - 30, 32, 7, LIME_PALE);
    fb.drawCircle(W - 30, 32, 9, LIME_DARK);
    // two drifting birds
    for (int b = 0; b < 2; b++) {
        int bx = (int)((t / (3 + b)) % (W + 30)) - 15;
        int by = 28 + b * 10 + (int)(sinf(t * 0.03f + b) * 2);
        fb.drawLine(bx - 3, by, bx, by - 2, TEAL_LIGHT);
        fb.drawLine(bx, by - 2, bx + 3, by, TEAL_LIGHT);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SCREENS
// ─────────────────────────────────────────────────────────────────────────────
static void drawHome() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("SIGHTINGS");
    drawRidge(idleT);

    int total = sgSeenTotal();
    char b[32];
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "LIFE LIST  %d / %d", total, SG_COUNT);
    fb.drawString(b, 6, 88, 2);
    drawBar(6, 98, W - 12, 5, total, SG_COUNT, LIME);

    // category tiles: 2 columns x 4 rows (7 cats + MILESTONES)
    const int cols = 2, cw = (W - 12) / cols, ch = 46;
    for (int i = 0; i < SG_CAT_COUNT + 1; i++) {
        int cx = 4 + (i % cols) * (cw + 4);
        int cy = 110 + (i / cols) * (ch + 3);
        bool sel = (i == homeSel);
        bool isMiles = (i == SG_CAT_COUNT);
        fb.fillRoundRect(cx, cy, cw, ch, 4, sel ? TEAL_DARK : TEAL_DEEP);
        fb.drawRoundRect(cx, cy, cw, ch, 4, sel ? LIME : TEAL_DARK);
        fb.setTextDatum(ML_DATUM);
        if (isMiles) {
            fb.setTextColor(sel ? LIME_PALE : PURPLE_LIGHT);
            fb.drawString("MILESTONES", cx + 6, cy + 14, 1);
            int paid = 0;
            for (int m = 0; m < NUM_MILES; m++) if ((sv.milesDone >> m) & 1) paid++;
            fb.setTextColor(sel ? LIME : TEAL_LIGHT);
            snprintf(b, sizeof(b), "%d / %d", paid, NUM_MILES);
            fb.drawString(b, cx + 6, cy + 30, 2);
        } else {
            int s = sgSeenInCat(i), t = sgTotalInCat(i);
            bool done = (s == t);
            fb.setTextColor(sel ? LIME_PALE : (done ? LIME : TEAL_BRIGHT));
            fb.drawString(SG_CAT_NAMES[i], cx + 6, cy + 10, 1);
            fb.setTextColor(sel ? LIME : TEAL_LIGHT);
            snprintf(b, sizeof(b), "%d/%d", s, t);
            fb.drawString(b, cx + 6, cy + 26, 2);
            drawBar(cx + 6, cy + 37, cw - 12, 3, s, t, done ? LIME : TEAL_BRIGHT);
        }
    }
    drawFooter("L: next", "R: open");
}

static void drawList() {
    fb.fillSprite(TEAL_DEEP);
    char t[24];
    snprintf(t, sizeof(t), "%s", SG_CAT_NAMES[curCat]);
    drawHeader(t);

    char b[28];
    fb.setTextDatum(MR_DATUM);
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b, sizeof(b), "%d/%d", sgSeenInCat(curCat), sgTotalInCat(curCat));
    fb.drawString(b, W - 4, 9, 1);

    const int ROW = 29, TOP = 22;
    const int VIS = (H - TOP - 14) / ROW;          // 9 rows
    if (listSel < listTop) listTop = listSel;
    if (listSel >= listTop + VIS) listTop = listSel - VIS + 1;

    for (int r = 0; r < VIS && (listTop + r) < catListN; r++) {
        int i = listTop + r;
        int y = TOP + r * ROW;
        int sp = catList[i];
        bool sel = (i == listSel);
        bool seen = sgSeen(sp);
        if (sel) {
            fb.fillRoundRect(1, y, W - 2, ROW - 2, 3, PURPLE_DARK);
            fb.drawRoundRect(1, y, W - 2, ROW - 2, 3, LIME_DARK);
        }
        sgDrawIcon(curCat, SG[sp].icon, 4, y + 1, 1, !seen && !sel);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(seen ? LIME_PALE : (sel ? TEAL_LIGHT : TEAL_BRIGHT));
        fb.drawString(SG[sp].name, 32, y + 9, 1);
        fb.setTextColor(TEAL_DARK);
        fb.drawString(sgHabName(SG[sp].hab), 32, y + 20, 1);
        if (seen) {
            fb.setTextDatum(MR_DATUM);
            fb.setTextColor(LIME);
            fb.drawString("SEEN", W - 6, y + 9, 1);
        } else {
            drawRarity(W - 6 - (SG[sp].rarity - 1) * 7, y + 9, SG[sp].rarity, PURPLE_LIGHT);
        }
    }

    // scrollbar
    if (catListN > VIS) {
        int bh = (VIS * (H - TOP - 14)) / catListN;
        int by = TOP + (listTop * (H - TOP - 14 - bh)) / (catListN - VIS);
        fb.fillRect(W - 2, by, 2, bh, TEAL_LIGHT);
    }
    drawFooter("L: next  hold: back", "R: view");
}

static void drawDetail() {
    int sp = catList[listSel];
    const SgSpecies& s = SG[sp];
    bool seen = sgSeen(sp);

    fb.fillSprite(TEAL_DEEP);
    drawHeader(SG_CAT_NAMES[curCat]);

    // hero panel with the 2x sprite
    fb.fillRoundRect(8, 26, W - 16, 92, 6, TEAL_DARK);
    fb.drawRoundRect(8, 26, W - 16, 92, 6, seen ? LIME_DARK : TEAL);
    // soft vignette corners
    fb.drawRoundRect(9, 27, W - 18, 90, 6, TEAL_DEEP);
    sgDrawIcon(curCat, s.icon, W / 2 - 24, 40, 2, false);
    if (seen) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(LIME);
        fb.drawString("ON THE LIFE LIST", W / 2, 108, 1);
    }

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(s.name, W / 2, 134, 2);

    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(sgHabName(s.hab), W / 2, 154, 1);

    drawRarity(W / 2 - (s.rarity - 1) * 3 - 2, 170, s.rarity, PURPLE_LIGHT);
    static const char* RLBL[5] = { "", "COMMON", "UNCOMMON", "RARE", "LEGENDARY" };
    fb.setTextColor(TEAL_DARK);
    fb.drawString(RLBL[s.rarity], W / 2, 182, 1);

    char b[20];
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b, sizeof(b), "%d XP", s.xp);
    fb.drawString(b, W / 2, 200, 2);

    if (!seen) {
        fb.fillRoundRect(20, 224, W - 40, 34, 5, PURPLE_DARK);
        fb.drawRoundRect(20, 224, W - 40, 34, 5, LIME);
        fb.setTextColor(LIME);
        fb.drawString("I SAW IT!", W / 2, 241, 2);
        drawFooter("L: back", "R: check off");
    } else {
        drawFooter("L: back", "R: back");
    }
}

static void drawCeleb() {
    int sp = celebIdx;
    const SgSpecies& s = SG[sp];
    fb.fillSprite(TEAL_DEEP);

    // starburst rays behind the panel
    for (int r = 0; r < 10; r++) {
        float a = r * 0.628f + animT * 0.03f;
        int rr = 30 + (int)(animT * 1.4f);
        if (rr > 90) rr = 90;
        fb.drawLine(W / 2, 96, W / 2 + (int)(cosf(a) * rr), 96 + (int)(sinf(a) * rr), TEAL_DARK);
    }

    fb.fillRoundRect(14, 40, W - 28, 128, 8, TEAL_DEEP);
    fb.drawRoundRect(14, 40, W - 28, 128, 8, LIME);
    fb.drawRoundRect(15, 41, W - 30, 126, 8, LIME_DARK);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(animT < 14 ? "SPOTTED!" : "LIFE LIST +1", W / 2, 56, 2);

    sgDrawIcon(curCat, s.icon, W / 2 - 24, 68, 2, false);

    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(s.name, W / 2, 126, 1);

    char b[20];
    snprintf(b, sizeof(b), "+%d XP", celebXp);
    fb.setTextColor(TEAL_BRIGHT);
    int bob = 146 + (int)(sinf(animT * 0.22f) * 3.0f);
    fb.drawString(b, W / 2, bob, 2);

    // sparkles orbit the panel
    for (int k = 0; k < 5; k++) {
        float a = animT * 0.06f + k * 1.256f;
        int sx = W / 2 + (int)(cosf(a) * 66);
        int sy = 100 + (int)(sinf(a) * 58);
        fb.drawPixel(sx, sy, LIME_PALE);
        fb.drawPixel(sx + 1, sy, LIME_PALE);
        fb.drawPixel(sx, sy + 1, LIME_PALE);
    }

    int y = 176;
    if (celebMile >= 0) {
        fb.fillRoundRect(12, y, W - 24, 38, 5, PURPLE_DARK);
        fb.drawRoundRect(12, y, W - 24, 38, 5, PURPLE_LIGHT);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("MILESTONE!", W / 2, y + 11, 1);
        fb.setTextColor(LIME_PALE);
        char m[40];
        snprintf(m, sizeof(m), "%s  -  %s", MILES[celebMile].name, MILES[celebMile].reward);
        fb.drawString(m, W / 2, y + 27, 1);
        y += 44;
    }
    if (celebCat) {
        fb.fillRoundRect(12, y, W - 24, 32, 5, PURPLE_DARK);
        fb.drawRoundRect(12, y, W - 24, 32, 5, LIME);
        fb.setTextColor(LIME);
        char m[40];
        snprintf(m, sizeof(m), "%s COMPLETE  +100 XP", SG_CAT_NAMES[curCat]);
        fb.drawString(m, W / 2, y + 16, 1);
    }
    drawFooter("", "L/R: continue");
}

static void drawMiles() {
    fb.fillSprite(TEAL_DEEP);
    drawHeader("MILESTONES");

    int total = sgSeenTotal();
    char b[32];
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME_PALE);
    snprintf(b, sizeof(b), "LIFE LIST  %d / %d", total, SG_COUNT);
    fb.drawString(b, 6, 26, 2);
    drawBar(6, 36, W - 12, 5, total, SG_COUNT, LIME);

    for (int m = 0; m < NUM_MILES; m++) {
        int y = 50 + m * 34;
        bool paid = (sv.milesDone >> m) & 1;
        bool next = !paid && (m == 0 || ((sv.milesDone >> (m - 1)) & 1));
        fb.fillRoundRect(4, y, W - 8, 31, 4, paid ? TEAL_DARK : TEAL_DEEP);
        fb.drawRoundRect(4, y, W - 8, 31, 4, paid ? LIME_DARK : (next ? TEAL_BRIGHT : TEAL_DARK));
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(paid ? LIME : (next ? TEAL_LIGHT : TEAL_DARK));
        snprintf(b, sizeof(b), "%d  %s", MILES[m].at, MILES[m].name);
        fb.drawString(b, 10, y + 9, 1);
        fb.setTextColor(paid ? LIME_PALE : TEAL_DARK);
        fb.drawString(MILES[m].reward, 10, y + 22, 1);
        if (paid) {
            fb.setTextDatum(MR_DATUM);
            fb.setTextColor(LIME);
            fb.drawString("PAID", W - 10, y + 15, 1);
        } else if (next) {
            fb.setTextDatum(MR_DATUM);
            fb.setTextColor(TEAL_BRIGHT);
            snprintf(b, sizeof(b), "%d to go", MILES[m].at - total > 0 ? MILES[m].at - total : 0);
            fb.drawString(b, W - 10, y + 15, 1);
        }
    }
    drawFooter("L/R: back", "");
}

// ─────────────────────────────────────────────────────────────────────────────
//  LIFECYCLE
// ─────────────────────────────────────────────────────────────────────────────
void sightingsEnter() {
    chordGuardReset();
    pgRegisterFlush(sgFlush);
    fb.setRotation(0);
    sgLoad();
    sgAtlasOk = sgAtlasIndex();
    scr = SC_HOME;
    homeSel = 0;
    curCat = 0;
    listSel = 0;
    listTop = 0;
    celebIdx = -1;
    animT = 0;
    idleT = 0;
    enterAt = millis();
    svLastWrite = millis();
    svDirty = false;
    lDown = rDown = false;
    lLongDone = rLongDone = false;
    sgCache = nullptr;
    sgCacheCat = -1;
}

void sightingsShutdown() {
    sgCacheFree();
    if (svDirty) sgFlush();
}

void sightingsTick() {
    sgInput();
    idleT++;

    switch (scr) {

    case SC_HOME:
        if (lTap) homeSel = (homeSel + 1) % (SG_CAT_COUNT + 1);
        if (rTap) {
            if (homeSel == SG_CAT_COUNT) { scr = SC_MILES; }
            else {
                curCat = homeSel;
                sgBuildCatList(curCat);
                sgCacheLoad(curCat);
                listSel = 0;
                listTop = 0;
                scr = SC_LIST;
            }
        }
        break;

    case SC_LIST:
        if (lTap) listSel = (listSel + 1) % catListN;
        if (lHoldFire) { sgCacheFree(); scr = SC_HOME; }
        if (rTap) scr = SC_DETAIL;
        break;

    case SC_DETAIL:
        if (lTap) scr = SC_LIST;
        if (rTap) {
            int sp = catList[listSel];
            if (!sgSeen(sp)) sgCheckOff(sp);
            else scr = SC_LIST;
        }
        break;

    case SC_CELEB:
        animT++;
        if (animT > 110 || lTap || rTap) {           // ~1.8 s of fanfare
            animT = 0;
            scr = SC_LIST;
        }
        break;

    case SC_MILES:
        if (lTap || rTap) scr = SC_HOME;
        break;
    }

}

void sightingsDraw() {
    switch (scr) {
    case SC_HOME:   drawHome();   break;
    case SC_LIST:   drawList();   break;
    case SC_DETAIL: drawDetail(); break;
    case SC_CELEB:  drawCeleb();  break;
    case SC_MILES:  drawMiles();  break;
    }
    fb.pushSprite(0, 0);
}
