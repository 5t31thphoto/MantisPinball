// extras_mode.cpp - NASA APOD, Flight Tracker, Crypto Candles.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <math.h>
#include <TJpg_Decoder.h>
#include "extras_mode.h"
#include "../chord_guard.h"
#include "../theme_colors.h"
#include "../net_yield.h"
#include "../shared_buffers.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// Shared scratch buffers (these modes never run simultaneously)
static char  exBuf[12288];
#define exJpeg g_jpegBuf   // shared heap image buffer (see shared_buffers.h)

// ---- WiFi credentials (read-only from the miner namespace) -------------------
static bool exWifiUp = false;
static String exSsid, exPass;

static bool exLoadWifi() {
    Preferences p;
    if (!p.begin("btcminer", true)) return false;
    exSsid = p.getString("ssid", "");
    exPass = p.getString("pass", "");
    p.end();
    return exSsid.length() > 0;
}

static bool exWifiConnect() {
    if (WiFi.status() == WL_CONNECTED) { exWifiUp = true; return true; }
    if (!exLoadWifi()) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(exSsid.c_str(), exPass.c_str());
    uint32_t dl = millis() + 12000;
    while (WiFi.status() != WL_CONNECTED && millis() < dl) { sysNetYield(); if (sysNetAborted()) break; delay(10); }
    exWifiUp = (WiFi.status() == WL_CONNECTED);
    return exWifiUp;
}

// ---- Generic HTTPS GET (no-heap header skip) ---------------------------------
static bool exGetS(const char* host, const char* path, char* out, size_t outLen) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(14);
    if (!c.connect(host, 443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\n"
             "User-Agent: Mozilla/5.0 (pinball-s3)\r\n"
             "Accept: application/json,*/*\r\nConnection: close\r\n\r\n",
             path, host);
    uint32_t dl = millis() + 16000;
    char hdr[160]; int hl = 0; bool ok = false; bool inHdr = true;
    while (inHdr && (c.connected() || c.available()) && millis() < dl) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            hdr[hl] = '\0';
            if (hl == 0) { inHdr = false; break; }
            if (strncmp(hdr, "HTTP/", 5) == 0) ok = (strstr(hdr, " 200") != nullptr);
            hl = 0;
        } else if (hl < 159) hdr[hl++] = (char)ch;
    }
    if (!ok) { c.stop(); return false; }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl && pos < outLen - 1) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        out[pos++] = (char)ch;
    }
    out[pos] = '\0'; c.stop();
    return pos > 10;
}

