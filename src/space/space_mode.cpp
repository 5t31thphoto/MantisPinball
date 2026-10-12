// ============================================================================
//  Space Weather + Seismic Report — space_mode.cpp
//
//  Landscape 320×170, exact same sprite lifecycle as weather_mode.cpp.
//  Left half (x=0..159):  Space Weather  (Kp, solar flux, G/S/R scales)
//  Right half (x=161..319): Seismic      (recent global M4.5+ earthquakes)
//
//  Free APIs, no key:
//    NOAA SWPC  (services.swpc.noaa.gov, HTTP port 80)
//      /products/noaa-planetary-k-index.json   — Kp index history (2.5KB)
//      /products/noaa-scales.json              — current G/S/R scales (<1KB)
//      /products/10cm-flux-30-day.json         — F10.7 solar flux index (~1KB)
//    USGS FDSN  (earthquake.usgs.gov, HTTPS port 443 - redirects from HTTP)
//      /fdsnws/event/1/query?format=geojson&minmagnitude=4.5
//        &orderby=time&limit=8                 — recent global M4.5+ (~8KB)
//
//  WiFi credentials: read-only from "btcminer" NVS namespace.
//  No user config needed beyond WiFi (uses global earthquake feed).
//  Refresh every 15 minutes.
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <math.h>
#include "space_mode.h"
#include "../theme_colors.h"
#include "../net_yield.h"
extern bool btnL, btnR, btnL_prev, btnR_prev;

// ── Shared objects (main.cpp) ─────────────────────────────────────────────────
extern TFT_eSPI    tft;
extern TFT_eSprite fb;
extern bool        fbReady;

// ── Landscape dims ────────────────────────────────────────────────────────────
static constexpr int LW = 320;
static constexpr int LH = 170;

// ── Sprite (identical pattern to weather_mode) ───────────────────────────────
static TFT_eSprite* sFb            = nullptr;
static uint8_t      sSavedRotation = 0;

// ── Timings ───────────────────────────────────────────────────────────────────
static constexpr uint32_t SP_REFRESH_MS = 900000;  // 15 min
static constexpr uint32_t SP_WIFI_MS    = 15000;
static constexpr uint32_t SP_RETRY_MS   = 30000;

// ── State ─────────────────────────────────────────────────────────────────────
enum SpPhase { SP_WIFI, SP_FETCH, SP_DISPLAY, SP_ERROR };
static SpPhase  spPhase   = SP_WIFI;
static uint32_t spPhaseAt = 0;
static char     spStatus[48] = "STARTING";

// ── Credentials (read-only from btcminer NVS) ────────────────────────────────
static char  spSsid[64] = "";
static char  spPass[64] = "";
static float spLat      = 0.0f;  // from weather zip (pinball NVS)
static float spLon      = 0.0f;
static char  spCity[40] = "";
static bool  spHasCoords = false;

// ── Kp data: last 8 readings (3h intervals = 24h window) ─────────────────────
static float  kpHistory[8]  = {};
static int    kpCount        = 0;
static float  kpCurrent      = 0.0f;
static int    aRunning        = 0;

// ── NOAA scales (G=geomagnetic, S=solar radiation, R=radio blackout) ─────────
// Each 0=none, 1-5=minor→extreme
static int  scaleG = 0, scaleS = 0, scaleR = 0;

// ── F10.7 solar flux (solar activity proxy) ───────────────────────────────────
static float f107 = 0.0f;   // sfu; quiet=70, active=150, very active=250+

// ── Earthquake data ───────────────────────────────────────────────────────────
static constexpr int EQ_MAX = 8;
struct EqEvent {
    float mag;
    float depth;    // km
    char  place[36];
    long  minsAgo;
};
static EqEvent eqList[EQ_MAX]   = {};   // local/regional
static int     eqCount          = 0;
static EqEvent eqGlobal[EQ_MAX] = {};   // global M5+
static int     eqGlobalCount    = 0;

// ── Fetch timestamp ───────────────────────────────────────────────────────────
static uint32_t spFetchedAt = 0;
static int      spPage       = 0;    // 0=space weather, 1=ISS tracker

// ── ISS data (Open Notify: plain HTTP, no key) ────────────────────────────────
// api.open-notify.org/iss-now.json  — position
// api.open-notify.org/astros.json   — crew
static float  issLat    = 0.0f;
static float  issLon    = 0.0f;
static int    issCrewN  = 0;
static char   issCrew[3][32] = {};  // first 3 crew names
static bool   issOk     = false;
// ISS overhead: true when ISS within ~1500km of user
static bool   issOverhead = false;
static uint32_t issOverheadAt = 0;

// ── HTTPS GET (port 443) for NOAA SWPC which requires HTTPS ─────────────────
static bool spGetS(const char* host, const char* path, char* out, size_t outLen) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12);
    if (!c.connect(host, 443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-sp/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n",
             path, host);
    uint32_t dl = millis() + 12000;
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

// ── HTTP GET (port 80, plain) ─────────────────────────────────────────────────
static bool spGet(const char* host, const char* path, char* buf, size_t blen) {
    WiFiClient c;
    if (!c.connect(host, 80)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-sp/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n",
             path, host);
    uint32_t dl = millis() + 12000;
    // skip headers
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            c.stop(); return false;
        }
    }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (pos < blen - 1) buf[pos++] = (char)ch;
    }
    buf[pos] = '\0';
    c.stop();
    return pos > 0;
}

