// ============================================================================
//  Rabbit Hole Mode — rabbit_mode.cpp
//
//  Portrait 170×320, reuses main.cpp fb sprite (no rotation swap needed).
//
//  Topics and their fetch strategies:
//
//  WIKIPEDIA topics (HTTP, en.wikipedia.org):
//    Fetch random article from a category via /w/api.php categorymembers,
//    then fetch the summary via /api/rest_v1/page/summary/<title>.
//    Go deeper: pick a linked title from the summary's content_urls siblings
//    stored in our deep-link list (extracted from the article's intro links).
//
//  HACKER NEWS (HTTP, hacker-news.firebaseio.com):
//    Fetch /v0/topstories.json → random story ID.
//    Fetch /v0/item/<id>.json → title, score, by, url.
//    Go deeper: fetch top comment (kids[0]) → /v0/item/<kid>.json → text.
//
//  GITHUB TRENDING (HTTPS, api.github.com):
//    Search repos by topic keyword, sort=stars, per_page=10, pick random.
//    Go deeper: show full description + language + star count.
//    (WiFiClientSecure, setInsecure())
//
//  HACKADAY RSS (HTTP, hackaday.com):
//    Fetch /blog/feed/ RSS, parse <title> + <description> items.
//    Go deeper: Wikipedia lookup of the first noun in the title.
//
//  ARXIV (HTTPS, export.arxiv.org):
//    Fetch Atom feed for category, parse <title> + first sentence of <summary>.
//    Go deeper: Wikipedia lookup of first keyword.
//
//  Display layout (170×320 portrait, theme colors):
//    y=0..16    topic badge bar (TEAL_DARK bg, topic name in LIME)
//    y=17..47   article title (LIME_PALE, wrapped, up to 3 lines)
//    y=48..48   divider line (TEAL_DARK)
//    y=49..270  body text (TEAL_BRIGHT, font 1, ~28 chars × ~14 lines)
//    y=271..271 divider line (TEAL_DARK)
//    y=272..284 depth breadcrumb  "DEPTH 2 · HACKER NEWS"
//    y=285..298 hint bar  "← NEW   DEEPER →   hold: TOPIC"
//    y=299..320 status / loading indicator
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <math.h>
#include <TJpg_Decoder.h>
#include "rabbit_mode.h"
#include "../theme_colors.h"
#include "../net_yield.h"
#include "../build_config.h"
#include "../shared_buffers.h"
#include "../test_jpeg.h"

// ── Shared fb (main.cpp) ────────────────────────────────────────────────────
extern TFT_eSprite fb;
extern bool        fbReady;
// Also need btnL/btnR from main.cpp
extern bool btnL, btnR, btnL_prev, btnR_prev;

// ── Display dims (portrait) ─────────────────────────────────────────────────
static constexpr int W = 170;
static constexpr int H = 320;

// ── WiFi credentials (read from btcminer NVS, read-only) ────────────────────
static char rbSsid[64] = "";
static char rbPass[64] = "";

// ── Topics ──────────────────────────────────────────────────────────────────
struct Topic {
    const char* name;       // display label
    const char* badge;      // short badge ≤12 chars
    uint8_t     fetchType;  // FETCH_WIKI / FETCH_HN / FETCH_GH / FETCH_RSS / FETCH_ARXIV
    const char* param;      // category name, search query, RSS url, arxiv cat
};

static constexpr uint8_t FETCH_WIKI  = 0;
static constexpr uint8_t FETCH_HN    = 1;
static constexpr uint8_t FETCH_GH    = 2;
static constexpr uint8_t FETCH_RSS   = 3;
static constexpr uint8_t FETCH_ARXIV = 4;
static constexpr uint8_t FETCH_SRCH  = 5;   // Wikipedia search (better than category)
static constexpr uint8_t FETCH_TESTIMG = 8; // known-good JPEG (decode test)

static const Topic TOPICS[] = {
    { "HACKER NEWS",         "HN",       FETCH_HN,    "" },
    { "RETRO GAMING",        "RETRO",    FETCH_SRCH,  "retro gaming|classic video game|arcade game history|video game console|8-bit game|game emulator" },
    { "SPECULATIVE COSMOLOGY","COSMOS",  FETCH_WIKI,  "Physical_cosmology" },
    { "ARDUINO PROJECTS",    "ARDUINO",  FETCH_GH,    "arduino|esp32|raspberrypi|microcontroller|electronics-diy" },
    { "HANDHELD GAMING",     "HANDHELD", FETCH_WIKI,  "Handheld_game_consoles" },
    { "CYBER DEVICES",       "CYBER",    FETCH_RSS,   "hackaday.com" },
    { "CRYPTO TECH",         "CRYPTO",   FETCH_RSS,   "coindesk.com" },
    { "SCIENCE PAPERS",      "ARXIV",    FETCH_ARXIV, "astro-ph.CO|cs.AI|physics.pop-ph|cond-mat.mes-hall" },
    { "WEIRD HISTORY",       "WEIRD",    FETCH_SRCH,  "unusual historical events|strange history|bizarre history|historical mystery|odd historical event" },
    { "RETRO COMPUTING",     "RETRO PC", FETCH_SRCH,  "vintage computer|home computer history|personal computer history|legacy computer hardware|8-bit computer|microcomputer history" },
    { "IMAGE TEST",          "IMGTEST",  FETCH_TESTIMG, "" },
};
static constexpr int TOPIC_COUNT = (int)(sizeof(TOPICS) / sizeof(TOPICS[0]));

// Enabled-topic filter: the installer sets RABBIT_TOPIC_MASK to choose which
// topics are included. We build a list of enabled indices at runtime and the
// navigation only steps through those, so disabled topics never appear.
static int  rbEnabled[TOPIC_COUNT];
static int  rbEnabledCount = 0;
static int  rbEnabledPos   = 0;   // position within rbEnabled[]

static void rbBuildEnabledList() {
    rbEnabledCount = 0;
    for (int i = 0; i < TOPIC_COUNT; i++) {
        // The IMAGE TEST topic is always available (it's a diagnostic), even
        // if it falls outside the configured topic mask's bits.
        bool isTest = (TOPICS[i].fetchType == FETCH_TESTIMG);
        if (isTest || (RABBIT_TOPIC_MASK & (1u << i))) rbEnabled[rbEnabledCount++] = i;
    }
    // Safety: if the mask somehow excluded everything, enable Hacker News
    if (rbEnabledCount == 0) { rbEnabled[0] = 0; rbEnabledCount = 1; }
}

// ── State ───────────────────────────────────────────────────────────────────
enum RbPhase {
    RB_WIFI,       // connecting
    RB_LOADING,    // fetching article
    RB_DISPLAY,    // showing article
    RB_ERROR,      // fetch failed
};
static RbPhase  rbPhase      = RB_WIFI;
static uint32_t rbPhaseAt    = 0;
static int      rbTopicIdx   = 0;
static int      rbDepth      = 0;   // 0=top, 1=deeper, etc.

// ── Article data ─────────────────────────────────────────────────────────────
static char rbTitle[80]      = "";
static char rbBody[1500]     = "";   // display body (full, scrolled if needed)
static char rbStatus[48]     = "";

// ── Scroll state ─────────────────────────────────────────────────────────────
static constexpr int      RB_VISIBLE_LINES   = 21;
static constexpr uint32_t RB_SCROLL_PAUSE_MS = 2000;
static constexpr uint32_t RB_SCROLL_STEP_MS  = 300;
static int      rbScrollOffset   = 0;
static int      rbTotalLines     = 0;
static uint32_t rbScrollLastStep = 0;
static bool     rbScrollPausing  = true;

// ── Deep link stack ──────────────────────────────────────────────────────────
// Up to 4 links extracted from each article for "go deeper"
static char rbDeepLinks[4][80];
static int  rbDeepCount      = 0;
static char rbDeepType       = 'W';  // 'W'=wiki, 'H'=hn item, 'G'=github, 'X'=wiki lookup

// ── For HN: store current story ID for comment diving ────────────────────────
static long  rbHnStoryId     = 0;
static long  rbHnCommentId   = 0;   // currently displayed kid

// ── Image topics (IMAGE TEST): current image ───────────────────────────────
// We just store the URL to display — no image data kept after render
static char  rbImageUrl[128] = "";  // thumbnail URL to fetch+display
static bool  rbHasImage      = false;

