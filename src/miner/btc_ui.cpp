// ============================================================================
//  btc_ui.cpp
//
//  Landscape (rotation 3, 320x170) UI for the BTC MINER mode.
//
//  Layout per spec:
//
//    Zone 1 (x =   0..119)  Desk clock + date + BTC price ticker
//    Zone 2 (x = 120..194)  Three slot reels, framed
//    Zone 3 (x = 195..319)  Telemetry: target bounty, hashrate, nonce, diff
//
//  Slot reel near-miss heuristic, share-difficulty driven:
//
//    ratio = last_share_diff / session_best_diff
//      < 0.10    MISS landing  (3 random low symbols)
//      0.10-0.49 CLOSE landing (1 matching pair somewhere)
//      0.50-0.99 NEAR-MISS     (2 match + off-by-one third)  <-- the tease
//      >= 1.00   NEW BEST      (3 match + lime flash)
//      block_solved BLOCK WIN  (DOGE-DOGE-DOGE persistent, NVS-stored)
//
//  Everything in this file uses the project's TEAL/PURPLE/LIME palette so
//  miner mode looks like a piece of the same device, not a separate app.
// ============================================================================

#include "btc_miner.h"
#include "btc_miner_internal.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

// rgb565 is defined in main.cpp; theme_colors.h needs it. Provide our own
// rgb565 is defined in theme_colors.h
#include "../theme_colors.h"
#include "../build_config.h"

// Local copies of the game-element aliases used in main.cpp. These match
// the values main.cpp picks so the miner UI feels visually identical to
// the rest of the firmware.
#define C_BG (TEAL_DEEP)
#define C_WALL (TEAL)
#define C_HUD_DIM (TEAL_BRIGHT)

// Forward decl of the framebuffer sprite from main.cpp - we don't reuse it
// in landscape because of dimension mismatch, we allocate our own.

// ----------------------------------------------------------------------------
//  Landscape framebuffer
// ----------------------------------------------------------------------------
static TFT_eSPI* sTft = nullptr;
static TFT_eSprite* sFb = nullptr;
static constexpr int LW = 320;
static constexpr int LH = 170;
static bool sFbOwned = false;
static uint8_t sSavedRotation = 0;

// Reel state - 3 reels, each cycles through 8 symbol indices.
// Symbols are drawn as characters from a curated set so they're easy to
// distinguish at small size.
static constexpr int REEL_COUNT  = 3;
static constexpr int SYMBOL_COUNT = 8;
// 7 normal symbols + the DOGE_WIN symbol at index 7 (only landed on block solve)
static const char* SYMBOLS[SYMBOL_COUNT] = {
    "$", "B", "X", "K", "C", "T", "H", "D"
};

struct ReelState {
    int     shown;        // currently displayed symbol index
    int     target;       // index it's settling on
    int     spinFrames;   // frames left of spin animation
    int     stopAt;       // when spinFrames hits this we land
};
static ReelState reels[REEL_COUNT];

enum LandingKind {
    LAND_IDLE,
    LAND_MISS,
    LAND_CLOSE,
    LAND_NEAR,
    LAND_BEST,
    LAND_BLOCK_WIN,
};
static LandingKind lastLanding = LAND_IDLE;
static uint32_t    landingShownAt = 0;
static uint32_t    blockWinShownAt = 0;
static double      sessionAvgDiff = 0;
static uint32_t    sessionShareCount = 0;

// Decide what symbols to land on, based on share difficulty bucket.
static void planLanding(double share_diff, double best_diff, bool block_solved) {
    if (block_solved) {
        for (int i = 0; i < REEL_COUNT; ++i) reels[i].target = 7;   // D D D
        lastLanding = LAND_BLOCK_WIN;
        return;
    }
    double ratio = (best_diff > 0.0) ? (share_diff / best_diff) : 0.0;
    int seed = (int)esp_random();
    auto pick = [&](int lo, int hi) {
        return lo + (esp_random() % (hi - lo + 1));
    };
    if (ratio < 0.10) {
        // MISS - three random symbols, low alphabet, intentionally unmatched
        reels[0].target = pick(0, 2);
        reels[1].target = pick(3, 5);
        reels[2].target = pick(0, 5);
        // ensure no accidental three-match
        if (reels[0].target == reels[1].target && reels[1].target == reels[2].target) {
            reels[2].target = (reels[2].target + 1) % 7;
        }
        lastLanding = LAND_MISS;
    } else if (ratio < 0.50) {
        // CLOSE - one matching pair somewhere, third different
        int s = pick(0, 6);
        reels[0].target = s;
        reels[1].target = s;
        reels[2].target = (s + 1 + (int)(esp_random() % 5)) % 7;
        lastLanding = LAND_CLOSE;
    } else if (ratio < 1.00) {
        // NEAR-MISS - two match, third is off by exactly one
        int s = pick(0, 6);
        reels[0].target = s;
        reels[1].target = s;
        reels[2].target = (s + 1) % 7;
        lastLanding = LAND_NEAR;
    } else {
        // NEW BEST - three matching, non-DOGE symbol
        int s = pick(0, 6);
        reels[0].target = s;
        reels[1].target = s;
        reels[2].target = s;
        lastLanding = LAND_BEST;
    }
    (void)seed;
}