// ── JSON helpers (no heap, no library) ───────────────────────────────────────
// Find last occurrence of "key": <float> in a flat JSON array of objects
static float jLastFloat(const char* s, const char* key) {
    char pat[32]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    float last = 0.0f;
    const char* p = s;
    while ((p = strstr(p, pat)) != nullptr) {
        p += strlen(pat);
        while (*p && *p != ':') p++;
        if (!*p) break; p++;
        while (*p == ' ') p++;
        char* e; float v = strtof(p, &e);
        if (e != p) last = v;
    }
    return last;
}

static int jLastInt(const char* s, const char* key) {
    char pat[32]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    int last = 0;
    const char* p = s;
    while ((p = strstr(p, pat)) != nullptr) {
        p += strlen(pat);
        while (*p && *p != ':') p++;
        if (!*p) break; p++;
        while (*p == ' ') p++;
        char* e; long v = strtol(p, &e, 10);
        if (e != p) last = (int)v;
    }
    return last;
}

// Extract all Kp values into history array (last kpCount entries)
static void parseKp(const char* body) {
    // Each object: {"time_tag":"...","Kp":N.NN,"a_running":N,...}
    // Collect up to 8 most recent values
    float vals[64]; int n = 0; int aLast = 0;
    const char* p = body;
    while ((p = strstr(p, "\"Kp\"")) != nullptr && n < 64) {
        p += 4;
        while (*p && *p != ':') p++;
        if (!*p) break; p++;
        while (*p == ' ') p++;
        char* e; float v = strtof(p, &e);
        if (e != p) vals[n++] = v;
    }
    // a_running: last value
    aLast = jLastInt(body, "a_running");

    // Take last 8
    int start = (n > 8) ? n - 8 : 0;
    kpCount = n - start;
    for (int i = 0; i < kpCount; i++) kpHistory[i] = vals[start + i];
    kpCurrent = (kpCount > 0) ? kpHistory[kpCount - 1] : 0.0f;
    aRunning = aLast;
}

// Parse noaa-scales.json:
// {"G":{"Scale":"G0","text":"None","MinorProb":"N/A","MajorProb":"N/A","Scale Number":0},...}
static void parseScales(const char* body) {
    // noaa-scales.json format:
    // {"G":{"Scale":"G0","text":"None","MinorProb":"N/A","MajorProb":"N/A","Scale Number":0},...}
    // Search for each scale key, then find "Scale Number" within that object
    auto extractScale = [](const char* s, const char* key) -> int {
        // Find "G": or "S": or "R": 
        char pat[8]; snprintf(pat, sizeof(pat), "\"%s\":", key);
        const char* p = strstr(s, pat);
        if (!p) {
            // Also try without colon (some versions have space)
            snprintf(pat, sizeof(pat), "\"%s\"", key);
            p = strstr(s, pat);
            if (!p) return 0;
        }
        // Find next "Scale Number" within ~200 chars (stays within this object)
        const char* q = strstr(p, "Scale Number");
        if (!q || q - p > 300) return 0;
        q += 12;
        while (*q && *q != ':') q++; if (!*q) return 0; q++;
        while (*q == ' ' || *q == '"') q++;
        char* e; long v = strtol(q, &e, 10);
        if (e == q) return 0;
        return (int)v;
    };
    scaleG = extractScale(body, "G");
    scaleS = extractScale(body, "S");
    scaleR = extractScale(body, "R");
}

// Parse 10cm-flux: last "flux" value
static void parseFlux(const char* body) {
    f107 = jLastFloat(body, "flux");
}