// ── JPEG image buffer + decode state ──────────────────────────────────────────
// Holds the downloaded JPEG bytes for TJpgDec to decode into the fb sprite.
// JPEG bytes for TJpgDec. Uses the shared heap buffer g_jpegBuf, acquired in
// rabbitEnter and freed in rabbitShutdown (see shared_buffers.h).
#define rbJpeg g_jpegBuf
static size_t   rbJpegLen     = 0;  // bytes actually downloaded
static bool     rbJpegReady    = false;  // true when a JPEG is ready to render
static int      rbJpegW        = 0;  // decoded dimensions
static int      rbJpegH        = 0;
static int      rbJpegScale     = 1;  // 1/2/4/8 downscale factor
static int      rbJpegOriginX   = 0;  // top-left render position
static int      rbJpegOriginY   = 0;

// ── Fetch control flags ───────────────────────────────────────────────────────
// Two-tick pattern: first tick draws the loading screen, second tick fetches.
// This ensures the user sees "LOADING..." before the blocking HTTP call.
static bool  rbNeedsFetch    = false;
static bool  rbFetchDeeper   = false;

// ── Fetch buffer (static, never heap) ────────────────────────────────────────
static char rbBuf[8192];

// ── Button state for single/long-press detection ────────────────────────────
static constexpr uint32_t RB_LONG_MS  = 1500;
static uint32_t rbRHoldStart = 0;
static uint32_t rbLHoldStart = 0;
static bool     rbRFired     = false;
static bool     rbLFired     = false;
// Track edges for single-click (fire on release if not a long-press)
static bool     rbRWasDown   = false;
static bool     rbLWasDown   = false;
static bool     rbDoDeeper   = false;
static bool     rbDoNew      = false;
static bool     rbDoNextTopic= false;

// ── HTTP GET helper (plain HTTP port 80) ─────────────────────────────────────
static bool rbGet(const char* host, const char* path,
                  char* out, size_t outLen) {
    WiFiClient c;
    if (!c.connect(host, 80)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 pinball-s3\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    uint32_t dl = millis() + 10000;
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            c.stop(); return false;
        }
    }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (pos < outLen - 1) out[pos++] = (char)ch;
    }
    out[pos] = '\0'; c.stop();
    return pos > 10;
}

// ── HTTPS GET helper (port 443, setInsecure) ─────────────────────────────────
static bool rbGetS(const char* host, const char* path,
                   char* out, size_t outLen) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12);
    if (!c.connect(host, 443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\n"
             "User-Agent: Mozilla/5.0 (compatible; pinball-s3)\r\n"
             "Accept: application/json,text/xml,*/*\r\n"
             "Connection: close\r\n\r\n", path, host);
    uint32_t dl = millis() + 14000;
    // Skip HTTP headers without heap String allocation
    char hdrBuf[160]; int hdrLen = 0;
    bool statusOk = false;
    bool inHeaders = true;
    while (inHeaders && (c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            hdrBuf[hdrLen] = '\0';
            if (hdrLen == 0) { inHeaders = false; break; }
            if (strncmp(hdrBuf, "HTTP/", 5) == 0)
                statusOk = (strstr(hdrBuf, " 200") != nullptr);
            hdrLen = 0;
        } else if (hdrLen < 159) {
            hdrBuf[hdrLen++] = (char)ch;
        }
    }
    if (!statusOk) { c.stop(); return false; }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (pos < outLen - 1) out[pos++] = (char)ch;
    }
    out[pos] = '\0'; c.stop();
    return pos > 10;
}

// Variant that sends a caller-supplied Accept header and follows one redirect.
// Used to fetch GitHub READMEs as raw markdown (Accept: application/vnd.github.raw)
// instead of base64-wrapped JSON.
static bool rbGetSAccept(const char* host, const char* path,
                         const char* accept, char* out, size_t outLen) {
    // Iterative redirect handling: each hop's WiFiClientSecure is created and
    // destroyed within one loop iteration, so we never hold two TLS contexts
    // on the stack at once (that double allocation crashed the device).
    char curHost[120]; strncpy(curHost, host, sizeof(curHost) - 1); curHost[sizeof(curHost)-1]='\0';
    char curPath[200]; strncpy(curPath, path, sizeof(curPath) - 1); curPath[sizeof(curPath)-1]='\0';

    for (int hop = 0; hop < 3; hop++) {
        bool redirect = false;
        char location[200] = "";
        {
            WiFiClientSecure c;
            c.setInsecure();
            c.setTimeout(12);
            if (!c.connect(curHost, 443)) return false;
            c.printf("GET %s HTTP/1.0\r\nHost: %s\r\n"
                     "User-Agent: Mozilla/5.0 (compatible; pinball-s3)\r\n"
                     "Accept: %s\r\n"
                     "Connection: close\r\n\r\n", curPath, curHost, accept);
            uint32_t dl = millis() + 14000;
            char hdrBuf[256]; int hdrLen = 0;
            int  status = 0;
            bool inHeaders = true;
            while (inHeaders && (c.connected() || c.available()) && millis() < dl) {
                int ch = c.read();
                if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
                if (ch == '\r') continue;
                if (ch == '\n') {
                    hdrBuf[hdrLen] = '\0';
                    if (hdrLen == 0) { inHeaders = false; break; }
                    if (strncmp(hdrBuf, "HTTP/", 5) == 0) {
                        const char* sp = strchr(hdrBuf, ' ');
                        if (sp) status = atoi(sp + 1);
                    } else if (strncasecmp(hdrBuf, "Location:", 9) == 0) {
                        const char* v = hdrBuf + 9;
                        while (*v == ' ') v++;
                        strncpy(location, v, sizeof(location) - 1);
                    }
                    hdrLen = 0;
                } else if (hdrLen < 255) {
                    hdrBuf[hdrLen++] = (char)ch;
                }
            }
            if ((status == 301 || status == 302 || status == 307) && location[0]) {
                redirect = true;
            } else if (status != 200) {
                return false;
            } else {
                size_t pos = 0;
                while ((c.connected() || c.available()) && millis() < dl) {
                    int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
                    if (pos < outLen - 1) out[pos++] = (char)ch;
                }
                out[pos] = '\0';
                return pos > 10;
            }
        }   // WiFiClientSecure destroyed here before the next hop
        if (redirect) {
            const char* lh = location;
            if (strncmp(lh, "https://", 8) == 0) lh += 8;
            else if (strncmp(lh, "http://", 7) == 0) lh += 7;
            int hi = 0; char nh[120];
            while (*lh && *lh != '/' && hi < 119) nh[hi++] = *lh++;
            nh[hi] = '\0';
            strncpy(curHost, nh, sizeof(curHost) - 1); curHost[sizeof(curHost)-1]='\0';
            strncpy(curPath, (*lh) ? lh : "/", sizeof(curPath) - 1); curPath[sizeof(curPath)-1]='\0';
        }
    }
    return false;
}

// ── TJpgDec callback: render a decoded 16x16 block into the fb sprite ─────────
// TJpgDec calls this for each MCU block during decode. We push the pixels
// straight into the portrait fb sprite (already allocated by main.cpp).
static bool rbJpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    // Offset by the render origin and clip to the visible image area
    int px = rbJpegOriginX + x;
    int py = rbJpegOriginY + y;
    // Stop if running past the bottom of the usable area
    if (py >= H - 24) return false;
    // setSwapBytes MUST be set on the SPRITE that receives pushImage (not on
    // TJpgDec). Without it, 16bpp sprite pushImage byte-order is wrong and the
    // image renders as garbage/invisible (TFT_eSPI issue #1658).
    fb.setSwapBytes(true);
    fb.pushImage(px, py, w, h, bitmap);
    return true;  // keep decoding
}

static bool jStr(const char* s, const char* key, char* out, int maxLen) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat); if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++; if (!*p) return false; p++;
    while (*p == ' ') p++;
    if (*p != '"') return false; p++;
    int i = 0;
    while (*p && *p != '"' && i < maxLen - 1) {
        // Handle basic JSON escape sequences
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = ' ';  break;
                case '"': out[i++] = '"';  break;
                case '\\':out[i++] = '\\'; break;
                default:  out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool jInt(const char* s, const char* key, long& out) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat); if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++; if (!*p) return false; p++;
    while (*p == ' ') p++;
    char* e; long v = strtol(p, &e, 10);
    if (e == p) return false;
    out = v; return true;
}

