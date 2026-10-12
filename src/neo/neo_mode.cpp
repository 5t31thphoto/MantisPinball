// neo_mode.cpp - Near-Earth Object close-approach dashboard (NASA NeoWs).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "neo_mode.h"
#include "../chord_guard.h"
#include "../net_yield.h"
#include "../sysclock/sysclock.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

static const char* NVS_MINER = "btcminer";   // WiFi creds (read-only)
static const char* NEO_HOST  = "api.nasa.gov";

// Fetch state
enum NeoState { NS_IDLE, NS_FETCHING, NS_OK, NS_ERR };
static NeoState neoState = NS_IDLE;
static char neoErr[40] = "";

// Parsed result (closest approach today)
static char  neoName[28]   = "";
static double neoMissLD    = 0;     // lunar distances
static double neoMissKM    = 0;
static double neoSpeedKmh  = 0;
static double neoDiamMinM  = 0;
static double neoDiamMaxM  = 0;
static bool  neoHazard     = false;
static int   neoCount      = 0;     // total approaches scanned today

static char neoSsid[40] = "", neoPass[64] = "";

static void neoLoadCreds() {
    Preferences p;
    if (p.begin(NVS_MINER, true)) {
        p.getString("ssid", neoSsid, sizeof(neoSsid));
        p.getString("pass", neoPass, sizeof(neoPass));
        p.end();
    }
}

// A big heap buffer for the response, allocated only during a fetch.
static char* neoBuf = nullptr;
static const size_t NEO_BUF = 24000;   // NeoWs 1-day feed is a few KB to ~20KB

// Minimal HTTPS GET into neoBuf (proven weather pattern).
static bool neoHttpGet(const char* path) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12);
    if (!c.connect(NEO_HOST, 443)) { strncpy(neoErr, "connect failed", sizeof(neoErr)); return false; }
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-neo/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n", path, NEO_HOST);
    uint32_t dl = millis() + 15000;
    // skip headers
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            strncpy(neoErr, "http error", sizeof(neoErr)); c.stop(); return false;
        }
    }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (pos < NEO_BUF - 1) neoBuf[pos++] = (char)ch;
    }
    neoBuf[pos] = 0;
    c.stop();
    return pos > 0;
}

// --- tiny JSON scanners (no library) ---
// Find the numeric value following a "key": in [from..). Returns it or NAN.
static double findNum(const char* s, const char* key, int from) {
    const char* p = strstr(s + from, key);
    if (!p) return NAN;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    return atof(p);
}

// Parse the NeoWs feed: walk each "close_approach_data" record, track the one
// with the smallest miss distance (lunar). Each asteroid object has name,
// estimated_diameter, is_potentially_hazardous_asteroid, and close_approach_data
// with miss_distance.lunar/kilometers and relative_velocity.kilometers_per_hour.
static bool neoParse() {
    if (!neoBuf) return false;
    neoCount = 0;
    double bestLD = 1e18;
    bool found = false;

    // Iterate over each asteroid block by scanning for the name field NeoWs uses.
    const char* NAMEKEY = "\"name\":";
    int idx = 0;
    int guard = 0;
    while (guard++ < 4000) {
        const char* np = strstr(neoBuf + idx, NAMEKEY);
        if (!np) break;
        int nameAt = (int)(np - neoBuf);
        int next = nameAt + (int)strlen(NAMEKEY);

        // This name block: read the asteroid name string
        const char* q = neoBuf + next;
        while (*q && *q != '"') q++;            // opening quote
        if (*q != '"') { idx = next; continue; }
        q++;
        char nm[28]; int k = 0;
        while (*q && *q != '"' && k < (int)sizeof(nm) - 1) nm[k++] = *q++;
        nm[k] = 0;

        // Only treat as an asteroid record if a miss_distance follows before the
        // next name. Find the nearest "lunar": after this name.
        const char* nextName = strstr(neoBuf + next, NAMEKEY);
        int searchEnd = nextName ? (int)(nextName - neoBuf) : (int)strlen(neoBuf);

        double ld = findNum(neoBuf, "\"lunar\":", next);
        // Make sure the lunar we found is within this record, not a later one.
        const char* lp = strstr(neoBuf + next, "\"lunar\":");
        if (!isnan(ld) && lp && (int)(lp - neoBuf) < searchEnd) {
            neoCount++;
            if (ld < bestLD) {
                bestLD = ld;
                found = true;
                strncpy(neoName, nm, sizeof(neoName) - 1); neoName[sizeof(neoName)-1] = 0;
                neoMissLD = ld;
                neoMissKM = findNum(neoBuf, "\"kilometers\":", next);
                neoSpeedKmh = findNum(neoBuf, "\"kilometers_per_hour\":", next);
                neoDiamMinM = findNum(neoBuf, "\"estimated_diameter_min\":", next);
                neoDiamMaxM = findNum(neoBuf, "\"estimated_diameter_max\":", next);
                const char* hp = strstr(neoBuf + nameAt, "\"is_potentially_hazardous_asteroid\":");
                neoHazard = false;
                if (hp && (int)(hp - neoBuf) < searchEnd) {
                    hp += strlen("\"is_potentially_hazardous_asteroid\":");
                    while (*hp == ' ') hp++;
                    neoHazard = (strncmp(hp, "true", 4) == 0);
                }
            }
        }
        idx = next;
    }
    return found;
}