// Parse USGS GeoJSON earthquake list
// {"features":[{"properties":{"mag":5.2,"place":"...","time":12345678,"depth":10},...}]}
// geometry.coordinates[2] = depth in km
static void parseQuakesInto(const char* body, EqEvent* list, int& count) {
    count = 0;
    uint32_t now = millis();

    // USGS time is Unix ms epoch. We don't have RTC, but we can compute
    // "minutes ago" relative to a reference. We'll just show the raw time_since
    // as a relative offset from the most recent event (event 0 = "now").
    // Simpler: USGS returns events newest-first when orderby=time,
    // so event 0 is the most recent. We'll label them "-0h", "-1h" etc
    // based on time delta between events.

    // Find the first event's time to use as reference (Unix ms - needs 64-bit)
    long long firstTime = 0;
    const char* p = strstr(body, "\"time\"");
    if (p) {
        p += 6;
        while (*p && *p != ':') p++; if (*p) p++;
        while (*p == ' ') p++;
        char* e; firstTime = strtoll(p, &e, 10);
    }

    // Parse each feature
    p = body;
    while (count < EQ_MAX) {
        p = strstr(p, "\"properties\"");
        if (!p) break;
        p += 12;

        // Find mag
        const char* magP = strstr(p, "\"mag\"");
        // Find place
        const char* placeP = strstr(p, "\"place\"");
        // Find time
        const char* timeP = strstr(p, "\"time\"");
        // Find depth in geometry (coordinates[2])
        const char* coordP = strstr(p, "\"coordinates\"");

        // Make sure all are within this feature (before next "properties")
        const char* nextProp = strstr(p + 1, "\"properties\"");
        auto inFeature = [&](const char* x) {
            return x && (nextProp == nullptr || x < nextProp);
        };

        if (!inFeature(magP) || !inFeature(placeP) || !inFeature(timeP)) break;

        // mag
        float mag = 0.0f;
        {
            const char* q = magP + 5;
            while (*q && *q != ':') q++; if (*q) q++;
            while (*q == ' ') q++;
            char* e; mag = strtof(q, &e);
            if (e == q) { p++; continue; }
        }

        // place — USGS gives "15km NNE of Taitung, Taiwan". We want just the
        // closest city, so strip the distance/direction prefix ("... of ").
        char place[36] = "";
        {
            const char* q = placeP + 7;
            while (*q && *q != ':') q++; if (*q) q++;
            while (*q == ' ') q++;
            if (*q == '"') {
                q++;
                char raw[64] = "";
                int i = 0;
                while (*q && *q != '"' && i < 63) raw[i++] = *q++;
                raw[i] = 0;
                // If there's an " of " segment, keep what follows it (the city).
                const char* ofp = strstr(raw, " of ");
                const char* cityStart = ofp ? ofp + 4 : raw;
                strncpy(place, cityStart, 35);
                place[35] = 0;
            }
        }

        // time (Unix ms) — must use 64-bit; 32-bit long overflows at ~2B
        long long evTime = 0;
        {
            const char* q = timeP + 6;
            while (*q && *q != ':') q++; if (*q) q++;
            while (*q == ' ') q++;
            char* e; evTime = strtoll(q, &e, 10);
            if (e == q) evTime = 0;
        }

        // depth from coordinates array: [lon, lat, depth_km]
        float depth = 0.0f;
        if (inFeature(coordP)) {
            const char* q = coordP + 13;
            while (*q && *q != '[') q++; if (*q) q++;
            // skip lon
            while (*q && *q != ',') q++; if (*q) q++;
            // skip lat
            while (*q && *q != ',') q++; if (*q) q++;
            while (*q == ' ') q++;
            char* e; depth = strtof(q, &e);
        }

        // minutes ago relative to first event
        long minsAgo = 0;
        if (firstTime > 0 && evTime > 0) {
            long long deltaMs = firstTime - evTime;  // ms, 64-bit safe
            minsAgo = (long)(deltaMs / 60000LL);
        }

        list[count].mag     = mag;
        list[count].depth   = depth;
        list[count].minsAgo = minsAgo;
        strncpy(list[count].place, place, 35);
        list[count].place[35] = 0;
        count++;
        p++;
    }
}

static void parseQuakes(const char* body) {
    parseQuakesInto(body, eqList, eqCount);
}

// ── Great-circle distance approx (km) ────────────────────────────────────────
static float spGCDist(float lat1, float lon1, float lat2, float lon2) {
    // Equirectangular approx — good enough for < 3000km
    float dLat = (lat2 - lat1) * 0.01745329f;
    float dLon = (lon2 - lon1) * 0.01745329f;
    float lat1r = lat1 * 0.01745329f;
    float lat2r = lat2 * 0.01745329f;
    float x = dLon * cosf((lat1r + lat2r) * 0.5f);
    float y = dLat;
    return sqrtf(x*x + y*y) * 6371.0f;
}

// ── Fetch ISS position + crew (plain HTTP, Open Notify) ───────────────────────
static void spFetchISS() {
    static char buf[1024];

    // Position
    memset(buf, 0, sizeof(buf));
    if (spGet("api.open-notify.org", "/iss-now.json", buf, sizeof(buf))) {
        const char* pos = strstr(buf, "iss_position");
        if (pos) {
            // iss_position has "latitude":"N.NNN" (string-quoted floats)
            // jLastFloat handles the key search the same way
            // ISS JSON: "latitude": "51.4983" — value is a quoted string
            // jLastFloat stops at the leading '"' so we need a manual parse
            auto parseISSCoord = [](const char* s, const char* key) -> float {
                char pat[32]; snprintf(pat, sizeof(pat), "\"%s\"", key);
                const char* p = strstr(s, pat);
                if (!p) return 0.0f;
                p += strlen(pat);
                while (*p && *p != ':') p++; if (!*p) return 0.0f; p++;
                while (*p == ' ') p++;
                if (*p == '"') p++;   // skip opening quote on string value
                char* e; float v = strtof(p, &e);
                return (e == p) ? 0.0f : v;
            };
            float lat = parseISSCoord(buf, "latitude");
            float lon = parseISSCoord(buf, "longitude");
            issLat = lat; issLon = lon; issOk = true;
            // Check overhead (within 1500km of user if we have coords)
            if (spHasCoords) {
                float dist = spGCDist(spLat, spLon, lat, lon);
                issOverhead = (dist < 1500.0f);
                if (issOverhead && issOverheadAt == 0)
                    issOverheadAt = millis();
                else if (!issOverhead)
                    issOverheadAt = 0;
            }
        }
    }

    // Crew
    memset(buf, 0, sizeof(buf));
    if (spGet("api.open-notify.org", "/astros.json", buf, sizeof(buf))) {
        // actual count parsed below from "number" field
        // Parse first 3 names
        const char* p = buf; issCrewN = 0;
        while (issCrewN < 3) {
            p = strstr(p + 1, "\"name\"");
            if (!p) break;
            // Parse "name":"..." manually
            char name[32] = "";
            const char* nk = strstr(p, "\"name\"");
            if (nk) {
                nk += 6;
                while (*nk && *nk != ':') nk++;
                if (*nk) nk++;
                while (*nk == ' ') nk++;
                if (*nk == '"') { nk++; int ni = 0;
                    while (*nk && *nk != '"' && ni < 31) name[ni++] = *nk++;
                    name[ni] = '\0';
                }
            }
            if (name[0]) { strncpy(issCrew[issCrewN++], name, 31); }
        }
        // Get actual count
        issCrewN = jLastInt(buf, "number");
    }
}