// Start the reels spinning. Each reel stops at a staggered point so the
// player perceives the suspense build like a real slot.
static void triggerSpin(double share_diff, double best_diff, bool block_solved) {
    planLanding(share_diff, best_diff, block_solved);
    for (int i = 0; i < REEL_COUNT; ++i) {
        reels[i].spinFrames = 22 + i * 8;     // ~22, 30, 38 frames (~0.4-0.7s)
        reels[i].stopAt     = 0;
    }
    landingShownAt = millis();
    if (block_solved) blockWinShownAt = millis();
}

// Per-frame reel tick. Cycles symbols rapidly while spinning, then snaps
// to the planned target.
static void tickReels() {
    for (int i = 0; i < REEL_COUNT; ++i) {
        if (reels[i].spinFrames > 0) {
            reels[i].spinFrames--;
            // randomize displayed symbol while spinning
            reels[i].shown = (int)(esp_random() % 7);
            if (reels[i].spinFrames == 0) {
                reels[i].shown = reels[i].target;
            }
        }
    }
}

// ----------------------------------------------------------------------------
//  Drawing helpers
// ----------------------------------------------------------------------------
static void drawReel(int x, int y, int idx, uint16_t bodyCol, uint16_t inkCol, bool flash) {
    int w = 20, h = 26;
    sFb->fillRoundRect(x, y, w, h, 3, flash ? LIME_PALE : bodyCol);
    sFb->drawRoundRect(x, y, w, h, 3, flash ? LIME : C_WALL);
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(inkCol);
    const char* s = (idx >= 0 && idx < SYMBOL_COUNT) ? SYMBOLS[idx] : "?";
    sFb->drawString(s, x + w / 2, y + h / 2 - 1, 4);
}

static void drawZoneDividers() {
    sFb->drawFastVLine(120, 4, LH - 8, TEAL_DARK);
    sFb->drawFastVLine(194, 4, LH - 8, TEAL_DARK);
}