// Tiny JSON string extractor (first "key":"value")
static bool exJStr(const char* s, const char* key, char* out, int maxLen) {
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(pat);
    while (*p && *p != ':') p++; if (!*p) { out[0]='\0'; return false; } p++;
    while (*p == ' ') p++;
    if (*p != '"') { out[0] = '\0'; return false; }
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxLen - 1) {
        if (*p == '\\' && *(p+1)) {
            char n = *(p+1);
            if (n == 'n') { out[i++] = ' '; p += 2; continue; }
            if (n == '"' || n == '\\' || n == '/') { out[i++] = n; p += 2; continue; }
            if (n == 'u') { p += 6; continue; }   // skip unicode escapes
            p += 2; continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

// Word-wrap a string into fb lines, return number of lines drawn
static int exDrawWrapped(const char* text, int x, int y, int maxChars,
                         int lineH, int maxY, uint16_t color) {
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(color);
    int i = 0, len = strlen(text), cy = y, lines = 0;
    while (i < len && cy < maxY) {
        int start = i, lastSp = -1, col = 0;
        while (i < len && text[i] != '\n' && col < maxChars) {
            if (text[i] == ' ') lastSp = i;
            i++; col++;
        }
        int end = i;
        if (i < len && text[i] != '\n' && lastSp > start) { end = lastSp; i = lastSp + 1; }
        else if (i < len && text[i] == '\n') { /* keep */ }
        char line[40];
        int n = end - start; if (n > 39) n = 39;
        memcpy(line, text + start, n); line[n] = '\0';
        fb.drawString(line, x, cy, 1);
        cy += lineH; lines++;
        if (i < len && text[i] == '\n') i++;
    }
    return lines;
}

// =============================================================================
//  JPEG decode shared by APOD (reuses the rabbit-mode approach)
// =============================================================================
static int exJpegScale = 1, exJpegOX = 0, exJpegOY = 0;
static size_t exJpegLen = 0;        // length of bytes currently in exJpeg[]
static int    exJpegDW = 0, exJpegDH = 0;  // decoded (scaled) dimensions
static bool exJpegOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
    int py = exJpegOY + y;
    if (py >= H - 60) return false;
    fb.pushImage(exJpegOX + x, py, w, h, bmp);
    return true;
}

// Download a JPEG into exJpeg[] and compute the scale/offset to fit. Does NOT
// draw - call exDrawJpeg() each frame to render from the cached bytes.
static bool exFetchJpeg(const char* url, int topY, int maxH) {
    exJpegLen = 0;
    if (!exJpeg) return false;   // shared buffer not allocated
    const char* h = url;
    if (strncmp(h, "https://", 8) == 0) h += 8;
    else if (strncmp(h, "http://", 7) == 0) h += 7;
    char host[96] = ""; int hi = 0;
    while (*h && *h != '/' && hi < 95) host[hi++] = *h++;
    host[hi] = '\0';
    const char* path = (*h) ? h : "/";

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(14);
    if (!c.connect(host, 443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-s3\r\n"
             "Accept: image/jpeg\r\nConnection: close\r\n\r\n", path, host);
    uint32_t dl = millis() + 16000;
    char hdr[160]; int hl = 0; bool ok = false; bool inHdr = true;
    while (inHdr && (c.connected() || c.available()) && millis() < dl) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            hdr[hl] = '\0';
            if (hl == 0) { inHdr = false; break; }
            if (strncmp(hdr, "HTTP/", 5) == 0) ok = (strstr(hdr, " 200") != nullptr);
            hl = 0;
        } else if (hl < 159) hdr[hl++] = (char)ch;
    }
    if (!ok) { c.stop(); return false; }
    size_t len = 0;
    while ((c.connected() || c.available()) && millis() < dl && len < g_jpegBufLen) {
        int ch = c.read(); if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        exJpeg[len++] = (uint8_t)ch;
    }
    c.stop();
    // Truncated (image bigger than buffer) -> treat as failure
    if (len < 100 || len >= g_jpegBufLen) return false;
    exJpegLen = len;

    uint16_t jw = 0, jh = 0;
    TJpgDec.setJpgScale(1);
    if (TJpgDec.getJpgSize(&jw, &jh, exJpeg, exJpegLen) != JDR_OK || jw == 0) {
        exJpegLen = 0; return false;
    }
    int scale = 1;
    while ((jw / scale > W || jh / scale > maxH) && scale < 8) scale *= 2;
    exJpegScale = scale;
    exJpegDW = jw / scale; exJpegDH = jh / scale;
    exJpegOX = (W - exJpegDW) / 2; if (exJpegOX < 0) exJpegOX = 0;
    exJpegOY = topY;
    return true;
}

// Render the cached JPEG into the fb sprite (call every frame).
static void exDrawJpeg() {
    if (exJpegLen == 0) return;
    TJpgDec.setJpgScale(exJpegScale);
    TJpgDec.setCallback(exJpegOut);
    TJpgDec.drawJpg(exJpegOX, exJpegOY, exJpeg, exJpegLen);
}

// =============================================================================
//  NASA APOD
// =============================================================================
static int  apodPhase = 0;   // 0 connecting, 1 fetching, 2 display, 3 error
static char apodTitle[80];
static char apodExpl[2048];
static char apodUrl[200];
static char apodMedia[16];
static bool apodImgOk = false;
static int  apodScroll = 0;
static bool apodNeedFetch = false;

void apodEnter() {
    apodPhase = 0; apodTitle[0] = apodExpl[0] = apodUrl[0] = 0;
    apodMedia[0] = 0; apodImgOk = false; apodScroll = 0;
    apodNeedFetch = true;
    jpegBufAcquire();   // shared image buffer for APOD images
    fb.setRotation(0);
}

void apodShutdown() {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); exWifiUp = false;
    jpegBufRelease();
}