static void neoDateStr(char* out, size_t n, int offsetDays) {
    time_t t = sysclockNow();
    if (t < 1700000000) t = 1704067200; // fallback 2024-01-01 if no clock
    t += offsetDays * 86400;
    struct tm tmv; gmtime_r(&t, &tmv);
    snprintf(out, n, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static void neoFetch() {
    neoState = NS_FETCHING;
    neoErr[0] = 0;
    if (neoSsid[0] == 0) { strncpy(neoErr, "no WiFi set (use Weather)", sizeof(neoErr)); neoState = NS_ERR; return; }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(neoSsid, neoPass);
        uint32_t dl = millis() + 15000;
        while (WiFi.status() != WL_CONNECTED && millis() < dl) { sysNetYield(); if (sysNetAborted()) break; delay(10); }
        if (WiFi.status() != WL_CONNECTED) { strncpy(neoErr, "WiFi connect failed", sizeof(neoErr)); neoState = NS_ERR; return; }
    }

    neoBuf = (char*)malloc(NEO_BUF);
    if (!neoBuf) { strncpy(neoErr, "out of memory", sizeof(neoErr)); neoState = NS_ERR; return; }

    char today[12]; neoDateStr(today, sizeof(today), 0);
    char path[160];
    snprintf(path, sizeof(path),
             "/neo/rest/v1/feed?start_date=%s&end_date=%s&api_key=DEMO_KEY",
             today, today);

    bool ok = neoHttpGet(path);
    if (ok) ok = neoParse();
    free(neoBuf); neoBuf = nullptr;

    if (ok) neoState = NS_OK;
    else { if (!neoErr[0]) strncpy(neoErr, "no data / parse fail", sizeof(neoErr)); neoState = NS_ERR; }
}

void neoEnter() {
    chordGuardReset();
    fb.setRotation(0);
    neoLoadCreds();
    neoState = NS_IDLE;
}
void neoShutdown() {
    if (neoBuf) { free(neoBuf); neoBuf = nullptr; }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void neoTick() {
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();
    if (neoState == NS_IDLE && (lEdge || rEdge)) { neoFetch(); return; }
    if ((neoState == NS_OK || neoState == NS_ERR) && rEdge) { neoFetch(); return; }
    if ((neoState == NS_OK || neoState == NS_ERR) && lEdge) { neoState = NS_IDLE; }
}

void neoDraw() {
    fb.fillSprite(TEAL_DEEP);

    // hazard red border
    if (neoState == NS_OK && neoHazard) {
        fb.drawRect(0, 0, W, H, PURPLE_LIGHT);
        fb.drawRect(1, 1, W - 2, H - 2, PURPLE_LIGHT);
    }

    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("ASTEROIDS", 4, 9, 1);

    if (neoState == NS_IDLE) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString("Near-Earth Objects", W/2, 80, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("Closest approach today", W/2, 110, 1);
        fb.setTextColor(LIME);
        fb.drawString("press to load", W/2, 150, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("uses NASA NeoWs", W/2, 200, 1);
        fb.drawString("(WiFi from Weather setup)", W/2, 216, 1);
    } else if (neoState == NS_FETCHING) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString("fetching...", W/2, H/2, 2);
    } else if (neoState == NS_ERR) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("couldn't load", W/2, 90, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString(neoErr, W/2, 120, 1);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("R: retry   L: back", W/2, H - 30, 1);
    } else { // NS_OK
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(neoHazard ? PURPLE_LIGHT : LIME);
        fb.drawString(neoHazard ? "POTENTIALLY HAZARDOUS" : "closest today", W/2, 30, 1);

        // name
        fb.setTextColor(LIME_PALE);
        fb.drawString(neoName, W/2, 52, 1);

        // miss distance — the headline number, in lunar distances
        fb.setTextColor(TEAL_BRIGHT);
        char b[28];
        snprintf(b, sizeof(b), "%.2f LD", neoMissLD);
        fb.drawString(b, W/2, 90, 4);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("lunar distances", W/2, 116, 1);

        // details
        fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
        int y = 140;
        snprintf(b, sizeof(b), "Miss: %.0f km", neoMissKM);
        fb.drawString(b, 10, y, 1); y += 18;
        snprintf(b, sizeof(b), "Speed: %.0f km/h", neoSpeedKmh);
        fb.drawString(b, 10, y, 1); y += 18;
        if (!isnan(neoDiamMinM) && !isnan(neoDiamMaxM)) {
            snprintf(b, sizeof(b), "Size: %.0f-%.0f m", neoDiamMinM, neoDiamMaxM);
            fb.drawString(b, 10, y, 1); y += 18;
        }
        snprintf(b, sizeof(b), "Tracked today: %d", neoCount);
        fb.drawString(b, 10, y, 1); y += 18;

        fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("L: back", 4, H - 7, 1);
        fb.setTextDatum(MR_DATUM); fb.drawString("R: refresh", W - 4, H - 7, 1);
    }

    fb.pushSprite(0, 0);
}