// ── Fetch all data (called in SP_FETCH state) ─────────────────────────────────
static bool spFetchAll() {
    static char buf[4096];

    // 1. Kp index
    memset(buf, 0, sizeof(buf));
    if (spGetS("services.swpc.noaa.gov",
               "/products/noaa-planetary-k-index.json",
               buf, sizeof(buf)))
        parseKp(buf);

    // 2. NOAA scales
    memset(buf, 0, sizeof(buf));
    if (spGetS("services.swpc.noaa.gov",
               "/products/noaa-scales.json",
               buf, sizeof(buf)))
        parseScales(buf);

    // 3. F10.7 flux (10cm-flux-30-day.json, ~1KB)
    memset(buf, 0, sizeof(buf));
    if (spGetS("services.swpc.noaa.gov",
               "/products/10cm-flux-30-day.json",
               buf, sizeof(buf)))
        parseFlux(buf);

    // 4. Earthquakes: regional (near weather zip) if coords available,
    //    otherwise global M5.5+ as fallback.
    static char eqBuf[8192];
    memset(eqBuf, 0, sizeof(eqBuf));
    char eqPath[256];
    // Always fetch global M5.0+ (last 7 days, 4 most recent)
    eqGlobalCount = 0;
    snprintf(eqPath, sizeof(eqPath),
        "/fdsnws/event/1/query?format=geojson"
        "&minmagnitude=5.0"
        "&orderby=time&limit=4");
    if (spGetS("earthquake.usgs.gov", eqPath, eqBuf, sizeof(eqBuf)))
        parseQuakesInto(eqBuf, eqGlobal, eqGlobalCount);

    // Also fetch regional if we have coords (M2.0+, last 30 days, 4 most recent)
    eqCount = 0;
    if (spHasCoords) {
        snprintf(eqPath, sizeof(eqPath),
            "/fdsnws/event/1/query?format=geojson"
            "&latitude=%.4f&longitude=%.4f&maxradiuskm=500"
            "&minmagnitude=2.0&orderby=time&limit=4",
            spLat, spLon);
        if (spGetS("earthquake.usgs.gov", eqPath, eqBuf, sizeof(eqBuf)))
            parseQuakesInto(eqBuf, eqList, eqCount);
    }

    // Fetch ISS position + crew (fast, plain HTTP, separate from SWPC)
    spFetchISS();

    return (kpCount > 0);
}

// ── Kp helpers ────────────────────────────────────────────────────────────────
static uint16_t kpColor(float kp) {
    if (kp < 3.0f) return LIME;
    if (kp < 4.0f) return rgb565(200, 200, 0);
    if (kp < 5.0f) return rgb565(220, 140, 0);
    if (kp < 6.0f) return rgb565(220, 60, 0);
    return PURPLE_BRIGHT;
}
static const char* kpCondition(float kp) {
    if (kp < 1.0f) return "QUIET";
    if (kp < 2.0f) return "QUIET";
    if (kp < 3.0f) return "UNSETTLED";
    if (kp < 4.0f) return "ACTIVE";
    if (kp < 5.0f) return "MINOR STORM";
    if (kp < 6.0f) return "MOD STORM";
    if (kp < 7.0f) return "STRONG STORM";
    if (kp < 8.0f) return "SEVERE STORM";
    if (kp < 9.0f) return "SEVERE+";
    return "EXTREME";
}

static uint16_t scaleColor(int s) {
    if (s == 0) return TEAL_DARK;
    if (s == 1) return LIME_DARK;
    if (s == 2) return LIME;
    if (s == 3) return rgb565(220, 140, 0);
    if (s == 4) return rgb565(220, 60, 0);
    return PURPLE_BRIGHT;
}
static const char* scaleName(int s) {
    static const char* n[] = {"0","1","2","3","4","5"};
    if (s < 0 || s > 5) return "?";
    return n[s];
}

// ── Magnitude color ───────────────────────────────────────────────────────────
static uint16_t magColor(float m) {
    if (m < 3.0f) return TEAL;
    if (m < 4.0f) return TEAL_BRIGHT;
    if (m < 5.0f) return LIME;
    if (m < 6.0f) return rgb565(220, 200, 0);
    if (m < 7.0f) return rgb565(220, 100, 0);
    return PURPLE_BRIGHT;
}

// ── Draw helpers (using sFb) ───────────────────────────────────────────────────
static void sp(const char* s, int x, int y, int font, uint16_t c,
               uint8_t datum = TL_DATUM) {
    sFb->setTextColor(c);
    sFb->setTextDatum(datum);
    sFb->drawString(s, x, y, font);
}
static void spn(float v, const char* fmt, int x, int y, int font, uint16_t c,
                uint8_t datum = TL_DATUM) {
    char buf[20];
    snprintf(buf, sizeof(buf), fmt, v);
    sFb->setTextColor(c); sFb->setTextDatum(datum);
    sFb->drawString(buf, x, y, font);
}

