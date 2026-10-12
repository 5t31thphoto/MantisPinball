// setup_mode.cpp - Central on-demand config portal.
//
// Same proven AP-portal pattern as the miner/weather/lightning setup screens:
// the device hosts a WiFi access point, the user connects and opens the form in
// a browser, edits the shared settings, and saves. Writes to the same NVS keys
// the other modes read, so this is just one more place to set them - the
// existing per-mode portals still work.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "setup_mode.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170, H = 320;
static constexpr const char* AP_SSID = "MantisSetup";
static constexpr const char* NVS_MINER = "btcminer";
static constexpr const char* NVS_GAME  = "pinball";

static WebServer* suSrv = nullptr;
static DNSServer* suDns = nullptr;
static bool  suOn = false;
static char  suIp[20] = "192.168.4.1";
static bool  suSaved = false;
static uint32_t suSavedAt = 0;

static char v_ssid[40], v_pass[40], v_btc[80], v_bch[80], v_dgb[80], v_zip[12];
static int  v_tz = -420;   // minutes east of UTC (default -7h = Mountain)

static void suLoad() {
    v_ssid[0]=v_pass[0]=v_btc[0]=v_bch[0]=v_dgb[0]=v_zip[0]='\0';
    { Preferences p; if (p.begin(NVS_MINER, true)) {
        p.getString("ssid", v_ssid, sizeof(v_ssid));
        p.getString("pass", v_pass, sizeof(v_pass));
        p.getString("btc_addr", v_btc, sizeof(v_btc));
        p.getString("bch_addr", v_bch, sizeof(v_bch));
        p.getString("dgb_addr", v_dgb, sizeof(v_dgb));
        p.end();
    } }
    { Preferences p; if (p.begin(NVS_GAME, true)) {
        p.getString("wx_zip", v_zip, sizeof(v_zip));
        v_tz = p.getInt("tz_off", -420);
        p.end();
    } }
}

static void suSave() {
    { Preferences p; if (p.begin(NVS_MINER, false)) {
        if (v_ssid[0]) p.putString("ssid", v_ssid);
        if (v_pass[0]) p.putString("pass", v_pass);
        p.putString("btc_addr", v_btc);
        p.putString("bch_addr", v_bch);
        p.putString("dgb_addr", v_dgb);
        p.end();
    } }
    { Preferences p; if (p.begin(NVS_GAME, false)) {
        if (v_zip[0]) p.putString("wx_zip", v_zip);
        p.putInt("tz_off", v_tz);
        p.end();
    } }
}

static String suHtml() {
    String h = F("<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<style>body{font-family:sans-serif;background:#101018;color:#e8e8e8;margin:0;padding:16px}"
                 "h2{color:#7CFFDC}label{display:block;margin-top:10px;font-size:14px;color:#aac}"
                 "input,select{width:100%;box-sizing:border-box;padding:8px;margin-top:3px;border-radius:6px;border:1px solid #334;background:#1a1a26;color:#fff}"
                 "button{margin-top:18px;width:100%;padding:12px;background:#5D005D;color:#fff;border:0;border-radius:8px;font-size:16px}"
                 ".sec{border-top:1px solid #334;margin-top:16px;padding-top:6px}</style></head><body>"
                 "<h2>Mantis Setup</h2><form method=POST action=/save>");
    h += F("<div class=sec><b>WiFi</b></div>");
    h += "<label>Network (SSID)</label><input name=ssid value='" + String(v_ssid) + "'>";
    h += "<label>Password</label><input name=pass type=password value='" + String(v_pass) + "'>";
    h += F("<div class=sec><b>Crypto wallets</b></div>");
    h += "<label>BTC address</label><input name=btc value='" + String(v_btc) + "'>";
    h += "<label>BCH address</label><input name=bch value='" + String(v_bch) + "'>";
    h += "<label>DGB address</label><input name=dgb value='" + String(v_dgb) + "'>";
    h += F("<div class=sec><b>Location & time</b></div>");
    h += "<label>ZIP code</label><input name=zip value='" + String(v_zip) + "'>";
    h += F("<label>Timezone (UTC offset)</label><select name=tz>");
    static const int offs[] = {-720,-660,-600,-540,-480,-420,-360,-300,-240,-180,-120,-60,0,60,120,180,240,300,330,480,540,600};
    for (unsigned i=0;i<sizeof(offs)/sizeof(offs[0]);i++) {
        int o=offs[i]; char lbl[16];
        snprintf(lbl,sizeof(lbl),"UTC%+d:%02d", o/60, abs(o%60));
        h += "<option value=" + String(o) + (o==v_tz?" selected":"") + ">" + String(lbl) + "</option>";
    }
    h += F("</select>");
    h += F("<button type=submit>Save</button></form></body></html>");
    return h;
}

