// ham_mode.cpp - Ham/shortwave propagation dashboard (hamqsl.com XML, keyless).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <string.h>
#include "ham_mode.h"
#include "../net_yield.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
#include "../chord_guard.h"
static constexpr int W = 170;
static constexpr int H = 320;

static const char* NVS_MINER = "btcminer";          // shared WiFi creds
static const char* HAM_HOST  = "www.hamqsl.com";
static const char* HAM_PATH  = "/solarxml.php";

enum HamState { HS_IDLE, HS_FETCHING, HS_OK, HS_ERR };
static HamState hamState = HS_IDLE;
static char hamErr[40] = "";
static int  hamPage = 0;   // 0 = bands, 1 = solar detail

// Solar metrics
static char hamFlux[12]   = "--";
static char hamSunspots[12]= "--";
static char hamKindex[12] = "--";
static char hamAindex[12] = "--";
static char hamXray[12]   = "--";
static char hamAurora[12] = "--";
static char hamSolarWind[12] = "--";
static char hamMuf[12]    = "--";
static char hamUpdated[40]= "";

// Band conditions: 4 HF spectra x {day,night}
struct Band { const char* label; char day[10]; char night[10]; };
static Band hamBands[4] = {
    { "80m-40m", "--", "--" },
    { "30m-20m", "--", "--" },
    { "17m-15m", "--", "--" },
    { "12m-10m", "--", "--" },
};
// hamqsl uses these exact name attributes:
static const char* BAND_KEYS[4] = { "80m-40m", "30m-20m", "17m-15m", "12m-10m" };

static char hamSsid[40] = "", hamPass[64] = "";

static void hamLoadCreds() {
    Preferences p;
    if (p.begin(NVS_MINER, true)) {
        p.getString("ssid", hamSsid, sizeof(hamSsid));
        p.getString("pass", hamPass, sizeof(hamPass));
        p.end();
    }
}

static char* hamBuf = nullptr;
static const size_t HAM_BUF = 12000;   // the XML feed is a few KB

// Extract the text content of <tag>...</tag> into out (first occurrence).
static bool xmlTag(const char* xml, const char* tag, char* out, size_t outlen) {
    char open[40];
    snprintf(open, sizeof(open), "<%s>", tag);
    const char* s = strstr(xml, open);
    if (!s) { out[0] = 0; return false; }
    s += strlen(open);
    // skip CDATA wrapper if present
    if (strncmp(s, "<![CDATA[", 9) == 0) s += 9;
    size_t k = 0;
    while (*s && *s != '<' && k < outlen - 1) out[k++] = *s++;
    out[k] = 0;
    // trim trailing/leading whitespace
    while (k > 0 && (out[k-1] == ' ' || out[k-1] == '\n' || out[k-1] == '\r' || out[k-1] == '\t')) out[--k] = 0;
    return k > 0;
}

// Parse a <band name="80m-40m" time="day">Good</band> entry.
static bool xmlBand(const char* xml, const char* name, const char* time, char* out, size_t outlen) {
    // find a <band ...> whose attributes contain both name="X" and time="Y"
    char namePat[24], timePat[20];
    snprintf(namePat, sizeof(namePat), "name=\"%s\"", name);
    snprintf(timePat, sizeof(timePat), "time=\"%s\"", time);
    const char* p = xml;
    while ((p = strstr(p, "<band")) != nullptr) {
        const char* end = strchr(p, '>');
        if (!end) break;
        // is this opening tag for our name+time?
        // (attributes live between p and end)
        bool hasName = false, hasTime = false;
        for (const char* q = p; q < end; q++) {
            if (!hasName && strncmp(q, namePat, strlen(namePat)) == 0) hasName = true;
            if (!hasTime && strncmp(q, timePat, strlen(timePat)) == 0) hasTime = true;
        }
        if (hasName && hasTime) {
            const char* val = end + 1;
            size_t k = 0;
            while (*val && *val != '<' && k < outlen - 1) out[k++] = *val++;
            out[k] = 0;
            while (k > 0 && (out[k-1] == ' ' || out[k-1] == '\n' || out[k-1]=='\r')) out[--k] = 0;
            return k > 0;
        }
        p = end + 1;
    }
    out[0] = 0;
    return false;
}