// ── Loading / error screens ───────────────────────────────────────────────────
static void spDrawLoading() {
    sFb->fillSprite(TEAL_DEEP);
    sFb->setTextDatum(MC_DATUM);
    sp("SPACE WEATHER", LW/2, 50, 4, LIME, MC_DATUM);
    sp(spStatus, LW/2, 90, 2, TEAL_BRIGHT, MC_DATUM);
    uint32_t t = millis() / 350;
    for (int i = 0; i < 5; i++) {
        uint16_t c = ((t % 5) == (uint32_t)i) ? LIME : TEAL_DARK;
        sFb->fillCircle(LW/2 - 32 + i*16, 115, 5, c);
    }
    sFb->pushSprite(0, 0);
}
static void spDrawError() {
    sFb->fillSprite(TEAL_DEEP);
    sFb->setTextDatum(MC_DATUM);
    sp("FETCH ERROR", LW/2, 60, 2, PURPLE_BRIGHT, MC_DATUM);
    sp(spStatus, LW/2, 84, 1, TEAL_BRIGHT, MC_DATUM);
    uint32_t ret = (SP_RETRY_MS - (millis() - spPhaseAt)) / 1000;
    char rb[24]; snprintf(rb, sizeof(rb), "retry in %lus", (unsigned long)ret);
    sp(rb, LW/2, 106, 1, TEAL, MC_DATUM);
    sFb->pushSprite(0, 0);
}

// ── Main display ──────────────────────────────────────────────────────────────
// ── Draw page 2: ISS Tracker ─────────────────────────────────────────────────
static void spDrawISS() {
    sFb->fillSprite(TEAL_DEEP);

    // Header
    sFb->fillRect(0, 0, LW, 14, TEAL_DARK);
    sp("ISS TRACKER", 4, 3, 1, LIME);
    sp("< L/R >", LW - 4, 3, 1, TEAL_DARK, TR_DATUM);

    // ── ISS Overhead celebration ─────────────────────────────────────────────
    if (issOverhead) {
        // Flashing aurora-green border
        uint32_t flash = (millis() / 200) & 1;
        uint16_t borderCol = flash ? LIME : LIME_DARK;
        sFb->drawRect(0, 0, LW, LH, borderCol);
        sFb->drawRect(1, 1, LW-2, LH-2, borderCol);
    }

    // ── Aurora visual cue (Kp >= 5 locally) ──────────────────────────────────
    bool auroraLocal = (kpCurrent >= 5.0f && spHasCoords && fabsf(spLat) > 30.0f);
    // Kp 5 = aurora visible above ~65° lat, 7 above ~55°, 9 above ~40°
    float auroraMinLat = 65.0f - (kpCurrent - 5.0f) * 5.0f;
    if (spHasCoords && kpCurrent >= 5.0f && fabsf(spLat) >= auroraMinLat)
        auroraLocal = true;

    if (auroraLocal) {
        uint32_t t = millis() / 150;
        // Animated aurora shimmer at top of screen
        for (int x = 0; x < LW; x++) {
            uint8_t h = (uint8_t)((sinf(x * 0.08f + t * 0.3f) + 1.0f) * 6.0f);
            uint16_t aCol = ((t + x/20) & 1) ? LIME_DARK : rgb565(0, 60+h*3, 40+h*2);
            for (int y = 15; y < 15 + h + 2; y++)
                sFb->drawPixel(x, y, aCol);
        }
    }

    int yBase = auroraLocal ? 32 : 18;

    // ── ISS Position ──────────────────────────────────────────────────────────
    if (issOk) {
        sp("ISS POSITION", 4, yBase, 1, TEAL);
        char posBuf[32];
        snprintf(posBuf, sizeof(posBuf), "%.2f°%c  %.2f°%c",
            fabsf(issLat), issLat >= 0 ? 'N' : 'S',
            fabsf(issLon), issLon >= 0 ? 'E' : 'W');
        sp(posBuf, 4, yBase + 10, 2, issOverhead ? LIME : TEAL_BRIGHT);

        if (issOverhead) {
            uint32_t flash2 = (millis() / 400) & 1;
            sp(flash2 ? "** OVERHEAD NOW **" : "                  ",
               LW/2, yBase + 26, 1, LIME, MC_DATUM);
        } else if (spHasCoords) {
            float dist = spGCDist(spLat, spLon, issLat, issLon);
            char distBuf[32]; snprintf(distBuf, sizeof(distBuf), "%.0f mi from you", dist * 0.621371f);
            sp(distBuf, 4, yBase + 26, 1, TEAL);
        }
    } else {
        sp("ISS: no data", 4, yBase + 10, 1, TEAL_DARK);
    }

    // ── Crew ──────────────────────────────────────────────────────────────────
    int crewY = yBase + 42;
    sFb->drawFastHLine(2, crewY - 4, LW-4, TEAL_DARK);
    char crewHdr[24]; snprintf(crewHdr, sizeof(crewHdr), "CREW IN SPACE: %d", issCrewN);
    sp(crewHdr, 4, crewY, 1, TEAL);
    for (int i = 0; i < 3 && i < issCrewN; i++) {
        sp(issCrew[i], 4, crewY + 10 + i*10, 1, TEAL_BRIGHT);
    }
    if (issCrewN > 3) {
        char more[16]; snprintf(more, sizeof(more), "+ %d more", issCrewN - 3);
        sp(more, 4, crewY + 42, 1, TEAL_DARK);
    }

    // ── Solar Activity Summary (mirrored from page 1) ─────────────────────────
    int solY = crewY + 58;
    sFb->drawFastHLine(2, solY - 4, LW-4, TEAL_DARK);
    sp("SOLAR ACTIVITY", 4, solY, 1, TEAL);

    char kpLine[32]; snprintf(kpLine, sizeof(kpLine), "Kp=%.1f  %s", kpCurrent, kpCondition(kpCurrent));
    sp(kpLine, 4, solY + 10, 1, kpColor(kpCurrent));

    char scaleLine[40];
    snprintf(scaleLine, sizeof(scaleLine), "G%s  S%s  R%s   F10.7: %.0f",
        scaleName(scaleG), scaleName(scaleS), scaleName(scaleR), f107);
    sp(scaleLine, 4, solY + 22, 1, TEAL_BRIGHT);

    if (auroraLocal) {
        sp("AURORA POSSIBLE AT YOUR LAT!", LW/2, solY + 34, 1, LIME, MC_DATUM);
    } else {
        char auBuf[40]; snprintf(auBuf, sizeof(auBuf), "Aurora: lat >= %.0f°", 
            65.0f - (kpCurrent > 5 ? (kpCurrent - 5.0f) * 5.0f : 0));
        sp(auBuf, 4, solY + 34, 1, TEAL_DARK);
    }

    // ── Age indicator ─────────────────────────────────────────────────────────
    uint32_t ageSec = (millis() - spFetchedAt) / 1000;
    char ageBuf[24]; snprintf(ageBuf, sizeof(ageBuf), "upd %lus ago", (unsigned long)ageSec);
    sp(ageBuf, LW - 4, LH - 4, 1, TEAL_DARK, BR_DATUM);

    sFb->pushSprite(0, 0);
}