// Extract Nth value from JSON array of longs: "key":[1,2,3,...]
static bool jArrLong(const char* s, const char* key, int idx, long& out) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat); if (!p) return false;
    while (*p && *p != '[') p++; if (!*p) return false; p++;
    for (int i = 0; i < idx; i++) {
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ']') return false;
        p++;
    }
    while (*p == ' ') p++;
    char* e; long v = strtol(p, &e, 10);
    if (e == p) return false;
    out = v; return true;
}

// Strip HTML tags from a string in-place (for RSS/HN text)
static void stripHtml(char* s) {
    char* r = s; char* w = s;
    while (*r) {
        if (*r == '<') { while (*r && *r != '>') r++; if (*r) r++; }
        else if (*r == '&') {
            // Common HTML entities
            if (strncmp(r, "&amp;",  5)==0) { *w++='&'; r+=5; }
            else if (strncmp(r, "&lt;",   4)==0) { *w++='<'; r+=4; }
            else if (strncmp(r, "&gt;",   4)==0) { *w++='>'; r+=4; }
            else if (strncmp(r, "&quot;", 6)==0) { *w++='"'; r+=6; }
            else if (strncmp(r, "&#039;", 6)==0) { *w++='\'';r+=6; }
            else { *w++ = *r++; }
        }
        else { *w++ = *r++; }
    }
    *w = '\0';
}

// URL-encode a string (spaces→%20, etc.) into out buffer
// URL-encode for SEARCH queries: spaces become + (not underscore)
static void urlEncodeQuery(const char* in, char* out, int maxLen) {
    int j = 0;
    for (int i = 0; in[i] && j < maxLen - 4; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[j++] = c;
        } else if (c == ' ') {
            out[j++] = '+';   // search uses + for spaces
        } else {
            j += snprintf(out + j, maxLen - j, "%%%02X", (uint8_t)c);
        }
    }
    out[j] = '\0';
}

static void urlEncode(const char* in, char* out, int maxLen) {
    int j = 0;
    for (int i = 0; in[i] && j < maxLen - 4; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[j++] = c;
        } else if (c == ' ') {
            out[j++] = '_';   // Wikipedia uses _ for spaces
        } else {
            j += snprintf(out + j, maxLen - j, "%%%02X", (uint8_t)c);
        }
    }
    out[j] = '\0';
}

// Word-wrap text into rbBody, max ~28 chars per line, up to maxLines lines
static void wrapInto(const char* text, int maxChars, int maxLines) {
    rbBody[0] = '\0';
    int lineCount = 0;
    const char* p = text;
    char line[32];
    int lineLen = 0;

    while (*p && lineCount < maxLines) {
        if (*p == '\n' || *p == '\r') {
            if (lineLen > 0) {
                line[lineLen] = '\0';
                strncat(rbBody, line, sizeof(rbBody) - strlen(rbBody) - 2);
                strncat(rbBody, "\n", sizeof(rbBody) - strlen(rbBody) - 1);
                lineLen = 0; lineCount++;
            }
            while (*p == '\n' || *p == '\r') p++;
            continue;
        }
        if (lineLen >= maxChars) {
            // Find last space to word-wrap
            int lastSp = lineLen - 1;
            while (lastSp > 0 && line[lastSp] != ' ') lastSp--;
            if (lastSp > 0) {
                line[lastSp] = '\0';
                strncat(rbBody, line, sizeof(rbBody) - strlen(rbBody) - 2);
                strncat(rbBody, "\n", sizeof(rbBody) - strlen(rbBody) - 1);
                lineCount++;
                // rewind p to where word started
                p -= (lineLen - lastSp - 1);
                lineLen = 0;
            } else {
                // No space found — hard break
                line[lineLen] = '\0';
                strncat(rbBody, line, sizeof(rbBody) - strlen(rbBody) - 2);
                strncat(rbBody, "\n", sizeof(rbBody) - strlen(rbBody) - 1);
                lineCount++; lineLen = 0;
            }
            continue;
        }
        line[lineLen++] = *p++;
    }
    // Flush remaining
    if (lineLen > 0 && lineCount < maxLines) {
        line[lineLen] = '\0';
        strncat(rbBody, line, sizeof(rbBody) - strlen(rbBody) - 2);
    }
}

// ── Extract Wikipedia internal links from extract text ───────────────────────
// Wikipedia summary extract doesn't have links, but we can pull related titles
// from the links API endpoint and store a few as deep links.
static void fetchWikiDeepLinks(const char* title) {
    rbDeepCount = 0;
    char encTitle[128]; urlEncode(title, encTitle, sizeof(encTitle));
    char path[256];
    snprintf(path, sizeof(path),
        "/w/api.php?action=query&titles=%s"
        "&prop=links&pllimit=10&plnamespace=0&format=json",
        encTitle);

    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("en.wikipedia.org", path, rbBuf, sizeof(rbBuf))) return;

    // Parse "title":"..." from links array
    const char* p = strstr(rbBuf, "\"links\"");
    if (!p) return;
    while (rbDeepCount < 4) {
        p = strstr(p, "\"title\"");
        if (!p) break;
        p += 7;
        while (*p && *p != ':') p++; if (!*p) break; p++;
        while (*p == ' ') p++;
        if (*p != '"') break; p++;
        char t[80]; int i = 0;
        while (*p && *p != '"' && i < 79) t[i++] = *p++;
        t[i] = '\0';
        if (i > 0 && t[0] != ':' && strncmp(t,"Category:",9)!=0
            && strncmp(t,"Wikipedia:",10)!=0 && strncmp(t,"File:",5)!=0) {
            strncpy(rbDeepLinks[rbDeepCount], t, 79);
            rbDeepCount++;
        }
    }
    rbDeepType = 'W';
    // Shuffle deep links so "go deeper" doesn't always hit the same link
    for (int i = rbDeepCount - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        if (j != i) {
            char tmp[80];
            strncpy(tmp, rbDeepLinks[i], 79);
            strncpy(rbDeepLinks[i], rbDeepLinks[j], 79);
            strncpy(rbDeepLinks[j], tmp, 79);
        }
    }
}

// ── Scroll reset (call after every successful article load) ─────────────────────
static void rbResetScroll() {
    rbScrollOffset   = 0;
    rbScrollPausing  = true;
    rbScrollLastStep = millis();
    rbTotalLines = 0;
    const char* p = rbBody;
    bool lastWasNL = true;
    while (*p) {
        if (*p == '\n') { rbTotalLines++; lastWasNL = true; }
        else              { if (lastWasNL) {} lastWasNL = false; }
        p++;
    }
    int len = (int)strlen(rbBody);
    if (len > 0 && rbBody[len-1] != '\n') rbTotalLines++;
}

// ── FETCH: Wikipedia article ─────────────────────────────────────────────────
// Fetches a random article from a category, then its summary.
// If deepTitle is set, fetch that specific title instead.
static bool fetchWiki(const char* category, const char* deepTitle = nullptr) {
    char titleToFetch[128] = "";

    if (deepTitle && deepTitle[0]) {
        strncpy(titleToFetch, deepTitle, sizeof(titleToFetch) - 1);
    } else {
        // Get random article from category (cmlimit=50 for more variety)
        char path[256];
        // Fetch up to 50 category members — large pool means better randomness
        snprintf(path, sizeof(path),
            "/w/api.php?action=query&list=categorymembers"
            "&cmtitle=Category:%s&cmlimit=50&cmtype=page&format=json",
            category);
        memset(rbBuf, 0, sizeof(rbBuf));
        if (!rbGetS("en.wikipedia.org", path, rbBuf, sizeof(rbBuf))) return false;

        // Restrict search to categorymembers array only
        const char* cmStart = strstr(rbBuf, "\"categorymembers\"");
        if (!cmStart) return false;

        // Count title entries
        int count = 0; const char* p = cmStart;
        while ((p = strstr(p, "\"title\"")) != nullptr) { count++; p++; }
        if (count == 0) return false;

        // Try up to 8 random picks to find a non-category, non-list article
        for (int attempt = 0; attempt < 8 && !titleToFetch[0]; attempt++) {
            int pick = esp_random() % count;
            const char* q = cmStart; int idx = 0;
            while ((q = strstr(q, "\"title\"")) != nullptr) {
                if (idx == pick) {
                    q += 7;
                    while (*q && *q != ':') q++; if (!*q) break; q++;
                    while (*q == ' ') q++;
                    if (*q == '"') { q++; int i = 0;
                        while (*q && *q != '"' && i < 127) titleToFetch[i++] = *q++;
                        titleToFetch[i] = '\0';
                        // Skip namespace-prefixed and list articles
                        if (strncmp(titleToFetch, "Category:", 9) == 0 ||
                            strncmp(titleToFetch, "Wikipedia:", 10) == 0 ||
                            strncmp(titleToFetch, "List of", 7) == 0 ||
                            strncmp(titleToFetch, "File:", 5) == 0) {
                            titleToFetch[0] = '\0';
                        }
                    }
                    break;
                }
                idx++; q++;
            }
        }
        if (!titleToFetch[0]) return false;
    }

    // Fetch summary via REST API
    char encTitle[128]; urlEncode(titleToFetch, encTitle, sizeof(encTitle));
    char path[256];
    snprintf(path, sizeof(path), "/api/rest_v1/page/summary/%s", encTitle);
    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("en.wikipedia.org", path, rbBuf, sizeof(rbBuf))) return false;

    // Parse title + extract
    char extractedTitle[80] = ""; char extract[512] = "";
    jStr(rbBuf, "title",   extractedTitle, sizeof(extractedTitle));
    jStr(rbBuf, "extract", extract,        sizeof(extract));

    if (!extractedTitle[0]) return false;

    strncpy(rbTitle, extractedTitle, sizeof(rbTitle) - 1);
    wrapInto(extract, 27, 50);

    // Fetch deep links
    fetchWikiDeepLinks(extractedTitle);
    rbResetScroll();
    return true;
}