static void apodDoFetch() {
    if (!exWifiConnect()) { apodPhase = 3; return; }
    apodPhase = 1;
    // DEMO_KEY is fine for a once-a-day image
    if (!exGetS("api.nasa.gov", "/planetary/apod?api_key=DEMO_KEY",
                exBuf, sizeof(exBuf))) {
        apodPhase = 3; return;
    }
    exJStr(exBuf, "title", apodTitle, sizeof(apodTitle));
    exJStr(exBuf, "explanation", apodExpl, sizeof(apodExpl));
    exJStr(exBuf, "media_type", apodMedia, sizeof(apodMedia));
    exJStr(exBuf, "url", apodUrl, sizeof(apodUrl));
    if (!apodTitle[0]) { apodPhase = 3; return; }
    // APOD images at the standard url are 1000-2000px (100-400KB) JPEGs which
    // exceed our decode buffer. We attempt the download but most won't fit;
    // when they don't, the explanation text (the real substance) is shown.
    apodImgOk = false;
    if (strcmp(apodMedia, "image") == 0 && apodUrl[0]) {
        apodImgOk = exFetchJpeg(apodUrl, 74, 150);
    }
    apodPhase = 2;
}

void apodTick() {
    if (apodNeedFetch) { apodNeedFetch = false; apodDoFetch(); return; }
    if (apodPhase != 2) {
        if ((btnR && !btnR_prev) && apodPhase == 3) { apodNeedFetch = true; }
        return;
    }
    // R scrolls the explanation, L scrolls back up
    if (btnR && !btnR_prev) apodScroll += 4;
    if (btnL && !btnL_prev) apodScroll = (apodScroll >= 4) ? apodScroll - 4 : 0;
}

void apodDraw() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("NASA APOD", 4, 8, 1);

    if (apodPhase == 0 || apodPhase == 1) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString(apodPhase == 0 ? "connecting..." : "fetching...",
                      W / 2, H / 2, 2);
        fb.pushSprite(0, 0); return;
    }
    if (apodPhase == 3) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("fetch failed", W / 2, H / 2 - 10, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("R: retry", W / 2, H / 2 + 14, 1);
        fb.pushSprite(0, 0); return;
    }

    // Display: title, then either the image (if it fits) or the explanation
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME);
    int titleLines = exDrawWrapped(apodTitle, 4, 20, 27, 12, 70, LIME);
    int ty = titleLines * 12 + 20;
    int bodyTop = ty + 4;

    if (apodImgOk && exJpegLen > 0 && exJpeg) {
        // Redraw the cached image (sprite was cleared this frame)
        exDrawJpeg();
        // Explanation scrolls below the image
        int capY = exJpegOY + exJpegDH + 6;
        if (capY < H - 20) {
            exDrawWrapped(apodExpl + apodScroll, 4, capY, 27, 11, H - 16,
                          TEAL_LIGHT);
        }
    } else {
        // Text-only (video APOD, or image too big for the buffer): the
        // explanation is genuinely interesting reading on its own.
        exDrawWrapped(apodExpl + apodScroll, 4, bodyTop, 27, 12, H - 16,
                      TEAL_LIGHT);
    }
    // Footer
    fb.fillRect(0, H - 12, W, 12, PURPLE_DARK);
    fb.setTextColor(LIME); fb.setTextDatum(ML_DATUM);
    fb.drawString("L/R scroll text", 3, H - 6, 1);
    fb.pushSprite(0, 0);
}

// =============================================================================
//  FLIGHT TRACKER  (OpenSky, anonymous)
// =============================================================================
struct Plane {
    char callsign[10];
    float lat, lon, altM, velMs, headDeg;
    char country[24];
};
static int    flPhase = 0;   // 0 conn, 1 fetch, 2 display, 3 error
static Plane  flList[12];
static int    flCount = 0;
static int    flSel = 0;
static float  flUserLat = 0, flUserLon = 0;
static bool   flHasCoords = false;
static bool   flNeedFetch = false;
static uint32_t flLastFetch = 0;

static void flLoadCoords() {
    Preferences p;
    if (p.begin("pinball", true)) {
        flUserLat = p.getFloat("wx_lat", 0.0f);
        flUserLon = p.getFloat("wx_lon", 0.0f);
        p.end();
    }
    flHasCoords = (flUserLat != 0.0f || flUserLon != 0.0f);
}