static void spDrawDisplay() {
    sFb->fillSprite(TEAL_DEEP);

    // ── Header bar ──────────────────────────────────────────────────────────
    sFb->fillRect(0, 0, LW, 14, TEAL_DARK);
    sp("SPACE WEATHER + SEISMIC", 4, 3, 1, LIME);
    uint32_t ageSec = (millis() - spFetchedAt) / 1000;
    char ageBuf[24]; snprintf(ageBuf, sizeof(ageBuf), "upd %lus ago", (unsigned long)ageSec);
    sp(ageBuf, LW - 4, 3, 1, TEAL, TR_DATUM);

    // Center divider
    sFb->drawFastVLine(160, 14, LH - 14, TEAL_DARK);

    // ══════════════════════════════════════════════════════════════════════
    // LEFT PANEL  x=0..159  SPACE WEATHER
    // ══════════════════════════════════════════════════════════════════════

    // ── Kp big number ───────────────────────────────────────────────────────
    char kpBuf[8]; snprintf(kpBuf, sizeof(kpBuf), "%.1f", kpCurrent);
    sp("Kp", 4, 17, 1, TEAL_DARK);
    sp(kpBuf, 4, 26, 6, kpColor(kpCurrent));     // large font
    sp(kpCondition(kpCurrent), 4, 66, 1, kpColor(kpCurrent));

    // A-index
    char aBuf[12]; snprintf(aBuf, sizeof(aBuf), "A=%d", aRunning);
    sp(aBuf, 4, 78, 1, TEAL_BRIGHT);

    // Aurora visibility hint (based on Kp and lat ~40N for Utah)
    // Kp ≥ 5 → visible at high lats, ≥ 7 → mid-lats, ≥ 9 → low lats
    const char* aurora;
    if      (kpCurrent >= 8.0f) aurora = "AURORA: LOW LAT";
    else if (kpCurrent >= 6.0f) aurora = "AURORA: MID LAT";
    else if (kpCurrent >= 5.0f) aurora = "AURORA: HIGH LAT";
    else if (kpCurrent >= 4.0f) aurora = "AURORA: POLAR";
    else                         aurora = "AURORA: POLAR ONLY";
    sp(aurora, 4, 90, 1, kpCurrent >= 5.0f ? LIME : TEAL_DARK);

    // ── Kp 24h bar graph ────────────────────────────────────────────────────
    // 8 bars each 10px wide, y=100..132, graph height 32px
    sFb->drawFastHLine(2, 100, 155, TEAL_DARK);
    sp("24h Kp", 2, 102, 1, TEAL_DARK);
    int barW = 10, barMaxH = 28;
    for (int i = 0; i < kpCount; i++) {
        int bx = 2 + i * (barW + 2);
        int bh = (int)(kpHistory[i] / 9.0f * barMaxH);
        if (bh < 1) bh = 1;
        // Background
        sFb->fillRect(bx, 113 + barMaxH - barMaxH, barW, barMaxH, TEAL_DARK);
        // Bar
        sFb->fillRect(bx, 113 + barMaxH - bh, barW, bh, kpColor(kpHistory[i]));
    }
    // Scale marks at y=113 (0) and y=113-28=-15? No, y=113 = bottom
    // Kp=9 line at top of bar area: y = 113
    // Kp=5 storm threshold line: y = 113 - (5/9*28) ≈ 113 - 15 = 98
    int stormY = 113 - (int)(5.0f / 9.0f * barMaxH);
    sFb->drawFastHLine(2, stormY, kpCount * 12, rgb565(220, 60, 0));

    // ── NOAA Scales ─────────────────────────────────────────────────────────
    sFb->drawFastHLine(2, 143, 155, TEAL_DARK);

    // G scale (geomagnetic)
    sp("G", 4, 147, 1, TEAL_DARK);
    char gBuf[4]; snprintf(gBuf, sizeof(gBuf), "G%s", scaleName(scaleG));
    sp(gBuf, 16, 147, 1, scaleColor(scaleG));

    // S scale (solar radiation)
    sp("S", 46, 147, 1, TEAL_DARK);
    char sBuf[4]; snprintf(sBuf, sizeof(sBuf), "S%s", scaleName(scaleS));
    sp(sBuf, 58, 147, 1, scaleColor(scaleS));

    // R scale (radio blackout)
    sp("R", 88, 147, 1, TEAL_DARK);
    char rBuf[4]; snprintf(rBuf, sizeof(rBuf), "R%s", scaleName(scaleR));
    sp(rBuf, 100, 147, 1, scaleColor(scaleR));

    // F10.7 flux
    sp("F10.7", 4, 158, 1, TEAL_DARK);
    const char* actLvl = f107 < 80 ? "QUIET" : f107 < 120 ? "MODERATE" : f107 < 180 ? "ACTIVE" : "HIGH";
    uint16_t actCol = f107 < 80 ? TEAL_BRIGHT : f107 < 120 ? LIME : f107 < 180 ? rgb565(220, 140, 0) : PURPLE_BRIGHT;
    char fluxBuf[20]; snprintf(fluxBuf, sizeof(fluxBuf), "%.0f sfu (%s)", f107, actLvl);
    sp(fluxBuf, 38, 158, 1, actCol);

    // ══════════════════════════════════════════════════════════════════════
    // RIGHT PANEL  x=162..319  SEISMIC (local top half, global bottom half)
    // Total height: 14..170 = 156px. Split at y=92.
    // Each section: header(10px) + 4 rows of 19px = 86px fits in 78px → use 18px rows
    // ══════════════════════════════════════════════════════════════════════
    int rx = 163;
    constexpr int EQ_ROW_H  = 18;
    constexpr int EQ_ROWS   = 4;

    // Helper lambda to draw one section
    auto drawEqSection = [&](const char* hdr, EqEvent* list, int cnt,
                              int startY, bool localSection) {
        // Section label
        sp(hdr, rx, startY, 1, localSection ? TEAL_BRIGHT : TEAL);

        if (cnt == 0) {
            sp(localSection ? "none in 30d" : "none in 7d",
               rx + 4, startY + 12, 1, TEAL_DARK);
            return;
        }
        for (int i = 0; i < cnt && i < EQ_ROWS; i++) {
            int ry = startY + 10 + i * EQ_ROW_H;
            EqEvent& eq = list[i];

            // Mag badge (22px wide)
            char magStr[6]; snprintf(magStr, sizeof(magStr), "M%.1f", eq.mag);
            uint16_t mc = magColor(eq.mag);
            sFb->fillRect(rx, ry, 22, EQ_ROW_H - 2, TEAL_DARK);
            sFb->drawRect(rx, ry, 22, EQ_ROW_H - 2, mc);
            sp(magStr, rx + 11, ry + EQ_ROW_H/2 - 4, 1, mc, TC_DATUM);

            // Time (right-aligned)
            char timeStr[10];
            if (eq.minsAgo < 60)
                snprintf(timeStr, sizeof(timeStr), "-%ldm", eq.minsAgo);
            else if (eq.minsAgo < 60*24)
                snprintf(timeStr, sizeof(timeStr), "-%.0fh", eq.minsAgo/60.0f);
            else
                snprintf(timeStr, sizeof(timeStr), "-%.0fd", eq.minsAgo/1440.0f);
            sp(timeStr, 317, ry, 1, TEAL_DARK, TR_DATUM);

            // Place (truncated, 16 chars max in remaining width)
            char place[17];
            strncpy(place, eq.place, 16); place[16] = 0;
            sp(place, rx + 25, ry + 1, 1, TEAL_BRIGHT);

            // Depth on second line (convert km -> miles)
            char depStr[12]; snprintf(depStr, sizeof(depStr), "%.0fmi deep", eq.depth * 0.621371f);
            sp(depStr, rx + 25, ry + 9, 1, TEAL);
        }
    };

    // Local section (top): y=16..91
    if (spHasCoords) {
        char localHdr[24];
        if (spCity[0]) snprintf(localHdr, sizeof(localHdr), "LOCAL 500km M2+");
        else           strncpy(localHdr, "LOCAL 500km M2+", 24);
        drawEqSection(localHdr, eqList, eqCount, 16, true);
    }

    // Divider
    int divY = spHasCoords ? 92 : 16;
    if (spHasCoords)
        sFb->drawFastHLine(rx, divY, 155, TEAL_DARK);

    // Global section (bottom or full panel if no coords)
    drawEqSection("GLOBAL M5+ 7d", eqGlobal, eqGlobalCount,
                  divY + (spHasCoords ? 2 : 0), false);

    sFb->pushSprite(0, 0);
}