// ── FETCH: Hacker News ───────────────────────────────────────────────────────
static bool fetchHN(bool getComment = false) {
    if (getComment && rbHnStoryId > 0) {
        // Fetch story to get kids list
        char path[64]; snprintf(path, sizeof(path), "/v0/item/%ld.json", rbHnStoryId);
        memset(rbBuf, 0, sizeof(rbBuf));
        if (!rbGetS("hacker-news.firebaseio.com", path, rbBuf, sizeof(rbBuf))) return false;

        long kid = 0;
        if (!jArrLong(rbBuf, "kids", 0, kid)) {
            // No comments — just show score and author again
            strncpy(rbTitle, "NO COMMENTS YET", sizeof(rbTitle) - 1);
            strncpy(rbBody, "This story has no\ncomments yet.", sizeof(rbBody) - 1);
            rbDeepCount = 0;
            return true;
        }
        rbHnCommentId = kid;

        // Fetch the comment
        snprintf(path, sizeof(path), "/v0/item/%ld.json", kid);
        memset(rbBuf, 0, sizeof(rbBuf));
        if (!rbGetS("hacker-news.firebaseio.com", path, rbBuf, sizeof(rbBuf))) return false;

        char by[40] = ""; char text[512] = "";
        jStr(rbBuf, "by",   by,   sizeof(by));
        jStr(rbBuf, "text", text, sizeof(text));
        stripHtml(text);

        snprintf(rbTitle, sizeof(rbTitle), "by %s", by);
        wrapInto(text, 27, 50);
        rbDeepCount = 0;  // can't go deeper from a comment
        rbDeepType = 'H';
        rbResetScroll();
        return true;
    }

    // Fetch top stories list
    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("hacker-news.firebaseio.com", "/v0/topstories.json",
               rbBuf, sizeof(rbBuf))) return false;

    // Count IDs in array (up to 200 for variety)
    int count = 0; const char* p = rbBuf;
    while (*p && count < 200) {
        if (*p == ',' || *p == '[') { count++; p++; }
        else p++;
    }
    if (count < 1) count = 1;

    // topstories returns a bare array [id1,id2,...] — parse directly
    long storyId = 0;
    int pick = esp_random() % (count > 50 ? 50 : count);  // pick from top 50
    {
        p = rbBuf;
        while (*p && *p != '[') p++;
        if (*p) p++;
        for (int i = 0; i < pick && *p; i++) {
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
        while (*p == ' ') p++;
        char* e; storyId = strtol(p, &e, 10);
        if (e == p || storyId <= 0) return false;
    }
    rbHnStoryId = storyId;

    // Fetch story
    char path[64]; snprintf(path, sizeof(path), "/v0/item/%ld.json", storyId);
    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("hacker-news.firebaseio.com", path, rbBuf, sizeof(rbBuf))) return false;

    char title[128] = ""; char by[40] = ""; long score = 0; long kids = 0;
    jStr(rbBuf, "title", title, sizeof(title));
    jStr(rbBuf, "by",    by,    sizeof(by));
    jInt(rbBuf, "score", score);

    if (!title[0]) return false;

    // Show full title - don't truncate
    strncpy(rbTitle, title, sizeof(rbTitle) - 1);

    long numKids = 0; jInt(rbBuf, "descendants", numKids);
    char body[320];
    // Build body: score/author on compact line then instructions
    snprintf(body, sizeof(body),
        "%ld pts  by %s\n%ld comments\n\nR: top comment\nL: new story",
        score, by, numKids);
    wrapInto(body, 27, 50);

    // Deep link is the comment thread
    rbDeepCount = 1;
    snprintf(rbDeepLinks[0], sizeof(rbDeepLinks[0]), "%ld", storyId);
    rbDeepType = 'H';
    rbResetScroll();
    return true;
}

// ── FETCH: GitHub repos ───────────────────────────────────────────────────────
static bool fetchGithub(const char* topic, bool deeper = false,
                        const char* repoFullName = nullptr) {
    if (deeper && repoFullName && repoFullName[0]) {
        // Fetch the README as RAW markdown (not base64 JSON) using the
        // GitHub media-type Accept header. api.github.com redirects raw
        // content to a CDN, which rbGetSAccept follows.
        char path[128];
        snprintf(path, sizeof(path), "/repos/%s/readme", repoFullName);
        memset(rbBuf, 0, sizeof(rbBuf));
        bool ok = rbGetSAccept("api.github.com", path,
                               "application/vnd.github.raw+json",
                               rbBuf, sizeof(rbBuf));
        strncpy(rbTitle, "README", sizeof(rbTitle) - 1);
        if (ok && rbBuf[0]) {
            // Strip markdown noise IN PLACE inside rbBuf (no big stack array).
            int w = 0;
            for (int i = 0; rbBuf[i]; i++) {
                char ch = rbBuf[i];
                if (ch == '#' || ch == '`' || ch == '*' || ch == '>' || ch == '\r')
                    continue;
                rbBuf[w++] = ch;
            }
            rbBuf[w] = '\0';
            wrapInto(rbBuf, 27, 200);
        } else {
            strncpy(rbBody, "README unavailable\n\nLEFT: new project",
                    sizeof(rbBody) - 1);
            rbResetScroll();
        }
        rbDeepCount = 0;
        return true;
    }

    // Pick a random query term from pipe-separated list for variety
    // e.g. "arduino|esp32|microcontroller" -> pick one randomly
    char chosenTopic[48] = "";
    {
        char topicCopy[64]; strncpy(topicCopy, topic, 63); topicCopy[63] = '\0';
        // Count pipe-separated options
        int opts = 1;
        for (char* c = topicCopy; *c; c++) if (*c == '|') opts++;
        int pick = esp_random() % opts;
        char* tok = strtok(topicCopy, "|");
        for (int i = 0; i < pick && tok; i++) tok = strtok(nullptr, "|");
        if (tok) strncpy(chosenTopic, tok, 47);
        else     strncpy(chosenTopic, topic, 47);
    }

    // Randomize page and sort for variety
    static const char* sorts[] = {"stars","updated","created"};
    int sortIdx = esp_random() % 3;
    int page = 1 + (esp_random() % 5);  // pages 1-5 for variety

    char path[256];
    snprintf(path, sizeof(path),
        "/search/repositories?q=topic:%s&sort=%s"
        "&per_page=10&page=%d",
        chosenTopic, sorts[sortIdx], page);

    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("api.github.com", path, rbBuf, sizeof(rbBuf))) return false;

    // Count items
    const char* p = strstr(rbBuf, "\"items\"");
    if (!p) return false;
    int count = 0; const char* q = p;
    while ((q = strstr(q + 1, "\"full_name\"")) != nullptr) count++;
    if (count == 0) return false;

    int pick = esp_random() % count;

    // Find Nth item by locating the Nth "full_name" key then backing up
    // to the start of that object { so all fields are reachable
    q = p;
    for (int i = 0; i <= pick; i++) {
        q = strstr(q + 1, "\"full_name\"");
        if (!q) return false;
    }
    // Back up to find the { that starts this repo object
    const char* objStart = q;
    while (objStart > p && *objStart != '{') objStart--;

    char fullName[80] = ""; char desc[200] = "";
    char lang[32] = ""; long stars = 0;
    jStr(objStart, "full_name",       fullName, sizeof(fullName));
    jStr(objStart, "description",     desc,     sizeof(desc));
    jStr(objStart, "language",        lang,     sizeof(lang));
    jInt(objStart, "stargazers_count", stars);

    if (!fullName[0]) return false;

    // Title = repo name (after /)
    const char* slash = strchr(fullName, '/');
    strncpy(rbTitle, slash ? slash + 1 : fullName, sizeof(rbTitle) - 1);

    char body[300];
    snprintf(body, sizeof(body),
        "%s\n\n%s\n\nStars: %ld\nLang: %s",
        fullName, desc[0] ? desc : "(no description)", stars,
        lang[0] ? lang : "?");
    wrapInto(body, 27, 50);

    rbDeepCount = 1;
    strncpy(rbDeepLinks[0], fullName, 79);
    rbDeepType = 'G';
    rbResetScroll();
    return true;
}