void flightEnter() {
    chordGuardReset();
    flPhase = 0; flCount = 0; flSel = 0; flNeedFetch = true;
    flLoadCoords();
    fb.setRotation(0);
}
void flightShutdown() {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); exWifiUp = false;
}

static float flDist(float la1, float lo1, float la2, float lo2) {
    float dLat = (la2 - la1) * 0.01745329f;
    float dLon = (lo2 - lo1) * 0.01745329f;
    float x = dLon * cosf((la1 + la2) * 0.5f * 0.01745329f);
    return sqrtf(x*x + dLat*dLat) * 6371.0f;
}

// Parse the OpenSky states array. Each state is itself an array:
// [icao24, callsign, country, t_pos, t_contact, lon, lat, baroAlt, onGround,
//  vel, heading, vRate, ...]
static void flParse() {
    flCount = 0;
    const char* p = strstr(exBuf, "\"states\"");
    if (!p) return;
    p = strchr(p, '[');     // outer array opener
    if (!p) return;
    p++;                    // step into the array of state arrays

    while (flCount < 12) {
        // Find the next state sub-array
        const char* arr = strchr(p, '[');
        if (!arr) break;
        const char* end = strchr(arr, ']');
        if (!end) break;

        Plane& pl = flList[flCount];
        memset(&pl, 0, sizeof(pl));

        // Tokenize fields between arr+1 and end by commas, respecting quotes.
        const char* f = arr + 1;
        int idx = 0;
        while (f < end && idx < 17) {
            // Skip leading whitespace
            while (f < end && (*f == ' ' || *f == ',')) f++;
            if (f >= end) break;

            // Capture this field's raw span [fieldStart, fieldEnd)
            const char* fieldStart = f;
            const char* fieldEnd;
            if (*f == '"') {
                // string field
                const char* q = f + 1;
                while (q < end && *q != '"') q++;
                // extract value (without quotes) for the fields we need
                if (idx == 1) {           // callsign
                    int i = 0; const char* s = f + 1;
                    while (s < q && i < 9) pl.callsign[i++] = *s++;
                    pl.callsign[i] = '\0';
                } else if (idx == 2) {    // origin country
                    int i = 0; const char* s = f + 1;
                    while (s < q && i < 23) pl.country[i++] = *s++;
                    pl.country[i] = '\0';
                }
                fieldEnd = (q < end) ? q + 1 : end;   // past closing quote
            } else {
                // number, bool, or null — read up to next comma
                fieldEnd = f;
                while (fieldEnd < end && *fieldEnd != ',') fieldEnd++;
                bool isNull = (strncmp(fieldStart, "null", 4) == 0);
                if (!isNull) {
                    float v = strtof(fieldStart, nullptr);
                    if (idx == 5) pl.lon = v;
                    else if (idx == 6) pl.lat = v;
                    else if (idx == 7) pl.altM = v;       // baro altitude (m)
                    else if (idx == 9) pl.velMs = v;      // velocity (m/s)
                    else if (idx == 10) pl.headDeg = v;   // true track (deg)
                    else if (idx == 13 && pl.altM == 0) pl.altM = v; // geo alt fallback
                }
            }
            // Always advance f past this field to the next comma (no hang)
            f = fieldEnd;
            while (f < end && *f != ',') f++;
            idx++;
        }

        // Trim trailing spaces from callsign
        for (int i = (int)strlen(pl.callsign) - 1; i >= 0 && pl.callsign[i] == ' '; i--)
            pl.callsign[i] = '\0';
        if (pl.callsign[0] == '\0') strcpy(pl.callsign, "(no id)");

        // Only keep planes with a plausible position (skip null-island junk)
        if (pl.lat != 0.0f || pl.lon != 0.0f) flCount++;

        p = end + 1;
        // Safety: if the outer pointer didn't advance, bail to avoid any hang
        if (p <= arr) break;
    }
}