// ── Sprite lifecycle (identical pattern to weatherEnter/weatherShutdown) ──────
static void spAcquireSprite() {
    if (sFb != nullptr) return;
    fbReady = false;
    sSavedRotation = tft.getRotation();
    tft.setRotation(3);
    tft.fillScreen(TEAL_DEEP);
    fb.deleteSprite();
    sFb = &fb;
    sFb->setColorDepth(16);
    if (!sFb->createSprite(LW, LH)) {
        sFb->setColorDepth(8);
        sFb->createSprite(LW, LH);
    }
}
static void spReleaseSprite() {
    if (sFb == nullptr) return;
    sFb->deleteSprite();
    sFb = nullptr;
    tft.setRotation(sSavedRotation);
    tft.fillScreen(TEAL_DEEP);
    fb.setColorDepth(16);
    if (!fb.createSprite(170, 320)) {
        fb.setColorDepth(8);
        fb.createSprite(170, 320);
    }
    fbReady = true;
}

// ── Public API ────────────────────────────────────────────────────────────────
void spaceEnter() {
    kpCount = 0; eqCount = 0; eqGlobalCount = 0; kpCurrent = 0; spPage = 0; issOk = false;
    scaleG = scaleS = scaleR = 0; f107 = 0;

    // Load WiFi creds from btcminer namespace (read-only)
    { Preferences p;
      if (p.begin("btcminer", true)) {
          p.getString("ssid", spSsid, sizeof(spSsid));
          p.getString("pass", spPass, sizeof(spPass));
          p.end();
      }
    }
    // Load location from pinball namespace (set by weather mode)
    { Preferences p;
      if (p.begin("pinball", true)) {
          spLat = p.getFloat("wx_lat", 0.0f);
          spLon = p.getFloat("wx_lon", 0.0f);
          p.getString("wx_city", spCity, sizeof(spCity));
          p.end();
      }
    }
    spHasCoords = (spLat != 0.0f || spLon != 0.0f);

    spAcquireSprite();

    if (spSsid[0] == '\0') {
        spPhase = SP_ERROR; spPhaseAt = millis();
        snprintf(spStatus, sizeof(spStatus), "NO WIFI - SET UP MINER FIRST");
    } else {
        spPhase = SP_WIFI; spPhaseAt = millis();
        snprintf(spStatus, sizeof(spStatus), "CONNECTING: %s", spSsid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(spSsid, spPass);
    }
}

void spaceShutdown() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    spReleaseSprite();
    delay(50);
}