// ── FETCH: RSS feed ───────────────────────────────────────────────────────────
static bool fetchRSS(const char* siteName, bool deeper = false,
                     const char* lookupTitle = nullptr) {
    if (deeper && lookupTitle && lookupTitle[0]) {
        // Go deeper: Wikipedia lookup of the title as a search
        char encTitle[128]; urlEncodeQuery(lookupTitle, encTitle, sizeof(encTitle));
        char path[256];
        snprintf(path, sizeof(path),
            "/w/api.php?action=query&list=search"
            "&srsearch=%s&srlimit=1&format=json", encTitle);
        memset(rbBuf, 0, sizeof(rbBuf));
        if (rbGetS("en.wikipedia.org", path, rbBuf, sizeof(rbBuf))) {
            char foundTitle[80] = "";
            const char* p = strstr(rbBuf, "\"title\"");
            if (p) {
                p += 7; while (*p && *p != ':') p++;
                if (*p) { p++; while (*p == ' ') p++;
                    if (*p == '"') { p++; int i = 0;
                        while (*p && *p != '"' && i < 79) foundTitle[i++] = *p++;
                        foundTitle[i] = '\0';
                    }
                }
            }
            if (foundTitle[0]) return fetchWiki(nullptr, foundTitle);
        }
        // Fallback: show what we have
        strncpy(rbTitle, "WIKI NOT FOUND", sizeof(rbTitle) - 1);
        strncpy(rbBody, "Wikipedia search\nreturned no results.\n\nLEFT: new article", sizeof(rbBody)-1);
        rbDeepCount = 0; rbResetScroll(); return true;
    }

    // Choose feed URL based on site
    const char* host;
    const char* path;
    bool useHttps = false;

    bool needsHttps = false;
    static char hackadayPath[48];  // must persist for the pointer
    if (strcmp(siteName, "hackaday.com") == 0) {
        host = "hackaday.com";
        int hpage = 1 + (esp_random() % 50);  // pages 1-50 for variety
        snprintf(hackadayPath, sizeof(hackadayPath), "/blog/feed/?paged=%d", hpage);
        path = hackadayPath;
    } else if (strcmp(siteName, "coindesk.com") == 0) {
        host = "www.coindesk.com"; path = "/arc/outboundfeeds/rss";
        needsHttps = true;
    } else {
        host = "lobste.rs"; path = "/rss";
    }

    memset(rbBuf, 0, sizeof(rbBuf));
    bool ok = needsHttps
        ? rbGetS(host, path, rbBuf, sizeof(rbBuf))
        : rbGet(host, path, rbBuf, sizeof(rbBuf));
    if (!ok && !needsHttps) ok = rbGetS(host, path, rbBuf, sizeof(rbBuf));
    if (!ok) return false;

    // Count <item> tags
    int count = 0; const char* p = rbBuf;
    while ((p = strstr(p + 1, "<item")) != nullptr) count++;
    if (count == 0) return false;

    // Pick randomly from up to 20 items for variety
    int maxPick = count > 20 ? 20 : count;
    int pick = esp_random() % maxPick;

    // Find Nth item
    p = rbBuf;
    for (int i = 0; i <= pick; i++) {
        p = strstr(p + 1, "<item");
        if (!p) return false;
    }

    // Extract <title> and <description> within this item
    char title[128] = ""; char descRaw[400] = "";

    const char* tp = strstr(p, "<title>");
    if (tp && tp - p < 2000) {
        tp += 7;
        // skip CDATA
        if (strncmp(tp, "<![CDATA[", 9) == 0) tp += 9;
        int i = 0;
        while (*tp && strncmp(tp, "]]>", 3) != 0 &&
               *tp != '<' && i < 127) title[i++] = *tp++;
        title[i] = '\0';
    }

    const char* dp = strstr(p, "<description>");
    if (dp && dp - p < 3000) {
        dp += 13;
        if (strncmp(dp, "<![CDATA[", 9) == 0) dp += 9;
        int i = 0;
        while (*dp && strncmp(dp, "]]>", 3) != 0 &&
               strncmp(dp, "</description>", 14) != 0 && i < 399)
            descRaw[i++] = *dp++;
        descRaw[i] = '\0';
        stripHtml(descRaw);
    }

    if (!title[0]) return false;

    strncpy(rbTitle, title, sizeof(rbTitle) - 1);
    stripHtml(rbTitle);

    char body[512];
    snprintf(body, sizeof(body), "%s\n\nRIGHT: Wikipedia\non this topic",
             descRaw[0] ? descRaw : "(no description)");
    wrapInto(body, 27, 50);

    rbDeepCount = 1;
    strncpy(rbDeepLinks[0], title, 79);
    rbDeepType = 'X';   // Wikipedia lookup
    rbResetScroll();
    return true;
}

// ── FETCH: arXiv ─────────────────────────────────────────────────────────────
static bool fetchArxiv(const char* category, bool deeper = false,
                       const char* keyword = nullptr) {
    if (deeper && keyword && keyword[0]) {
        // Go deeper: Wikipedia on first keyword
        return fetchWiki(nullptr, keyword);
    }

    // Pick random category from pipe-separated list
    char chosenCat[32] = "";
    {
        char catCopy[64]; strncpy(catCopy, category, 63); catCopy[63] = '\0';
        int opts = 1;
        for (char* c = catCopy; *c; c++) if (*c == '|') opts++;
        int pick = esp_random() % opts;
        char* tok = strtok(catCopy, "|");
        for (int i = 0; i < pick && tok; i++) tok = strtok(nullptr, "|");
        if (tok) strncpy(chosenCat, tok, 31);
        else     strncpy(chosenCat, category, 31);
    }

    // Randomize start position for variety (0-40)
    int startPos = (esp_random() % 5) * 10;

    char path[256];
    snprintf(path, sizeof(path),
        "/api/query?search_query=cat:%s"
        "&sortBy=submittedDate&max_results=10&start=%d",
        chosenCat, startPos);

    memset(rbBuf, 0, sizeof(rbBuf));
    // ArXiv can be slow - retry once on failure
    if (!rbGetS("export.arxiv.org", path, rbBuf, sizeof(rbBuf))) {
        delay(500);
        memset(rbBuf, 0, sizeof(rbBuf));
        if (!rbGetS("export.arxiv.org", path, rbBuf, sizeof(rbBuf))) return false;
    }

    // Count <entry> tags
    int count = 0; const char* p = rbBuf;
    while ((p = strstr(p + 1, "<entry>")) != nullptr) count++;
    if (count == 0) return false;

    int pick = esp_random() % count;
    p = rbBuf;
    for (int i = 0; i <= pick; i++) {
        p = strstr(p + 1, "<entry>");
        if (!p) return false;
    }

    // Extract <title> and <summary>
    char title[128] = ""; char summary[512] = "";

    const char* tp = strstr(p, "<title>");
    if (tp && tp - p < 3000) {
        tp += 7;
        int i = 0;
        while (*tp && strncmp(tp, "</title>", 8) != 0 && i < 127)
            title[i++] = *tp++;
        title[i] = '\0';
        // Trim newlines
        for (int k = 0; k < i; k++) if (title[k] == '\n' || title[k] == '\r') title[k] = ' ';
    }

    const char* sp2 = strstr(p, "<summary>");
    if (sp2 && sp2 - p < 4000) {
        sp2 += 9;
        int i = 0;
        while (*sp2 && strncmp(sp2, "</summary>", 10) != 0 && i < 511)
            summary[i++] = *sp2++;
        summary[i] = '\0';
        // Trim leading whitespace
        char* t = summary;
        while (*t == ' ' || *t == '\n' || *t == '\r') t++;
        memmove(summary, t, strlen(t) + 1);
        // Replace newlines with spaces
        for (int k = 0; summary[k]; k++) if (summary[k] == '\n' || summary[k] == '\r') summary[k] = ' ';
    }

    if (!title[0]) return false;

    strncpy(rbTitle, title, sizeof(rbTitle) - 1);
    wrapInto(summary, 27, 50);

    // Extract first "interesting" word for deeper Wikipedia lookup
    rbDeepCount = 0;
    // Find first capitalized word longer than 4 chars in title
    const char* w = title;
    while (*w) {
        while (*w && (*w < 'A' || *w > 'Z')) w++;
        if (!*w) break;
        const char* wEnd = w;
        while (*wEnd && *wEnd != ' ' && *wEnd != ',' && *wEnd != ':') wEnd++;
        int wLen = (int)(wEnd - w);
        if (wLen > 4) {
            strncpy(rbDeepLinks[0], w, wLen < 79 ? wLen : 79);
            rbDeepLinks[0][wLen < 79 ? wLen : 79] = '\0';
            rbDeepCount = 1;
            rbDeepType = 'X';
            break;
        }
        w = wEnd;
        if (*w) w++;
    }

    rbResetScroll();
    return true;
}