// Format milliseconds as HH:MM:SS uptime
static void fmtUptime(uint32_t ms, char *out, size_t n) {
    uint32_t total = ms / 1000;
    uint32_t h = total / 3600;
    uint32_t m = (total / 60) % 60;
    uint32_t s = total % 60;
    snprintf(out, n, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// Format hashes count compactly: 1234567 -> "1.23 MH"
static void fmtHashesCompact(uint64_t h, char *out, size_t n) {
    if (h < 1000)           { snprintf(out, n, "%llu H",   (unsigned long long)h); return; }
    if (h < 1000000)        { snprintf(out, n, "%.1f KH",  h / 1000.0); return; }
    if (h < 1000000000ULL)  { snprintf(out, n, "%.2f MH",  h / 1000000.0); return; }
    snprintf(out, n, "%.2f GH", h / 1000000000.0);
}

// Format share difficulty - keep it short
static void fmtDiff(double d, char *out, size_t n) {
    if (d <= 0)         snprintf(out, n, "---");
    else if (d < 1)     snprintf(out, n, "%.4f", d);
    else if (d < 1000)  snprintf(out, n, "%.2f", d);
    else                snprintf(out, n, "%.0f", d);
}

// ----------------------------------------------------------------------------
//  Zone composers
// ----------------------------------------------------------------------------
static void composeZoneClock() {
    // Background tint
    sFb->fillRect(0, 0, 120, LH, C_BG);

    // Title strip
    sFb->fillRect(0, 0, 120, 14, TEAL_DARK);
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(LIME);
    sFb->drawString("DESK CLOCK", 60, 7, 1);

    // Try to render real wall-clock time from the SNTP-synced system
    // clock in Mountain Time (MST UTC-7 / MDT UTC-6 with US DST rules).
    // If NTP hasn't returned yet (time before year 2000), fall back
    // to session uptime so the field never goes blank.
    char buf[24];
    char dateBuf[24] = "";
    char tzBuf[8]    = "";
    time_t nowSec = time(nullptr);
    bool haveRealTime = (nowSec > 1700000000);   // sometime in 2023+
    if (haveRealTime) {
        // Configurable timezone (set by the installer in build_config.h).
        // Base offset in minutes, plus optional US daylight-saving rules.
        int  tzOffMin = DEFAULT_TZ_OFFSET;
        bool useDst   = (DEFAULT_TZ_DST != 0);
        bool isDST    = false;

        if (useDst) {
            // US DST: 2nd Sunday of March 02:00 local -> 1st Sunday Nov 02:00.
            // Computed in UTC using the base offset.
            struct tm utc; gmtime_r(&nowSec, &utc);
            // Spring forward at 02:00 local == (02:00 - offset) UTC
            int springUtcHour = 2 - (tzOffMin / 60);
            int fallUtcHour    = 2 - ((tzOffMin + 60) / 60);  // during DST offset+60
            struct tm dstStart = {};
            dstStart.tm_year = utc.tm_year; dstStart.tm_mon = 2; dstStart.tm_mday = 1;
            dstStart.tm_hour = springUtcHour; mktime(&dstStart);
            dstStart.tm_mday = 1 + ((7 - dstStart.tm_wday) % 7) + 7;
            time_t dstStartT = mktime(&dstStart);
            struct tm dstEnd = {};
            dstEnd.tm_year = utc.tm_year; dstEnd.tm_mon = 10; dstEnd.tm_mday = 1;
            dstEnd.tm_hour = fallUtcHour; mktime(&dstEnd);
            dstEnd.tm_mday = 1 + ((7 - dstEnd.tm_wday) % 7);
            time_t dstEndT = mktime(&dstEnd);
            isDST = (nowSec >= dstStartT && nowSec < dstEndT);
        }

        int effOff = tzOffMin + (isDST ? 60 : 0);
        time_t localSec = nowSec + (time_t)effOff * 60;
        struct tm tmv; gmtime_r(&localSec, &tmv);

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        snprintf(tzBuf, sizeof(tzBuf), "%s",
                 (useDst && isDST) ? DEFAULT_TZ_LABEL_DST : DEFAULT_TZ_LABEL);
        static const char* DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
        static const char* MON[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};
        snprintf(dateBuf, sizeof(dateBuf), "%s %s %02d",
                 DOW[tmv.tm_wday], MON[tmv.tm_mon], tmv.tm_mday);
    } else {
        fmtUptime(millis() - g_miner.stats.session_started_at, buf, sizeof(buf));
        snprintf(dateBuf, sizeof(dateBuf), "syncing...");
        snprintf(tzBuf,   sizeof(tzBuf),   "%s", DEFAULT_TZ_LABEL);
    }
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(LIME_PALE);
    // Font 4 (~26px) fits the 120px zone; font 6 was ~48px and clipped.
    sFb->drawString(buf, 60, 36, 4);

    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString(tzBuf, 60, 52, 1);

    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString(dateBuf, 60, 62, 2);

    // Connection state line
    const char *phaseStr = g_miner.status_line;
    uint16_t phaseCol = (g_miner.phase == MINER_MINING)   ? LIME
                      : (g_miner.phase == MINER_ERROR)    ? PURPLE_LIGHT
                      : (g_miner.phase == MINER_SETUP)    ? PURPLE_BRIGHT
                                                          : TEAL_LIGHT;
    sFb->setTextColor(phaseCol);
    sFb->drawString(phaseStr, 60, 78, 2);

    // Active coin/mode badge
    {
        // Highlight the mode in a colour that signals what kind it is:
        //   LIME    = lottery solo  (BTC, BCH, DGB solo)
        //   PURPLE_BRIGHT = pool mode (DGB proportional)
        uint16_t modeCol = (g_miner.activePool == POOL_BTC_POOL)
                         ? PURPLE_BRIGHT : LIME;
        sFb->setTextColor(modeCol);
        const char *modeLabel = (g_miner.activePool < POOL_COUNT)
                              ? POOL_LABELS[g_miner.activePool] : "BTC SOLO";
        sFb->drawString(modeLabel, 60, 96, 1);
    }

    // Truncated wallet address for the active coin
    {
        const std::string *addr = &g_miner.cfg.btc_address;
        switch (g_miner.activePool) {
            case POOL_BCH:      addr = &g_miner.cfg.bch_address; break;
            case POOL_DGB_SOLO: addr = &g_miner.cfg.dgb_address; break;
            default:            addr = &g_miner.cfg.btc_address; break;
        }
        if (!addr->empty()) {
            char short_addr[14];
            size_t len = addr->size();
            size_t start = len > 10 ? len - 10 : 0;
            snprintf(short_addr, sizeof(short_addr), "..%s", addr->c_str() + start);
            sFb->setTextColor(C_HUD_DIM);
            sFb->drawString(short_addr, 60, 110, 1);
        }
    }

    // Jackpot value + odds (bottom of zone 1)
    if (g_miner.phase == MINER_SETUP && g_miner.portalActive) {
        sFb->fillRect(2, 122, 116, 44, PURPLE_DARK);
        sFb->drawRect(2, 122, 116, 44, LIME);
        sFb->setTextColor(LIME_PALE);
        sFb->drawString("SETUP NEEDED", 60, 132, 1);
        sFb->setTextColor(LIME);
        sFb->drawString(g_miner.ap_ssid.c_str(), 60, 146, 1);
        sFb->setTextColor(TEAL_LIGHT);
        sFb->drawString(g_miner.ap_ip.c_str(),   60, 158, 1);
    } else {
        char jacBuf[20];
        if (g_miner.jackpot_usd >= 1000000.0f) {
            snprintf(jacBuf, sizeof(jacBuf), "$%.2fM", g_miner.jackpot_usd / 1000000.0f);
        } else if (g_miner.jackpot_usd >= 1000.0f) {
            snprintf(jacBuf, sizeof(jacBuf), "$%.0fK", g_miner.jackpot_usd / 1000.0f);
        } else if (g_miner.jackpot_usd > 0.0f) {
            snprintf(jacBuf, sizeof(jacBuf), "$%.2f", g_miner.jackpot_usd);
        } else {
            snprintf(jacBuf, sizeof(jacBuf), "syncing...");
        }
        sFb->setTextColor(TEAL_LIGHT);
        sFb->drawString("JACKPOT", 60, 124, 1);
        sFb->setTextColor(LIME);
        sFb->drawString(jacBuf, 60, 134, 2);

        // Odds: "1 in N blocks" for the current coin, plus how many
        // times easier this coin is vs BTC at the same hashrate.
        // Both use the correct formula: expected = (netDiff * 2^32) / hashrate
        char oddsBuf[22];
        char relBuf[22];
        if (g_miner.odds_days <= 0.0f) {
            snprintf(oddsBuf, sizeof(oddsBuf), "---");
            snprintf(relBuf,  sizeof(relBuf),  "---");
        } else {
            // ~144 blocks/day for Bitcoin-family SHA256 chains
            double blocks = (double)g_miner.odds_days * 144.0;
            if (blocks < 1.0e3)
                snprintf(oddsBuf, sizeof(oddsBuf), "1 in %.0f blk", blocks);
            else if (blocks < 1.0e6)
                snprintf(oddsBuf, sizeof(oddsBuf), "1in%.0fK blk", blocks / 1.0e3);
            else if (blocks < 1.0e9)
                snprintf(oddsBuf, sizeof(oddsBuf), "1in%.0fM blk", blocks / 1.0e6);
            else if (blocks < 1.0e12)
                snprintf(oddsBuf, sizeof(oddsBuf), "1in%.0fB blk", blocks / 1.0e9);
            else
                snprintf(oddsBuf, sizeof(oddsBuf), ">1T blk");

            // Relative to BTC at current hashrate
            double hr = (double)g_miner.stats.hashrate_kHs * 1000.0;
            if (hr > 0.0) {
                double btcBlocks = (90.0e12 * 4294967296.0) / hr / 86400.0 * 144.0;
                double ratio = btcBlocks / blocks;
                if (ratio < 1.5)
                    snprintf(relBuf, sizeof(relBuf), "= BTC");
                else if (ratio < 1.0e3)
                    snprintf(relBuf, sizeof(relBuf), "%.0fx vs BTC", ratio);
                else if (ratio < 1.0e6)
                    snprintf(relBuf, sizeof(relBuf), "%.0fKx vs BTC", ratio / 1.0e3);
                else
                    snprintf(relBuf, sizeof(relBuf), "%.0fMx vs BTC", ratio / 1.0e6);
            } else {
                snprintf(relBuf, sizeof(relBuf), "---");
            }
        }
        sFb->setTextColor(TEAL_LIGHT);
        sFb->drawString("EXP WIN", 60, 148, 1);
        sFb->setTextColor(C_HUD_DIM);
        sFb->drawString(oddsBuf, 60, 158, 1);
        sFb->setTextColor(TEAL_DARK);
        sFb->drawString(relBuf, 60, 168, 1);
    }
}


// ============================================================================
//  Miner Girl — mascot sprite engine
//
//  Draws a 16×22 pixel chibi character in the Zone 2 gap (y=16..57)
//  above the slot reels. Shares the theme palette: LIME hair, PURPLE shirt,
//  TEAL helmet/pants. Animates at 6fps via millis() gating — no extra task.
//
//  State → animation:
//    MINER_OFF / MINER_SETUP               → IDLE  (bouncing, happy)
//    MINER_WIFI_CONN/POOL_CONN/
//      MINER_SUBSCRIBE / MINER_AUTHORIZE   → PREP  (hoisting pickaxe, 4fps)
//    MINER_MINING                          → MINE  (swinging at wall, 6fps)
//    MINER_ERROR                           → SAD   (slumped, static)
//    LAND_BEST / LAND_BLOCK_WIN            → YAY   (jump, 2s, then→MINE)
// ============================================================================

// ── Sprite pixel data (PROGMEM) ──────────────────────────────────────────────
// 0x0000 = transparent (draw C_BG instead). All colors from theme palette.
// 16 cols × 22 rows per frame.

static const uint16_t GIRL_MINE_0[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x7A85U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x7A85U, 0x0596U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0000U, 0x7A85U, 0x0596U, 0x0596U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0xD4ADU, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x0000U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
static const uint16_t GIRL_MINE_1[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xD4ADU, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0xD4ADU, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0xF5D1U, 0xF5D1U, 0x7A85U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x7A85U, 0x0596U, 0x0596U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
static const uint16_t GIRL_IDLE_0[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
// IDLE_1 = IDLE_0 shifted down 1px (bounce effect)
static const uint16_t GIRL_IDLE_1[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
static const uint16_t GIRL_PREP_0[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x7A85U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x7A85U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0xF5D1U, 0x0596U, 0x0596U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
// PREP_1: pickaxe fully raised (near MINE_0 pose)
static const uint16_t GIRL_PREP_1[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x7A85U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x7A85U, 0x0596U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x7A85U, 0x0596U, 0x0596U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0xD4ADU, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
static const uint16_t GIRL_SAD_0[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x7A85U, 0x7A85U, 0x7A85U, 0x7A85U, 0x7A85U, 0x7A85U, 0x7A85U, 0x7A85U, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
static const uint16_t GIRL_YAY_0[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0596U, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x038EU, 0x0596U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xFFFFU, 0xFFFFU, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xDB2DU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xDB2DU, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0xF5D1U, 0xF5D1U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};
// YAY_1: landing squat (same as IDLE_1 — she bounces back to idle stance)
static const uint16_t GIRL_YAY_1[22][16] PROGMEM = {
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0260U, 0x0260U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x07E0U, 0x0260U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0145U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xDB2DU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x9012U, 0x580BU, 0x9012U, 0x9012U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0xF5D1U, 0xF5D1U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0x9012U, 0xF5D1U, 0xF5D1U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x038EU, 0x038EU, 0x038EU, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x01E7U, 0x01E7U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
    {0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0145U, 0x0145U, 0x0145U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U},
};

// ── Miner girl layout constants ──────────────────────────────────────────────
static constexpr int      GIRL_W        = 16;    // sprite width in pixels
static constexpr int      GIRL_H        = 22;    // sprite height in pixels
static constexpr int      GIRL_X        = 124;   // x position in landscape fb
static constexpr int      GIRL_Y        = 16;    // y position in landscape fb
static constexpr int      GIRL_FPS      = 6;
static constexpr uint32_t GIRL_FRAME_MS = 1000 / GIRL_FPS;  // 167ms

// ── Animation state ───────────────────────────────────────────────────────────
enum GirlAnim {
    GIRL_ANIM_IDLE, GIRL_ANIM_PREP, GIRL_ANIM_MINE,
    GIRL_ANIM_SAD,  GIRL_ANIM_YAY,
};
static GirlAnim   sGirlAnim       = GIRL_ANIM_IDLE;
static uint8_t    sGirlFrame      = 0;
static uint32_t   sGirlLastTick   = 0;
static uint32_t   sGirlYayUntil  = 0;   // millis() when YAY ends

// Pointer tables — indexed by frame within each animation
static const uint16_t (* const GIRL_FRAMES_MINE[2])[16] = { GIRL_MINE_0, GIRL_MINE_1 };
static const uint16_t (* const GIRL_FRAMES_IDLE[2])[16] = { GIRL_IDLE_0, GIRL_IDLE_1 };
static const uint16_t (* const GIRL_FRAMES_PREP[2])[16] = { GIRL_PREP_0, GIRL_PREP_1 };
static const uint16_t (* const GIRL_FRAMES_YAY[2])[16]  = { GIRL_YAY_0,  GIRL_YAY_1  };

// ── Sprite blit ───────────────────────────────────────────────────────────────
// Draws a 16×22 sprite at (dx, dy) in the framebuffer.
// Pixels with value 0x0000 are skipped (transparent).
static void blitGirl(int dx, int dy, const uint16_t (*frame)[16]) {
    for (int row = 0; row < GIRL_H; ++row) {
        for (int col = 0; col < GIRL_W; ++col) {
            uint16_t px = pgm_read_word(&frame[row][col]);
            if (px) sFb->drawPixel(dx + col, dy + row, px);
            else    sFb->drawPixel(dx + col, dy + row, C_BG);
        }
    }
}

// ── Diamond peek ─────────────────────────────────────────────────────────────
// Draws a small diamond shape at x=192..199, vertically centred in the gap.
// Two diamonds stacked so she always has something to mine toward.
static void drawDiamonds() {
    // Diamond colors: TEAL_LIGHT center, white highlight, TEAL shadow
    auto dp = [&](int x, int y, uint16_t c){ sFb->drawPixel(x, y, c); };
    auto dd = [&](int cx, int cy) {
        // 5×4 diamond shape
        dp(cx+2, cy+0, TEAL_LIGHT);
        dp(cx+1, cy+1, 0xFFFFU); dp(cx+2, cy+1, TEAL_LIGHT); dp(cx+3, cy+1, TEAL);
        dp(cx+0, cy+2, TEAL_LIGHT); dp(cx+1, cy+2, TEAL_LIGHT); dp(cx+2, cy+2, 0xFFFFU); dp(cx+3, cy+2, TEAL); dp(cx+4, cy+2, TEAL);
        dp(cx+1, cy+3, TEAL); dp(cx+2, cy+3, TEAL_LIGHT); dp(cx+3, cy+3, TEAL);
        dp(cx+2, cy+4, TEAL);
    };
    dd(194, 20);
    dd(196, 30);
    dd(194, 40);
}

// ── Wall strip ────────────────────────────────────────────────────────────────
// Draws a narrow brick wall at x=140..150 (just to the right of the girl)
// so she appears to be chipping into it. Zone 2 divider line is at x=194.
static void drawMineWall() {
    for (int y = 16; y < 58; y++) {
        for (int x = 140; x < 150; x++) {
            bool isMortar = (x == 140) || (x == 149);
            bool isShift  = ((y / 6) & 1);
            int  brickX   = isShift ? ((x - 143) % 6) : ((x - 140) % 6);
            bool isJoint  = (brickX == 0) || ((y % 6) == 0);
            uint16_t c = isMortar ? TEAL_DARK :
                         isJoint  ? TEAL_DARK :
                                    rgb565(0, 55, 80);
            sFb->drawPixel(x, y, c);
        }
    }
}

// ── Chip particles ────────────────────────────────────────────────────────────
// Show 2–3 flying chips when in MINE frame 1 (pickaxe hits wall)
static void drawChips(uint32_t now) {
    // Cycle through 3 chip positions based on millis
    uint8_t phase = (now / 167) & 3;
    struct { int x, y; } chips[3] = {
        {138 - (int)(phase),   22 + (int)(phase & 1)},
        {136 - (int)(phase)*2, 28},
        {137 - (int)(phase),   34 + (int)(phase & 1)},
    };
    for (auto &c : chips)
        sFb->drawPixel(c.x, c.y, TEAL_LIGHT);
}

// ── Main girl tick + draw ─────────────────────────────────────────────────────
// Called from composeZoneReels(). Advances animation state and blits.
static void drawMinerGirl() {
    uint32_t now = millis();

    // ── State transitions ────────────────────────────────────────────────────
    GirlAnim target;
    if (sGirlAnim == GIRL_ANIM_YAY && now < sGirlYayUntil) {
        target = GIRL_ANIM_YAY;   // stay in YAY until timer expires
    } else {
        switch (g_miner.phase) {
            case MINER_OFF:
            case MINER_SETUP:
                target = GIRL_ANIM_IDLE;  break;
            case MINER_WIFI_CONN:
            case MINER_POOL_CONN:
            case MINER_SUBSCRIBE:
            case MINER_AUTHORIZE:
                target = GIRL_ANIM_PREP;  break;
            case MINER_MINING:
                target = GIRL_ANIM_MINE;  break;
            case MINER_ERROR:
            default:
                target = GIRL_ANIM_SAD;   break;
        }
        // Trigger YAY on landing events
        if (g_miner.phase == MINER_MINING &&
            (lastLanding == LAND_BEST || lastLanding == LAND_BLOCK_WIN) &&
            (now - landingShownAt) < 200) {   // fresh event (within first 200ms)
            target = GIRL_ANIM_YAY;
            sGirlYayUntil = now + 2000;
        }
    }

    if (target != sGirlAnim) {
        sGirlAnim  = target;
        sGirlFrame = 0;
        sGirlLastTick = now;
    }

    // ── Frame advance at 6fps ────────────────────────────────────────────────
    uint32_t frameMs = GIRL_FRAME_MS;
    if (sGirlAnim == GIRL_ANIM_PREP) frameMs = 333;   // 3fps for slow hoist
    if (sGirlAnim == GIRL_ANIM_SAD)  frameMs = 0;     // static

    if (frameMs && (now - sGirlLastTick) >= frameMs) {
        sGirlLastTick = now;
        switch (sGirlAnim) {
            case GIRL_ANIM_MINE:
            case GIRL_ANIM_IDLE:
            case GIRL_ANIM_PREP:
            case GIRL_ANIM_YAY:
                sGirlFrame = (sGirlFrame + 1) & 1;   // toggle 0/1
                break;
            default: break;
        }
    }

    // ── Select frame pointer ─────────────────────────────────────────────────
    const uint16_t (*frame)[16];
    switch (sGirlAnim) {
        case GIRL_ANIM_MINE: frame = GIRL_FRAMES_MINE[sGirlFrame]; break;
        case GIRL_ANIM_IDLE: frame = GIRL_FRAMES_IDLE[sGirlFrame]; break;
        case GIRL_ANIM_PREP: frame = GIRL_FRAMES_PREP[sGirlFrame]; break;
        case GIRL_ANIM_YAY:  frame = GIRL_FRAMES_YAY [sGirlFrame]; break;
        default:             frame = GIRL_SAD_0;                    break;
    }

    // ── Blit ─────────────────────────────────────────────────────────────────
    drawMineWall();
    blitGirl(GIRL_X, GIRL_Y, frame);
    drawDiamonds();
    if (sGirlAnim == GIRL_ANIM_MINE && sGirlFrame == 1)
        drawChips(now);
}

static void composeZoneReels() {
    sFb->fillRect(122, 0, 71, LH, C_BG);

    // Miner girl mascot — above the reels
    drawMinerGirl();

    // Title
    sFb->fillRect(122, 0, 71, 14, TEAL_DARK);
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(LIME);
    sFb->drawString("SLOTS", 157, 7, 1);

    // Frame around the reels
    int frameX = 124, frameY = 60, frameW = 67, frameH = 32;
    sFb->drawRoundRect(frameX, frameY, frameW, frameH, 4, PURPLE);
    sFb->drawRoundRect(frameX - 1, frameY - 1, frameW + 2, frameH + 2, 5, TEAL_DARK);

    bool blockFlash = (millis() - blockWinShownAt) < 4000 && blockWinShownAt > 0
                      && lastLanding == LAND_BLOCK_WIN;
    bool bestFlash  = (millis() - landingShownAt) < 1200 && lastLanding == LAND_BEST;

    for (int i = 0; i < REEL_COUNT; ++i) {
        int rx = frameX + 2 + i * 22;
        int ry = frameY + 3;
        bool flash = (blockFlash) || (bestFlash && ((millis() / 120) & 1));
        uint16_t body = blockFlash ? LIME : LIME_PALE;
        uint16_t ink  = (lastLanding == LAND_BLOCK_WIN) ? PURPLE_BRIGHT
                     : (lastLanding == LAND_BEST)       ? PURPLE
                                                        : TEAL_DEEP;
        drawReel(rx, ry, reels[i].shown, body, ink, flash);
    }

    // Landing label
    const char *label = "";
    uint16_t labCol = C_HUD_DIM;
    switch (lastLanding) {
        case LAND_IDLE:      label = "WAITING";    labCol = C_HUD_DIM;      break;
        case LAND_MISS:      label = "MISS";        labCol = PURPLE_LIGHT;   break;
        case LAND_CLOSE:     label = "CLOSE";       labCol = TEAL_LIGHT;     break;
        case LAND_NEAR:      label = "NEAR MISS";   labCol = PURPLE_BRIGHT;  break;
        case LAND_BEST:      label = "NEW BEST";    labCol = LIME;           break;
        case LAND_BLOCK_WIN: label = "BLOCK WIN!";  labCol = LIME;           break;
    }
    sFb->setTextColor(labCol);
    sFb->drawString(label, 157, 102, 1);

    // Best-ever badge
    char buf[32];
    snprintf(buf, sizeof(buf), "BEST %.2f", g_miner.stats.all_time_best_diff);
    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString(buf, 157, 116, 1);

    snprintf(buf, sizeof(buf), "BLK %lu", (unsigned long)g_miner.stats.blocks_found);
    sFb->setTextColor(LIME_PALE);
    sFb->drawString(buf, 157, 130, 1);

    // Spin counter
    snprintf(buf, sizeof(buf), "SPN %lu", (unsigned long)g_miner.stats.spins_total);
    sFb->setTextColor(C_HUD_DIM);
    sFb->drawString(buf, 157, 144, 1);
}

static void composeZoneTelemetry() {
    sFb->fillRect(196, 0, 124, LH, C_BG);
    sFb->fillRect(196, 0, 124, 14, TEAL_DARK);
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(LIME);
    sFb->drawString((g_miner.activePool == POOL_BTC_POOL) ? "POOL" : "MINER", 258, 7, 1);

    char buf[40];

    // Hashrate - the most important number
    sFb->setTextDatum(TL_DATUM);
    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString("HASHRATE", 200, 22, 1);
    sFb->setTextColor(LIME_PALE);
    snprintf(buf, sizeof(buf), "%.1f KH/s", g_miner.stats.hashrate_kHs);
    sFb->drawString(buf, 200, 32, 2);

    // Total hashes this session
    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString("TOTAL", 200, 56, 1);
    fmtHashesCompact(g_miner.stats.hashes_total, buf, sizeof(buf));
    sFb->setTextColor(LIME);
    sFb->drawString(buf, 200, 66, 2);

    // Last share / best share
    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString("LAST DIFF", 200, 90, 1);
    fmtDiff(g_miner.stats.last_share_diff, buf, sizeof(buf));
    sFb->setTextColor(LIME_PALE);
    sFb->drawString(buf, 200, 100, 2);

    sFb->setTextColor(TEAL_LIGHT);
    sFb->drawString("SESSION BEST", 200, 122, 1);
    fmtDiff(g_miner.stats.session_best_diff, buf, sizeof(buf));
    sFb->setTextColor(PURPLE_LIGHT);
    sFb->drawString(buf, 200, 132, 2);

    // Accept rate: show as % with colour coding (green/yellow/red)
    // Also show raw A/S counts in small text below
    if (g_miner.stats.shares_submitted > 0) {
        float rate = (float)g_miner.stats.shares_accepted
                   / (float)g_miner.stats.shares_submitted * 100.0f;
        uint16_t rateCol = (rate >= 95.0f) ? LIME
                         : (rate >= 75.0f) ? TEAL_LIGHT
                                           : PURPLE_BRIGHT;
        sFb->setTextColor(TEAL_LIGHT);
        sFb->drawString("ACCEPT", 200, 148, 1);
        snprintf(buf, sizeof(buf), "%.0f%%", rate);
        sFb->setTextColor(rateCol);
        sFb->drawString(buf, 200, 158, 2);
    } else {
        sFb->setTextColor(C_HUD_DIM);
        sFb->drawString("ACCEPT", 200, 148, 1);
        sFb->drawString("---", 200, 158, 2);
    }
    // Raw A/S counts in tiny text at very bottom
    snprintf(buf, sizeof(buf), "%lu/%lu shr",
        (unsigned long)g_miner.stats.shares_accepted,
        (unsigned long)g_miner.stats.shares_submitted);
    sFb->setTextColor(C_HUD_DIM);
    sFb->setTextDatum(TL_DATUM);
    sFb->drawString(buf, 200, 158, 1);
}

// Persistent block-win banner overlay (for ~5s after a block solve)
static void overlayBlockWin() {
    if (lastLanding != LAND_BLOCK_WIN) return;
    if (millis() - blockWinShownAt > 5000) return;
    sFb->fillRect(40, 60, LW - 80, 50, LIME);
    sFb->drawRect(40, 60, LW - 80, 50, LIME_PALE);
    sFb->setTextDatum(MC_DATUM);
    sFb->setTextColor(PURPLE_BRIGHT);
    sFb->drawString("BLOCK SOLVED!", LW / 2, 78, 4);
    sFb->setTextColor(PURPLE);
    sFb->drawString("payout incoming", LW / 2, 98, 2);
}

// ----------------------------------------------------------------------------
//  Public draw entry called from main.cpp's loop()
// ----------------------------------------------------------------------------
extern TFT_eSPI tft;        // from main.cpp
extern bool fbReady;        // from main.cpp

extern TFT_eSprite fb;   // main.cpp portrait sprite - we reuse its buffer

void minerDraw() {
    if (!g_miner.running) return;

    // First frame after entering: reuse the main portrait framebuffer rather
    // than allocating a second one. Without PSRAM, two 320x170 sprites at
    // 16-bit depth would exhaust internal SRAM (~217 KB each, ~300 KB free).
    // We delete the portrait sprite's internal buffer, resize it to 320x170
    // landscape, and take ownership; on exit we recreate it portrait.
    if (sFb == nullptr) {
        sTft = &tft;
        sSavedRotation = sTft->getRotation();
        sTft->setRotation(3);
        sTft->fillScreen(C_BG);

        // Reuse main fb object - free its portrait buffer first
        fb.deleteSprite();

        sFb = &fb;
        sFb->setColorDepth(16);
        if (!sFb->createSprite(LW, LH)) {
            sFb->setColorDepth(8);
            if (!sFb->createSprite(LW, LH)) {
                // Still failed - abort draw to avoid null-ptr crash
                sFb = nullptr;
                return;
            }
        }
        sFbOwned = false;   // we don't delete sFb, main.cpp owns the object
        // Reset reel state on entry
        for (int i = 0; i < REEL_COUNT; ++i) {
            reels[i].shown = 0;
            reels[i].target = 0;
            reels[i].spinFrames = 0;
        }
        lastLanding = LAND_IDLE;
    }

    // Check for new spin trigger from miner core
    if (g_miner.uiSpinTrigger) {
        g_miner.uiSpinTrigger = false;
        triggerSpin(g_miner.stats.last_share_diff,
                    g_miner.stats.session_best_diff,
                    (millis() - g_miner.blockWinFlash) < 200);
    }

    tickReels();

    sFb->fillSprite(C_BG);

    composeZoneClock();
    composeZoneReels();
    composeZoneTelemetry();
    drawZoneDividers();
    overlayBlockWin();

    // Corner ticks (consistent with the rest of the firmware)
    sFb->drawFastHLine(0, 0,    14, LIME);
    sFb->drawFastVLine(0, 0,    14, LIME);
    sFb->drawFastHLine(LW - 14, 0,    14, LIME);
    sFb->drawFastVLine(LW - 1,  0,    14, LIME);
    sFb->drawFastHLine(0, LH - 1, 14, LIME);
    sFb->drawFastVLine(0, LH - 14, 14, LIME);
    sFb->drawFastHLine(LW - 14, LH - 1, 14, LIME);
    sFb->drawFastVLine(LW - 1,  LH - 14, 14, LIME);

    // ===== RIGHT-HOLD progress bar: coin cycle =====
    // Drawn at the bottom of the screen as a green growing bar,
    // same pattern as the sleep bar in the rest of the firmware.
    if (g_miner.rightHoldMs > 100) {
        static constexpr uint32_t HOLD_MS = 1500;
        static constexpr int BAR_Y   = LH - 5;
        static constexpr int BAR_X   = 10;
        static constexpr int BAR_W   = LW - 20;
        static constexpr int BAR_H   = 3;
        int filled = (int)(((float)g_miner.rightHoldMs / HOLD_MS) * BAR_W);
        if (filled > BAR_W) filled = BAR_W;
        sFb->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, TEAL_LIGHT);
        sFb->fillRect(BAR_X, BAR_Y, filled, BAR_H, LIME);
        // Label showing what we're switching to
        if (g_miner.nextPool != g_miner.activePool || g_miner.coinSwitchPending) {
            const char *nextLabel = POOL_LABELS[g_miner.nextPool];
            char switchBuf[24];
            snprintf(switchBuf, sizeof(switchBuf), "> %s", nextLabel);
            sFb->setTextDatum(MC_DATUM);
            sFb->setTextColor(LIME);
            sFb->drawString(switchBuf, LW / 2, LH - 12, 1);
        } else {
            // Show what holding will switch to (preview)
            int preview = ((int)g_miner.activePool + 1) % POOL_COUNT;
            char switchBuf[24];
            snprintf(switchBuf, sizeof(switchBuf), "> %s", POOL_LABELS[preview]);
            sFb->setTextDatum(MC_DATUM);
            sFb->setTextColor(TEAL_LIGHT);
            sFb->drawString(switchBuf, LW / 2, LH - 12, 1);
        }
    }

    // ===== LEFT-HOLD progress bar: reset confirm =====
    if (g_miner.leftHoldMs > 100) {
        static constexpr uint32_t HOLD_MS = 1500;
        static constexpr int BAR_Y   = LH - 5;
        static constexpr int BAR_X   = 10;
        static constexpr int BAR_W   = LW - 20;
        static constexpr int BAR_H   = 3;
        int filled = (int)(((float)g_miner.leftHoldMs / HOLD_MS) * BAR_W);
        if (filled > BAR_W) filled = BAR_W;
        sFb->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, TEAL_LIGHT);
        sFb->fillRect(BAR_X, BAR_Y, filled, BAR_H, PURPLE_BRIGHT);
    }

    // ===== RESET CONFIRM OVERLAY =====
    if (g_miner.resetConfirmPending) {
        // Semi-opaque banner across the middle of the screen
        int bx = 20, by = 58, bw = LW - 40, bh = 54;
        sFb->fillRect(bx, by, bw, bh, PURPLE_DARK);
        sFb->drawRect(bx, by, bw, bh, PURPLE_BRIGHT);
        sFb->drawRect(bx + 1, by + 1, bw - 2, bh - 2, PURPLE_LIGHT);
        sFb->setTextDatum(MC_DATUM);
        sFb->setTextColor(LIME);
        sFb->drawString("RESET SETUP MODE?", LW / 2, by + 14, 2);
        sFb->setTextColor(LIME_PALE);
        sFb->drawString("Hold LEFT again to confirm", LW / 2, by + 30, 1);
        sFb->setTextColor(C_HUD_DIM);
        sFb->drawString("Clears WiFi + wallet addresses", LW / 2, by + 44, 1);
    }

    sFb->pushSprite(0, 0);
}

// Called from minerShutdown - tears down our sprite and restores portrait
// rotation so the games look right when we return.
void minerUiShutdown() {
    if (sFb) {
        // Free the landscape buffer we allocated inside the shared fb object
        sFb->deleteSprite();
        if (sFbOwned) {
            // We allocated the TFT_eSprite object itself - delete it
            delete sFb;
        } else {
            // We borrowed main.cpp's fb object - recreate it in portrait mode
            // so the rest of the firmware can draw normally again.
            extern TFT_eSprite fb;
            fb.setColorDepth(16);
            if (!fb.createSprite(170, 320)) {
                fb.setColorDepth(8);
                fb.createSprite(170, 320);
            }
        }
        sFb = nullptr;
        sFbOwned = false;
    }
    if (sTft) {
        sTft->setRotation(sSavedRotation);
        sTft->fillScreen(C_BG);
        sTft = nullptr;
    }
}