static bool hamParse() {
    if (!hamBuf) return false;
    // require at least the solarflux tag to consider it valid
    bool ok = xmlTag(hamBuf, "solarflux", hamFlux, sizeof(hamFlux));
    xmlTag(hamBuf, "sunspots", hamSunspots, sizeof(hamSunspots));
    xmlTag(hamBuf, "kindex", hamKindex, sizeof(hamKindex));
    xmlTag(hamBuf, "aindex", hamAindex, sizeof(hamAindex));
    xmlTag(hamBuf, "xray", hamXray, sizeof(hamXray));
    xmlTag(hamBuf, "aurora", hamAurora, sizeof(hamAurora));
    xmlTag(hamBuf, "solarwind", hamSolarWind, sizeof(hamSolarWind));
    xmlTag(hamBuf, "muf", hamMuf, sizeof(hamMuf));
    xmlTag(hamBuf, "updated", hamUpdated, sizeof(hamUpdated));
    for (int i = 0; i < 4; i++) {
        xmlBand(hamBuf, BAND_KEYS[i], "day",   hamBands[i].day,   sizeof(hamBands[i].day));
        xmlBand(hamBuf, BAND_KEYS[i], "night", hamBands[i].night, sizeof(hamBands[i].night));
    }
    return ok;
}

static bool hamHttpGet(const char* host, const char* path) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12);
    if (!c.connect(host, 443)) { strncpy(hamErr, "connect failed", sizeof(hamErr)); return false; }
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\n"
             "User-Agent: Mozilla/5.0 (pinball-ham)\r\n"
             "Accept: application/xml,text/xml\r\nConnection: close\r\n\r\n", path, host);
    uint32_t dl = millis() + 15000;
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            strncpy(hamErr, "http error", sizeof(hamErr)); c.stop(); return false;
        }
    }
    size_t pos = 0;
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) { sysNetYield(); if (sysNetAborted()) break; delay(1); continue; }
        if (pos < HAM_BUF - 1) hamBuf[pos++] = (char)ch;
    }
    hamBuf[pos] = 0;
    c.stop();
    return pos > 0;
}

static void hamFetch() {
    hamState = HS_FETCHING;
    hamErr[0] = 0;
    if (hamSsid[0] == 0) { strncpy(hamErr, "no WiFi set (use Weather)", sizeof(hamErr)); hamState = HS_ERR; return; }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(hamSsid, hamPass);
        uint32_t dl = millis() + 15000;
        while (WiFi.status() != WL_CONNECTED && millis() < dl) { sysNetYield(); if (sysNetAborted()) break; delay(10); }
        if (WiFi.status() != WL_CONNECTED) { strncpy(hamErr, "WiFi connect failed", sizeof(hamErr)); hamState = HS_ERR; return; }
    }

    hamBuf = (char*)malloc(HAM_BUF);
    if (!hamBuf) { strncpy(hamErr, "out of memory", sizeof(hamErr)); hamState = HS_ERR; return; }

    bool ok = hamHttpGet(HAM_HOST, HAM_PATH);
    if (ok) ok = hamParse();
    free(hamBuf); hamBuf = nullptr;

    if (ok) hamState = HS_OK;
    else { if (!hamErr[0]) strncpy(hamErr, "no data / parse fail", sizeof(hamErr)); hamState = HS_ERR; }
}