// ── FETCH: Wikipedia search (better variety than categorymembers) ────────────
static bool fetchWikiSearch(const char* query, const char* deepTitle = nullptr) {
    if (deepTitle && deepTitle[0]) return fetchWiki(nullptr, deepTitle);

    // The topic param may hold several alternative queries separated by '|'.
    // Pick one at random so a topic that returns thin results on one phrasing
    // gets other chances (e.g. "retrocomputing" vs "vintage computer").
    char chosen[160];
    {
        // Count alternatives
        int alts = 1;
        for (const char* s = query; *s; s++) if (*s == '|') alts++;
        int pick = esp_random() % alts;
        // Copy the pick-th segment into chosen[]
        const char* s = query; int seg = 0; int i = 0;
        while (*s && seg < pick) { if (*s == '|') seg++; s++; }
        while (*s && *s != '|' && i < 159) chosen[i++] = *s++;
        chosen[i] = '\0';
        if (!chosen[0]) { strncpy(chosen, query, 159); chosen[159] = '\0'; }
    }

    // Use Wikipedia search API for a broad topic query
    char encQ[128]; urlEncodeQuery(chosen, encQ, sizeof(encQ));
    int offset = (esp_random() % 5) * 10;  // random result page
    char path[256];
    snprintf(path, sizeof(path),
        "/w/api.php?action=query&list=search"
        "&srsearch=%s&srlimit=50&sroffset=%d"
        "&srnamespace=0&format=json",
        encQ, offset);

    memset(rbBuf, 0, sizeof(rbBuf));
    if (!rbGetS("en.wikipedia.org", path, rbBuf, sizeof(rbBuf))) return false;

    // Parse search results: "search":[{"title":"..."}...]
    const char* sr = strstr(rbBuf, "\"search\"");
    if (!sr) return false;
    int count = 0; const char* p = sr;
    while ((p = strstr(p+1, "\"title\"")) != nullptr) count++;
    if (count == 0) return false;

    int pick = esp_random() % count;
    char titleToFetch[128] = "";
    p = sr;
    for (int idx = 0; idx <= pick; idx++) {
        p = strstr(p+1, "\"title\"");
        if (!p) return false;
    }
    p += 7;
    while (*p && *p != ':') p++; if (!*p) return false; p++;
    while (*p == ' ') p++;
    if (*p == '"') { p++; int i = 0;
        while (*p && *p != '"' && i < 127) titleToFetch[i++] = *p++;
        titleToFetch[i] = '\0';
    }
    if (!titleToFetch[0]) return false;
    return fetchWiki(nullptr, titleToFetch);
}

// Known-good JPEG fetch — verifies the whole image pipeline (HTTPS fetch with
// redirect, TJpgDec decode, sprite render) independently of any network host. Uses a
// small Wikimedia Commons JPEG thumbnail served over HTTPS.
static bool fetchTestImg(const char*) {
    rbHasImage = true;
    rbDeepCount = 0;
    rbDeepType = 'I';
    strncpy(rbTitle, "IMAGE TEST", sizeof(rbTitle)-1);
    strncpy(rbBody, "L: reload\nhold: next topic", sizeof(rbBody)-1);
    rbResetScroll();

    // NO NETWORK: decode a JPEG that is compiled into the firmware. This proves
    // the decode + render pipeline works independently of any fetch. If THIS
    // shows the colored squares, the renderer is fine and every other failure
    // has been the network/host. If it doesn't, the bug is local.
    rbJpegReady = false;
    if (!rbJpeg) { strncpy(rbStatus, "TEST: no buffer", sizeof(rbStatus)-1); return true; }
    if (TEST_JPEG_LEN > g_jpegBufLen) { strncpy(rbStatus, "TEST: too big", sizeof(rbStatus)-1); return true; }

    memcpy(rbJpeg, TEST_JPEG, TEST_JPEG_LEN);
    rbJpegLen = TEST_JPEG_LEN;

    uint16_t jw = 0, jh = 0;
    TJpgDec.setJpgScale(1);
    if (TJpgDec.getJpgSize(&jw, &jh, rbJpeg, rbJpegLen) != JDR_OK || jw == 0) {
        strncpy(rbStatus, "TEST: getJpgSize failed", sizeof(rbStatus)-1);
        rbImageUrl[0] = 'x'; rbImageUrl[1] = '\0';   // mark hasImage path
        return true;
    }
    int scale = 1;
    while ((jw / scale > W || jh / scale > (H - 40)) && scale < 8) scale *= 2;
    rbJpegScale = scale;
    rbJpegW = jw / scale;
    rbJpegH = jh / scale;
    rbJpegOriginX = (W - rbJpegW) / 2;  if (rbJpegOriginX < 0) rbJpegOriginX = 0;
    rbJpegOriginY = 20 + ((H - 44 - rbJpegH) / 2);
    if (rbJpegOriginY < 20) rbJpegOriginY = 20;
    rbJpegReady = true;
    strncpy(rbImageUrl, "embedded-test", sizeof(rbImageUrl)-1);  // non-empty so draw runs
    snprintf(rbStatus, sizeof(rbStatus), "TEST OK %dx%d s%d", jw, jh, scale);
    return true;
}


// ── Central fetch dispatcher ─────────────────────────────────────────────────
static bool rbFetch(bool deeper = false) {
    const Topic& t = TOPICS[rbTopicIdx];
    rbHasImage = false;  // clear image flag for non-image topics
    rbStatus[0] = '\0';  // clear any stale mode-specific diagnostic

    if (deeper && rbDeepCount > 0) {
        switch (rbDeepType) {
            case 'W': return fetchWiki(nullptr, rbDeepLinks[0]);
            case 'H': return fetchHN(true);
            case 'G': return fetchGithub(t.param, true, rbDeepLinks[0]);
            case 'X': return fetchRSS(t.param, true, rbDeepLinks[0]);
        }
    }

    // New random article
    switch (t.fetchType) {
        case FETCH_WIKI:  return fetchWiki(t.param);
        case FETCH_HN:    return fetchHN(false);
        case FETCH_GH:    return fetchGithub(t.param);
        case FETCH_RSS:   return fetchRSS(t.param);
        case FETCH_ARXIV: return fetchArxiv(t.param);
        case FETCH_SRCH:  return fetchWikiSearch(t.param);
        case FETCH_TESTIMG: return fetchTestImg(t.param);
    }
    return false;
}