void spaceTick() {
    uint32_t now = millis();

    switch (spPhase) {

        case SP_WIFI:
            if (WiFi.status() == WL_CONNECTED) {
                snprintf(spStatus, sizeof(spStatus), "FETCHING DATA");
                spPhase = SP_FETCH; spPhaseAt = now;
            } else if (now - spPhaseAt > SP_WIFI_MS) {
                spPhase = SP_ERROR; spPhaseAt = now;
                snprintf(spStatus, sizeof(spStatus), "WIFI TIMEOUT");
            }
            break;

        case SP_FETCH:
            snprintf(spStatus, sizeof(spStatus), "DOWNLOADING");
            if (spFetchAll()) {
                spFetchedAt = now;
                spPhase = SP_DISPLAY; spPhaseAt = now;
            } else {
                spPhase = SP_ERROR; spPhaseAt = now;
                snprintf(spStatus, sizeof(spStatus), "FETCH FAILED");
            }
            break;

        case SP_DISPLAY:
            // Single click L or R = toggle between page 0 (space wx) and page 1 (ISS)
            // (double-click both is caught by main.cpp first → menu exit)
            {
                bool rEdge = btnR && !btnR_prev && !btnL;
                bool lEdge = btnL && !btnL_prev && !btnR;
                if (rEdge || lEdge) spPage = 1 - spPage;
            }
            // Refresh ISS position more often on ISS page (every 30s)
            if (spPage == 1 && now - spFetchedAt >= 30000) {
                spFetchISS();
                spFetchedAt = now;  // update for ISS page specifically
            }
            // Full refresh every 15 minutes
            if (now - spFetchedAt >= SP_REFRESH_MS) {
                spPhase = SP_FETCH; spPhaseAt = now;
                snprintf(spStatus, sizeof(spStatus), "REFRESHING");
            }
            break;

        case SP_ERROR:
            if (now - spPhaseAt >= SP_RETRY_MS) {
                spPhase = SP_WIFI; spPhaseAt = now;
                snprintf(spStatus, sizeof(spStatus), "RECONNECTING");
                WiFi.mode(WIFI_STA);
                WiFi.begin(spSsid, spPass);
            }
            break;
    }
}

void spaceDraw() {
    if (!sFb) return;
    switch (spPhase) {
        case SP_WIFI:    spDrawLoading(); break;
        case SP_FETCH:   spDrawLoading(); break;
        case SP_DISPLAY:
            if (spPage == 1) spDrawISS();
            else             spDrawDisplay();
            break;
        case SP_ERROR:   spDrawError();   break;
    }
}