static void flDoFetch() {
    if (!exWifiConnect()) { flPhase = 3; return; }
    flPhase = 1;
    char path[160];
    if (flHasCoords) {
        float d = 0.6f;  // ~60km box
        snprintf(path, sizeof(path),
            "/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
            flUserLat - d, flUserLon - d, flUserLat + d, flUserLon + d);
    } else {
        snprintf(path, sizeof(path), "/api/states/all?lamin=39&lomin=-112&lamax=42&lomax=-109");
    }
    if (!exGetS("opensky-network.org", path, exBuf, sizeof(exBuf))) {
        flPhase = 3; return;
    }
    flParse();
    flPhase = 2;
    flLastFetch = millis();
    if (flSel >= flCount) flSel = 0;
}

void flightTick() {
    chordGuardTick();
    if (flNeedFetch) { flNeedFetch = false; flDoFetch(); return; }
    if (flPhase == 3 && chordREdge()) { flNeedFetch = true; return; }
    if (flPhase != 2) return;
    // R cycles through planes, L forces a refresh
    if (chordREdge() && flCount > 0) flSel = (flSel + 1) % flCount;
    if (chordLEdge()) flNeedFetch = true;
    // Auto-refresh every 20s
    if (millis() - flLastFetch > 20000) flNeedFetch = true;
}

void flightDraw() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("FLIGHTS OVERHEAD", 4, 8, 1);

    if (flPhase == 0 || flPhase == 1) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString(flPhase == 0 ? "connecting..." : "scanning sky...",
                      W / 2, H / 2, 2);
        fb.pushSprite(0, 0); return;
    }
    if (flPhase == 3) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("fetch failed", W / 2, H / 2 - 10, 2);
        fb.setTextColor(TEAL_DARK); fb.drawString("R: retry", W / 2, H / 2 + 14, 1);
        fb.pushSprite(0, 0); return;
    }
    if (flCount == 0) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("no planes", W / 2, H / 2 - 16, 2);
        fb.drawString("overhead now", W / 2, H / 2 + 4, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("L: refresh", W / 2, H - 20, 1);
        fb.pushSprite(0, 0); return;
    }

    Plane& pl = flList[flSel];
    // Big callsign
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
    fb.drawString(pl.callsign, W / 2, 24, 4);

    // Detail rows
    int y = 64;
    auto row = [&](const char* label, const char* val, uint16_t col) {
        fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString(label, 8, y, 1);
        fb.setTextDatum(TR_DATUM); fb.setTextColor(col);
        fb.drawString(val, W - 8, y, 2);
        y += 26;
    };
    char b[32];
    snprintf(b, sizeof(b), "%.0f ft", pl.altM * 3.281f);
    row("ALTITUDE", b, TEAL_BRIGHT);
    snprintf(b, sizeof(b), "%.0f mph", pl.velMs * 2.237f);
    row("SPEED", b, TEAL_BRIGHT);
    snprintf(b, sizeof(b), "%.0f deg", pl.headDeg);
    row("HEADING", b, TEAL_BRIGHT);
    if (flHasCoords) {
        float dkm = flDist(flUserLat, flUserLon, pl.lat, pl.lon);
        snprintf(b, sizeof(b), "%.1f mi", dkm * 0.621371f);
        row("DISTANCE", b, LIME);
    }
    // Country (smaller)
    // Registration country (where the aircraft is registered, not its location)
    char regLine[40]; snprintf(regLine, sizeof(regLine), "reg: %s", pl.country);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(regLine, W / 2, y + 4, 1);

    // Footer: which plane + controls
    fb.fillRect(0, H - 26, W, 26, PURPLE_DARK);
    char cnt[16]; snprintf(cnt, sizeof(cnt), "%d / %d", flSel + 1, flCount);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString(cnt, W / 2, H - 18, 1);
    fb.setTextDatum(ML_DATUM); fb.drawString("L: refresh", 3, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: next", W - 3, H - 7, 1);
    fb.pushSprite(0, 0);
}

// =============================================================================
//  CRYPTO CANDLES  (Heikin-Ashi + MACD from CoinGecko OHLC)
// =============================================================================
static int    caPhase = 0;
static float  caO[60], caH[60], caL[60], caC[60];   // raw OHLC
static int    caN = 0;
static float  haO[60], haH[60], haL[60], haC[60];   // Heikin-Ashi
static float  caMacd[60], caSignal[60], caHist[60];
static bool   caNeedFetch = false;
static uint32_t caLastFetch = 0;
static int    caView = 0;   // 0 = candles, 1 = MACD focus