// ── Button tick (called from rabbitTick) ─────────────────────────────────────
static void rbTickButtons() {
    rbDoDeeper    = false;
    rbDoNew       = false;
    rbDoNextTopic = false;

    uint32_t now = millis();
    bool rDown = btnR && !btnL;
    bool lDown = btnL && !btnR;
    bool rEdge = rDown && !rbRWasDown;
    bool lEdge = lDown && !rbLWasDown;

    // Track hold starts
    if (rDown) {
        if (rbRHoldStart == 0) { rbRHoldStart = now; rbRFired = false; }
        if (!rbRFired && (now - rbRHoldStart) >= RB_LONG_MS) {
            rbRFired = true;
            rbDoNextTopic = true;
        }
    } else {
        // Released — if not a long press, it was a single click
        if (rbRWasDown && !rbRFired) rbDoDeeper = true;
        rbRHoldStart = 0; rbRFired = false;
    }

    if (lDown) {
        if (rbLHoldStart == 0) { rbLHoldStart = now; rbLFired = false; }
        if (!rbLFired && (now - rbLHoldStart) >= RB_LONG_MS) {
            rbLFired = true;
            rbDoNextTopic = true;
        }
    } else {
        if (rbLWasDown && !rbLFired) rbDoNew = true;
        rbLHoldStart = 0; rbLFired = false;
    }

    rbRWasDown = rDown;
    rbLWasDown = lDown;
}

// ── Draw screens ─────────────────────────────────────────────────────────────
static void rbDrawLoading() {
    fb.fillSprite(TEAL_DEEP);

    // Topic badge
    fb.fillRect(0, 0, W, 16, TEAL_DARK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME);
    fb.drawString(TOPICS[rbTopicIdx].badge, W/2, 8, 1);

    // Loading indicator
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("LOADING...", W/2, 140, 2);
    fb.setTextColor(TEAL);
    fb.drawString(rbStatus, W/2, 165, 1);

    // Spinner dots
    uint32_t t = millis() / 250;
    for (int i = 0; i < 5; i++) {
        uint16_t c = ((t % 5) == (uint32_t)i) ? LIME : TEAL_DARK;
        fb.fillCircle(W/2 - 32 + i * 16, 185, 5, c);
    }

    // Bottom hint
    fb.fillRect(0, H - 22, W, 22, TEAL_DARK);
    fb.setTextColor(TEAL);
    fb.drawString("hold btn: NEXT TOPIC", W/2, H - 11, 1);

    fb.pushSprite(0, 0);
}

static void rbDrawError() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 16, TEAL_DARK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME);
    fb.drawString(TOPICS[rbTopicIdx].badge, W/2, 8, 1);

    fb.setTextColor(PURPLE_BRIGHT);
    fb.drawString("FETCH ERROR", W/2, 130, 2);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(rbStatus, W/2, 155, 1);
    fb.setTextColor(TEAL);
    fb.drawString("L: new  hold: next topic", W/2, 175, 1);

    fb.fillRect(0, H - 22, W, 22, TEAL_DARK);
    fb.setTextColor(TEAL);
    fb.drawString("hold btn: NEXT TOPIC", W/2, H - 11, 1);

    fb.pushSprite(0, 0);
}

static void rbDrawDisplay() {
    fb.fillSprite(TEAL_DEEP);

    // ── For image topics: show image instead of text ──────────────────
    if (rbHasImage && rbImageUrl[0]) {
        fb.fillRect(0, 0, W, 16, TEAL_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(TOPICS[rbTopicIdx].name, 4, 8, 1);
        if (rbJpegReady && rbJpegLen > 0 && rbJpeg) {
            // Render the decoded JPEG into the fb sprite via TJpgDec callback
            TJpgDec.setJpgScale(rbJpegScale);
            TJpgDec.setCallback(rbJpgOutput);
            // drawJpg's x,y are added by TJpgDec to each block before the
            // callback. Our callback ALSO adds rbJpegOrigin, so pass 0,0 here
            // to avoid double-offsetting the image off-screen.
            TJpgDec.drawJpg(0, 0, rbJpeg, rbJpegLen);
            fb.setTextColor(TEAL); fb.setTextDatum(TC_DATUM);
            int capY = rbJpegOriginY + rbJpegH + 4;
            if (capY < H - 26) fb.drawString(rbTitle, W/2, capY, 1);
        } else {
            fb.setTextColor(TEAL_BRIGHT); fb.setTextDatum(TL_DATUM);
            fb.drawString(rbTitle, 4, 24, 2);
            fb.setTextColor(TEAL_DARK); fb.setTextDatum(MC_DATUM);
            // Show the specific reason (rbStatus holds the image diagnostic) so
            // we can tell a fetch failure from a decode failure on-device.
            fb.drawString(rbStatus[0] ? rbStatus : "image unavailable", W/2, H/2, 1);
        }
        fb.fillRect(0, H-22, W, 22, TEAL_DARK);
        fb.setTextColor(TEAL_BRIGHT); fb.setTextDatum(ML_DATUM);
        fb.drawString(" NEW", 4, H-11, 1);
        fb.setTextDatum(MR_DATUM);
        fb.drawString("RELATED ", W-4, H-11, 1);
        fb.pushSprite(0,0);
        return;
    }

    // ── Topic badge bar ────────────────────────────────────────────────────
    fb.fillRect(0, 0, W, 16, TEAL_DARK);
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME);
    fb.drawString(TOPICS[rbTopicIdx].name, 4, 8, 1);
    // Depth indicator on right
    if (rbDepth > 0) {
        char depBuf[8]; snprintf(depBuf, sizeof(depBuf), "D+%d", rbDepth);
        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString(depBuf, W - 4, 8, 1);
    }

    // ── Title (up to 3 lines, word-wrapped) ───────────────────────────────
    // Font 2 at ~170px wide = ~18 chars per line
    // We draw it manually to wrap it
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(TL_DATUM);
    // Simple wrap: break title at 18 chars at word boundary
    {
        char tmp[80]; strncpy(tmp, rbTitle, 79); tmp[79] = '\0';
        int titleY = 20; int line = 0;
        char* p = tmp;
        while (*p && line < 3) {
            // Find break point
            int maxC = 18;
            char* q = p + strlen(p) <= p + maxC ? p + strlen(p) : p + maxC;
            // Step back to word boundary
            if (*q != '\0' && *q != '\0') {
                char* b = q;
                while (b > p && *b != ' ') b--;
                if (b > p) q = b;
            }
            char saved = *q; *q = '\0';
            fb.drawString(p, 4, titleY, 2);
            *q = saved;
            if (*q == ' ') q++;
            p = q;
            titleY += 18; line++;
        }
    }

    // ── Divider ───────────────────────────────────────────────────────────
    int bodyY = 20 + 3 * 18 + 2;  // = 76
    fb.drawFastHLine(0, bodyY, W, TEAL_DARK);
    bodyY += 4;  // = 80

    // ── Body text (with scroll offset) ──────────────────────────────────────
    fb.setTextColor(TEAL_BRIGHT);
    fb.setTextDatum(TL_DATUM);
    // Skip rbScrollOffset lines, then draw up to RB_VISIBLE_LINES lines
    int y = bodyY;
    const char* p = rbBody;
    int lineIdx = 0;
    // Skip to scroll offset
    while (*p && lineIdx < rbScrollOffset) {
        const char* end = strchr(p, '\n');
        if (end) { p = end + 1; lineIdx++; }
        else break;
    }
    // Draw visible lines
    int drawn = 0;
    while (*p && y < 270 && drawn < RB_VISIBLE_LINES) {
        const char* end = strchr(p, '\n');
        int len = end ? (int)(end - p) : (int)strlen(p);
        char line[32]; int toCopy = len < 31 ? len : 31;
        strncpy(line, p, toCopy); line[toCopy] = '\0';
        fb.drawString(line, 4, y, 1);
        y += 9; drawn++;
        if (end) p = end + 1; else break;
    }
    // Scroll indicator: small dot on right edge if scrollable
    if (rbTotalLines > RB_VISIBLE_LINES) {
        // Draw a tiny scroll bar on the far right
        int barH = (int)((float)RB_VISIBLE_LINES / rbTotalLines * 180.0f);
        if (barH < 4) barH = 4;
        int barY = bodyY + (int)((float)rbScrollOffset / rbTotalLines * 180.0f);
        fb.fillRect(W - 3, bodyY, 3, 180, TEAL_DARK);
        fb.fillRect(W - 3, barY, 3, barH, TEAL);
    }

    // ── Bottom info bar ───────────────────────────────────────────────────
    fb.drawFastHLine(0, 272, W, TEAL_DARK);

    // Deep link hint
    if (rbDeepCount > 0) {
        char hint[36];
        switch (rbDeepType) {
            case 'W': snprintf(hint, sizeof(hint), "> %s", rbDeepLinks[0]); break;
            case 'H': strncpy(hint, "> top comment", 35); break;
            case 'G': snprintf(hint, sizeof(hint), "> %s", rbDeepLinks[0]); break;
            case 'X': snprintf(hint, sizeof(hint), "> wiki: %.20s", rbDeepLinks[0]); break;
            default:  strncpy(hint, "> deeper", 35); break;
        }
        hint[35] = '\0';
        fb.setTextColor(TEAL);
        fb.setTextDatum(TL_DATUM);
        fb.drawString(hint, 2, 276, 1);
    }

    // Controls footer
    fb.fillRect(0, H - 22, W, 22, TEAL_DARK);
    fb.setTextColor(TEAL_BRIGHT);
    fb.setTextDatum(ML_DATUM);
    fb.drawString("\x11 NEW", 4, H - 11, 1);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TEAL);
    fb.drawString("hold: TOPIC", W/2, H - 11, 1);
    fb.setTextDatum(MR_DATUM);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("DEEP \x10", W - 4, H - 11, 1);

    fb.pushSprite(0, 0);
}