static void suHandleRoot() { suSrv->send(200, "text/html", suHtml()); }
static void suHandleSave() {
    if (suSrv->hasArg("ssid")) strncpy(v_ssid, suSrv->arg("ssid").c_str(), sizeof(v_ssid)-1);
    if (suSrv->hasArg("pass")) strncpy(v_pass, suSrv->arg("pass").c_str(), sizeof(v_pass)-1);
    if (suSrv->hasArg("btc"))  strncpy(v_btc,  suSrv->arg("btc").c_str(),  sizeof(v_btc)-1);
    if (suSrv->hasArg("bch"))  strncpy(v_bch,  suSrv->arg("bch").c_str(),  sizeof(v_bch)-1);
    if (suSrv->hasArg("dgb"))  strncpy(v_dgb,  suSrv->arg("dgb").c_str(),  sizeof(v_dgb)-1);
    if (suSrv->hasArg("zip"))  strncpy(v_zip,  suSrv->arg("zip").c_str(),  sizeof(v_zip)-1);
    if (suSrv->hasArg("tz"))   v_tz = suSrv->arg("tz").toInt();
    suSave();
    suSaved = true; suSavedAt = millis();
    suSrv->send(200, "text/html",
        F("<html><body style='font-family:sans-serif;background:#101018;color:#7CFFDC;padding:20px'>"
          "<h2>Saved!</h2><p>You can close this page and leave setup on the device.</p></body></html>"));
}

static void suStart() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    IPAddress ip = WiFi.softAPIP();
    snprintf(suIp, sizeof(suIp), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    suDns = new DNSServer();
    suDns->start(53, "*", ip);
    suSrv = new WebServer(80);
    suSrv->on("/", suHandleRoot);
    suSrv->on("/save", HTTP_POST, suHandleSave);
    suSrv->onNotFound(suHandleRoot);
    suSrv->begin();
    suOn = true;
}

static void suStop() {
    if (suSrv) { suSrv->stop(); delete suSrv; suSrv = nullptr; }
    if (suDns) { suDns->stop(); delete suDns; suDns = nullptr; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    suOn = false;
}

void setupModeEnter() {
    fb.setRotation(0);
    suLoad();
    suSaved = false;
    suStart();
}

void setupModeTick() {
    if (suOn) {
        if (suDns) suDns->processNextRequest();
        if (suSrv) suSrv->handleClient();
    }
}

void setupModeDraw() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 20, PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("SETUP", W/2, 10, 2);

    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
    int y = 32;
    fb.drawString("1. On your phone, join", 8, y, 1); y += 14;
    fb.setTextColor(LIME);
    fb.drawString("   WiFi:", 8, y, 1);
    fb.setTextColor(LIME_PALE); fb.drawString(AP_SSID, 52, y, 1); y += 18;
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("2. Open a browser to", 8, y, 1); y += 14;
    fb.setTextColor(LIME_PALE); fb.drawString(suIp, 20, y, 1); y += 20;
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("3. Edit & Save:", 8, y, 1); y += 14;
    fb.setTextColor(TEAL_DARK);
    fb.drawString(" - WiFi credentials", 8, y, 1); y += 12;
    fb.drawString(" - Crypto wallets", 8, y, 1); y += 12;
    fb.drawString(" - ZIP code", 8, y, 1); y += 12;
    fb.drawString(" - Timezone", 8, y, 1); y += 20;

    if (suSaved) {
        fb.fillRoundRect(8, y, W-16, 30, 5, PURPLE_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString("Settings saved!", W/2, y+15, 2);
    }

    fb.fillRect(0, H-14, W, 14, PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("dbl-tap both = menu", W/2, H-7, 1);

    fb.pushSprite(0, 0);
}

void setupModeShutdown() {
    suStop();
}