void candlesEnter() {
    chordGuardReset();
    caPhase = 0; caN = 0; caView = 0; caNeedFetch = true;
    fb.setRotation(0);
}
void candlesShutdown() {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); exWifiUp = false;
}

// Parse CoinGecko OHLC: bare array of [ts, o, h, l, c] arrays
static void caParse() {
    caN = 0;
    const char* p = exBuf;
    while (*p && *p != '[') p++;
    if (*p) p++;   // into outer array
    while (caN < 60) {
        const char* arr = strchr(p, '[');
        if (!arr) break;
        const char* end = strchr(arr, ']');
        if (!end) break;
        // fields: ts, open, high, low, close
        const char* f = arr + 1;
        float v[5] = {0,0,0,0,0};
        int idx = 0;
        while (f < end && idx < 5) {
            while (*f == ' ' || *f == ',') f++;
            if (f >= end) break;
            v[idx++] = strtof(f, nullptr);
            while (f < end && *f != ',') f++;
        }
        if (idx >= 5) {
            caO[caN] = v[1]; caH[caN] = v[2]; caL[caN] = v[3]; caC[caN] = v[4];
            caN++;
        }
        p = end + 1;
    }
    // Compute Heikin-Ashi
    for (int i = 0; i < caN; i++) {
        haC[i] = (caO[i] + caH[i] + caL[i] + caC[i]) / 4.0f;
        if (i == 0) haO[i] = (caO[i] + caC[i]) / 2.0f;
        else        haO[i] = (haO[i-1] + haC[i-1]) / 2.0f;
        haH[i] = fmaxf(caH[i], fmaxf(haO[i], haC[i]));
        haL[i] = fminf(caL[i], fminf(haO[i], haC[i]));
    }
    // Compute MACD (12/26 EMA, 9 signal) on close
    if (caN > 0) {
        float ema12 = caC[0], ema26 = caC[0];
        const float k12 = 2.0f / 13.0f, k26 = 2.0f / 27.0f, k9 = 2.0f / 10.0f;
        float sig = 0; bool sigInit = false;
        for (int i = 0; i < caN; i++) {
            ema12 = caC[i] * k12 + ema12 * (1 - k12);
            ema26 = caC[i] * k26 + ema26 * (1 - k26);
            caMacd[i] = ema12 - ema26;
            if (!sigInit) { sig = caMacd[i]; sigInit = true; }
            else sig = caMacd[i] * k9 + sig * (1 - k9);
            caSignal[i] = sig;
            caHist[i] = caMacd[i] - caSignal[i];
        }
    }
}

static void caDoFetch() {
    if (!exWifiConnect()) { caPhase = 3; return; }
    caPhase = 1;
    if (!exGetS("api.coingecko.com",
                "/api/v3/coins/bitcoin/ohlc?vs_currency=usd&days=1",
                exBuf, sizeof(exBuf))) {
        caPhase = 3; return;
    }
    caParse();
    caPhase = (caN > 0) ? 2 : 3;
    caLastFetch = millis();
}

void candlesTick() {
    chordGuardTick();
    if (caNeedFetch) { caNeedFetch = false; caDoFetch(); return; }
    if (caPhase == 3 && chordREdge()) { caNeedFetch = true; return; }
    if (caPhase != 2) return;
    if (chordREdge()) caView = 1 - caView;       // toggle candle/MACD focus
    if (chordLEdge()) caNeedFetch = true;        // refresh
    if (millis() - caLastFetch > 120000) caNeedFetch = true;  // 2min auto
}