// ── Public API ─────────────────────────────────────────────────────────────────
void rabbitEnter() {
    // Load WiFi from btcminer NVS (read-only)
    { Preferences p;
      if (p.begin("btcminer", true)) {
          p.getString("ssid", rbSsid, sizeof(rbSsid));
          p.getString("pass", rbPass, sizeof(rbPass));
          p.end();
      }
    }

    // Pick a random starting topic from the enabled set
    rbBuildEnabledList();
    rbEnabledPos = esp_random() % rbEnabledCount;
    rbTopicIdx   = rbEnabled[rbEnabledPos];
    rbDepth    = 0;
    rbTitle[0] = '\0'; rbBody[0] = '\0';
    rbDeepCount= 0;
    rbHnStoryId = 0;
    rbHasImage = false; rbImageUrl[0] = 0;
    rbJpegReady = false; rbJpegLen = 0;
    // TJpgDec setup: pixels go into the fb sprite which needs byte-swapped data
    // The sprite does the byte swap in pushImage (see rbJpgOutput). Make sure
    // TJpgDec does NOT also swap, or the two cancel out and colors break.
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(rbJpgOutput);

    // Clear button state
    rbRHoldStart = rbLHoldStart = 0;
    rbRFired = rbLFired = false;
    rbRWasDown = rbLWasDown = false;
    rbDoDeeper = rbDoNew = rbDoNextTopic = false;

    jpegBufAcquire();   // grab the shared image buffer for this session

    // The embedded IMAGE TEST needs no WiFi — go straight to the loading/decode
    // phase even if no network is configured.
    if (TOPICS[rbTopicIdx].fetchType == FETCH_TESTIMG) {
        rbPhase = RB_LOADING; rbPhaseAt = millis();
        rbNeedsFetch = false; rbFetchDeeper = false;
        strncpy(rbStatus, "DECODING TEST", sizeof(rbStatus)-1);
        return;
    }

    if (rbSsid[0] == '\0') {
        rbPhase = RB_ERROR; rbPhaseAt = millis();
        strncpy(rbStatus, "NO WIFI - SET UP MINER FIRST", sizeof(rbStatus)-1);
        return;
    }

    rbPhase = RB_WIFI; rbPhaseAt = millis();
    rbNeedsFetch = false; rbFetchDeeper = false;
    snprintf(rbStatus, sizeof(rbStatus), "CONNECTING: %s", rbSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(rbSsid, rbPass);
}

void rabbitShutdown() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);
    jpegBufRelease();   // return the shared image buffer to the heap
    // fbReady is already true; we never swapped to landscape
}

void rabbitTick() {
    uint32_t now = millis();
    rbTickButtons();

    switch (rbPhase) {

        case RB_WIFI:
            // The embedded IMAGE TEST needs no network — go straight to loading.
            if (TOPICS[rbTopicIdx].fetchType == FETCH_TESTIMG) {
                rbPhase = RB_LOADING; rbPhaseAt = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                strncpy(rbStatus, "DECODING TEST", sizeof(rbStatus)-1);
                break;
            }
            if (WiFi.status() == WL_CONNECTED) {
                rbPhase      = RB_LOADING;
                rbPhaseAt    = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                snprintf(rbStatus, sizeof(rbStatus),
                         "FETCHING %s", TOPICS[rbTopicIdx].badge);
            } else if (now - rbPhaseAt > 15000) {
                rbPhase = RB_ERROR; rbPhaseAt = now;
                strncpy(rbStatus, "WIFI TIMEOUT", sizeof(rbStatus)-1);
            }
            break;

        case RB_LOADING:
            // Two-tick pattern: first entry draws loading screen (rbNeedsFetch=true),
            // second tick does the blocking HTTP fetch.
            if (rbNeedsFetch) {
                rbNeedsFetch = false;
                bool ok = rbFetch(rbFetchDeeper);
                rbFetchDeeper = false;
                if (ok) { rbPhase = RB_DISPLAY; }
                else    { rbPhase = RB_ERROR;
                          strncpy(rbStatus, "FETCH FAILED - L: retry", sizeof(rbStatus)-1); }
                rbPhaseAt = now;
            }
            // else: just drew loading screen last frame, set flag for next frame
            else { rbNeedsFetch = true; }
            break;

        case RB_DISPLAY:
            // Auto-scroll if content exceeds visible lines
            if (rbTotalLines > RB_VISIBLE_LINES) {
                if (rbScrollPausing) {
                    if (now - rbScrollLastStep >= RB_SCROLL_PAUSE_MS) {
                        rbScrollPausing  = false;
                        rbScrollLastStep = now;
                    }
                } else if (now - rbScrollLastStep >= RB_SCROLL_STEP_MS) {
                    rbScrollLastStep = now;
                    int maxOffset = rbTotalLines - RB_VISIBLE_LINES;
                    if (rbScrollOffset < maxOffset) {
                        rbScrollOffset++;
                    } else {
                        // Reached bottom — pause then restart from top
                        rbScrollOffset  = 0;
                        rbScrollPausing = true;
                    }
                }
            }
            if (rbDoNextTopic) {
                rbEnabledPos = (rbEnabledPos + 1) % rbEnabledCount;
                rbTopicIdx   = rbEnabled[rbEnabledPos];
                rbDepth    = 0;
                rbDeepCount= 0;
                rbHnStoryId= 0;
                rbPhase    = RB_LOADING; rbPhaseAt = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                snprintf(rbStatus, sizeof(rbStatus),
                         "LOADING %s", TOPICS[rbTopicIdx].badge);
            } else if (rbDoDeeper) {
                if (rbDeepCount > 0) {
                    rbDepth++;
                    rbPhase      = RB_LOADING;
                    rbPhaseAt    = now;
                    rbNeedsFetch = false;  // will be set on next frame after draw
                    rbFetchDeeper = true;
                    snprintf(rbStatus, sizeof(rbStatus), "GOING DEEPER...");
                }
                // else: no deep links available, ignore
            } else if (rbDoNew) {
                rbDepth    = 0;
                rbDeepCount= 0;
                rbHnStoryId= 0;
                rbPhase      = RB_LOADING; rbPhaseAt = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                snprintf(rbStatus, sizeof(rbStatus),
                         "LOADING %s", TOPICS[rbTopicIdx].badge);
            }
            break;

        case RB_ERROR:
            if (rbDoNextTopic) {
                rbEnabledPos = (rbEnabledPos + 1) % rbEnabledCount;
                rbTopicIdx   = rbEnabled[rbEnabledPos];
                rbDepth    = 0;
                rbDeepCount= 0;
                rbHnStoryId= 0;
                rbPhase      = WiFi.status() == WL_CONNECTED ? RB_LOADING : RB_WIFI;
                rbPhaseAt    = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                snprintf(rbStatus, sizeof(rbStatus),
                         "LOADING %s", TOPICS[rbTopicIdx].badge);
            } else if (rbDoNew) {
                rbDepth = 0; rbDeepCount = 0;
                rbPhase      = WiFi.status() == WL_CONNECTED ? RB_LOADING : RB_WIFI;
                rbPhaseAt    = now;
                rbNeedsFetch = false; rbFetchDeeper = false;
                snprintf(rbStatus, sizeof(rbStatus),
                         "LOADING %s", TOPICS[rbTopicIdx].badge);
            }
            break;
    }
}

void rabbitDraw() {
    if (!fbReady) return;
    switch (rbPhase) {
        case RB_WIFI:    rbDrawLoading(); break;
        case RB_LOADING: rbDrawLoading(); break;
        case RB_DISPLAY: rbDrawDisplay(); break;
        case RB_ERROR:   rbDrawError();   break;
    }
}