void hamEnter() {
    chordGuardReset();
    fb.setRotation(0);
    hamLoadCreds();
    hamState = HS_IDLE;
    hamPage = 0;
}
void hamShutdown() {
    if (hamBuf) { free(hamBuf); hamBuf = nullptr; }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void hamTick() {
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();
    if (hamState == HS_IDLE && (lEdge || rEdge)) { hamFetch(); return; }
    if (hamState == HS_OK) {
        if (lEdge) hamPage ^= 1;       // toggle bands / solar detail
        if (rEdge) hamFetch();         // refresh
    }
    if (hamState == HS_ERR) {
        if (rEdge) hamFetch();
        if (lEdge) hamState = HS_IDLE;
    }
}

// map a condition word to a color
static uint16_t condColor(const char* c) {
    if (strncasecmp(c, "Good", 4) == 0)     return LIME;
    if (strncasecmp(c, "Fair", 4) == 0)     return LIME_PALE;
    if (strncasecmp(c, "Poor", 4) == 0)     return PURPLE_LIGHT;
    if (strncasecmp(c, "Closed", 6) == 0)   return TEAL_DARK;
    return TEAL_LIGHT;
}

static void drawBands() {
    fb.setTextDatum(TL_DATUM);
    // column headers
    fb.setTextColor(TEAL_DARK);
    fb.drawString("BAND", 8, 28, 1);
    fb.setTextDatum(MC_DATUM);
    fb.drawString("DAY", 104, 32, 1);
    fb.drawString("NIGHT", 144, 32, 1);

    int y = 46;
    for (int i = 0; i < 4; i++) {
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(hamBands[i].label, 8, y + 10, 2);

        // day pill
        uint16_t dc = condColor(hamBands[i].day);
        fb.fillRoundRect(84, y, 40, 20, 3, TEAL_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(dc);
        fb.drawString(hamBands[i].day, 104, y + 10, 1);

        // night pill
        uint16_t nc = condColor(hamBands[i].night);
        fb.fillRoundRect(126, y, 38, 20, 3, TEAL_DARK);
        fb.setTextColor(nc);
        fb.drawString(hamBands[i].night, 145, y + 10, 1);

        y += 26;
    }

    // MUF + a quick "best band" hint line
    fb.setTextDatum(ML_DATUM); fb.setTextColor(TEAL_LIGHT);
    char b[28];
    snprintf(b, sizeof(b), "MUF: %s", hamMuf[0] ? hamMuf : "--");
    fb.drawString(b, 8, y + 6, 1);

    fb.setTextColor(TEAL_DARK);
    fb.drawString("Good=open  Poor=weak", 8, y + 22, 1);
}

static void drawSolar() {
    fb.setTextDatum(TL_DATUM);
    int y = 30;
    auto row = [&](const char* k, const char* v, uint16_t col) {
        fb.setTextColor(TEAL_LIGHT); fb.drawString(k, 8, y, 1);
        fb.setTextDatum(TR_DATUM); fb.setTextColor(col);
        fb.drawString(v[0] ? v : "--", W - 8, y, 1);
        fb.setTextDatum(TL_DATUM);
        y += 22;
    };
    row("Solar Flux (SFI)", hamFlux, LIME);
    row("Sunspots (SN)", hamSunspots, LIME);
    row("K-index", hamKindex, condColor("Fair"));
    row("A-index", hamAindex, LIME_PALE);
    row("X-ray", hamXray, LIME_PALE);
    row("Aurora", hamAurora, PURPLE_LIGHT);
    row("Solar Wind", hamSolarWind, TEAL_LIGHT);
    row("MUF", hamMuf, LIME_PALE);

    y += 6;
    fb.setTextColor(TEAL_DARK);
    if (hamUpdated[0]) {
        fb.drawString("Updated:", 8, y, 1); y += 14;
        fb.drawString(hamUpdated, 8, y, 1);
    }
}

void hamDraw() {
    fb.fillSprite(TEAL_DEEP);

    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("HAM RADIO", 4, 9, 1);
    if (hamState == HS_OK) {
        fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString(hamPage == 0 ? "bands" : "solar", W - 4, 9, 1);
    }

    if (hamState == HS_IDLE) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString("HF Propagation", W/2, 70, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("Which bands are open", W/2, 95, 1);
        fb.setTextColor(LIME);
        fb.drawString("press to load", W/2, 140, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("data: hamqsl.com (N0NBH)", W/2, 200, 1);
        fb.drawString("WiFi from Weather setup", W/2, 216, 1);
    } else if (hamState == HS_FETCHING) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString("fetching...", W/2, H/2, 2);
    } else if (hamState == HS_ERR) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("couldn't load", W/2, 90, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString(hamErr, W/2, 120, 1);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("R: retry   L: back", W/2, H - 30, 1);
    } else { // HS_OK
        if (hamPage == 0) drawBands();
        else              drawSolar();

        fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(hamPage == 0 ? "L: solar" : "L: bands", 4, H - 7, 1);
        fb.setTextDatum(MR_DATUM); fb.drawString("R: refresh", W - 4, H - 7, 1);
    }

    fb.pushSprite(0, 0);
}