void candlesDraw() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("BTC / USD  24h", 4, 8, 1);

    if (caPhase == 0 || caPhase == 1) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString(caPhase == 0 ? "connecting..." : "loading...", W / 2, H / 2, 2);
        fb.pushSprite(0, 0); return;
    }
    if (caPhase == 3) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("fetch failed", W / 2, H / 2 - 10, 2);
        fb.setTextColor(TEAL_DARK); fb.drawString("R: retry", W / 2, H / 2 + 14, 1);
        fb.pushSprite(0, 0); return;
    }

    // Current price + change
    float last = caC[caN - 1], first = caC[0];
    float chg = (last - first) / first * 100.0f;
    char pb[24]; snprintf(pb, sizeof(pb), "$%.0f", last);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME);
    fb.drawString(pb, 4, 20, 4);
    char cb[16]; snprintf(cb, sizeof(cb), "%+.2f%%", chg);
    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(chg >= 0 ? TFT_GREEN : rgb565(255, 80, 80));
    fb.drawString(cb, W - 4, 24, 2);

    // --- Heikin-Ashi candle chart ---
    int chartTop = 48, chartH = caView == 0 ? 170 : 90, chartBot = chartTop + chartH;
    float hi = haH[0], lo = haL[0];
    for (int i = 0; i < caN; i++) { hi = fmaxf(hi, haH[i]); lo = fminf(lo, haL[i]); }
    float range = (hi - lo); if (range < 1) range = 1;
    int n = caN; if (n > 56) n = 56;
    int startI = caN - n;
    float cw = (float)(W - 8) / n;
    auto yOf = [&](float price) {
        return chartBot - (int)((price - lo) / range * chartH);
    };
    for (int k = 0; k < n; k++) {
        int i = startI + k;
        int cx = 4 + (int)(k * cw + cw / 2);
        bool up = haC[i] >= haO[i];
        uint16_t col = up ? TFT_GREEN : rgb565(255, 80, 80);
        // wick
        fb.drawFastVLine(cx, yOf(haH[i]), yOf(haL[i]) - yOf(haH[i]) + 1, col);
        // body
        int bodyTop = yOf(fmaxf(haO[i], haC[i]));
        int bodyBot = yOf(fminf(haO[i], haC[i]));
        int bw = (int)(cw * 0.7f); if (bw < 1) bw = 1;
        int bh = bodyBot - bodyTop; if (bh < 1) bh = 1;
        fb.fillRect(cx - bw / 2, bodyTop, bw, bh, col);
    }

    // --- MACD panel below ---
    int mTop = chartBot + 6, mH = H - 16 - mTop, mMid = mTop + mH / 2;
    fb.drawFastHLine(0, mTop - 3, W, TEAL_DARK);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("MACD 12/26/9", 4, mTop - 2, 1);
    float mmax = 0;
    for (int i = startI; i < caN; i++) {
        mmax = fmaxf(mmax, fabsf(caMacd[i]));
        mmax = fmaxf(mmax, fabsf(caSignal[i]));
        mmax = fmaxf(mmax, fabsf(caHist[i]));
    }
    if (mmax < 1) mmax = 1;
    int mDrawTop = mTop + 8;
    int mDrawH = (H - 14) - mDrawTop;
    int mZero = mDrawTop + mDrawH / 2;
    auto mY = [&](float v) { return mZero - (int)(v / mmax * (mDrawH / 2)); };
    // histogram
    for (int k = 0; k < n; k++) {
        int i = startI + k;
        int cx = 4 + (int)(k * cw + cw / 2);
        int hy = mY(caHist[i]);
        uint16_t hc = caHist[i] >= 0 ? rgb565(0, 180, 90) : rgb565(180, 60, 60);
        if (hy <= mZero) fb.fillRect(cx - 1, hy, 2, mZero - hy + 1, hc);
        else             fb.fillRect(cx - 1, mZero, 2, hy - mZero + 1, hc);
    }
    // MACD + signal lines
    for (int k = 1; k < n; k++) {
        int i = startI + k, ip = startI + k - 1;
        int x0 = 4 + (int)((k-1) * cw + cw / 2), x1 = 4 + (int)(k * cw + cw / 2);
        fb.drawLine(x0, mY(caMacd[ip]), x1, mY(caMacd[i]), LIME);
        fb.drawLine(x0, mY(caSignal[ip]), x1, mY(caSignal[i]), PURPLE_LIGHT);
    }
    fb.drawFastHLine(4, mZero, W - 8, TEAL_DARK);

    // Footer
    fb.fillRect(0, H - 12, W, 12, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME);
    fb.drawString("L: refresh", 3, H - 6, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: zoom", W - 3, H - 6, 1);
    fb.pushSprite(0, 0);
}
