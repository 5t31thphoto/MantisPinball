// ============================================================================
//  btc_miner.cpp
//
//  Solo Bitcoin lottery miner mode.
//
//  This file implements the entire mining stack:
//
//    - NVS configuration (WiFi creds, BTC payout address, pool overrides)
//    - WiFi station connect with timeout and graceful failure
//    - Captive portal access point with a setup form when unconfigured
//    - Stratum v1 client (text JSON-RPC over TCP)
//    - Job pickup, midstate computation, hash task pinned to Core 0
//    - Share submission and difficulty-aware reel-spin events
//    - Optional DigiByte SHA-256 fallback pool when BTC pool is down
//    - State machine for setup -> connecting -> mining -> error states
//
//  The whole thing only ever runs while the user is sitting in MINER mode.
//  When the user exits via double-click, minerShutdown() tears everything
//  down cleanly so the rest of the firmware (pinball, etc) sees no side
//  effects.
//
//  Slot-reel near-miss heuristic for share events lives in btc_ui.cpp.
// ============================================================================

#include "btc_miner.h"
#include "btc_miner_internal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "miner/nerdSHA256plus.h"
#include "miner/sha256m.h"
#include "miner/model/block.h"
#include "miner/model/target.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <climits>

// ----------------------------------------------------------------------------
//  Default pool endpoints
//
//  Primary: solo Bitcoin lottery (massive jackpot, astronomical odds)
//  Fallback: DigiByte SHA-256 solo (small reward, real chance of a win)
//
//  Both are Stratum v1 and use the same hash algorithm, so the only thing
//  that changes between them is the host/port and the wallet address.
// ----------------------------------------------------------------------------
// Pool defaults - all verified to accept low-hashrate ESP32-class miners.
//
// BTC SOLO:  public-pool.io — purpose-built for NerdMiner/ESP32, ultra-low diff
// BTC POOL:  btc.hmpool.io port 3337 — HashedMax Unity Pool, PPLNS proportional
//            payouts, ESP32/NerdMiner specific port, start diff 0.01, 10K sat min
// BCH SOLO:  bitcoincash.nerdpool.xyz — NMMiner official, low-diff BCH solo
//            Alternative: pool.bchsolo.com:3333
// DGB SOLO:  dgb-stratum.solominer.net — NMMiner official DGB reference pool
#define DEFAULT_BTC_POOL_HOST    "public-pool.io"
#define DEFAULT_BTC_POOL_PORT    21496
#define DEFAULT_BTCP_POOL_HOST   "btc.hmpool.io"    // HashedMax - PPLNS proportional
#define DEFAULT_BTCP_POOL_PORT   3337               // ESP32/NerdMiner specific port
#define DEFAULT_BCH_POOL_HOST    "bitcoincash.nerdpool.xyz"
#define DEFAULT_BCH_POOL_PORT    7771
#define DEFAULT_DGB_SOLO_HOST    "dgb-stratum.solominer.net"
#define DEFAULT_DGB_SOLO_PORT    3333

// Stratum authorize passwords - all use "x" (standard anonymous).
// HashedMax honors suggest_difficulty, so no d= hack needed.
#define STRATUM_PASS_BTC      "x"
#define STRATUM_PASS_BTCP     "x"
#define STRATUM_PASS_BCH      "x"
#define STRATUM_PASS_DGB_SOLO "x"
// Keep old names compiling
#define STRATUM_PASS_DGBP     "x"
#define DEFAULT_DGBP_POOL_HOST DEFAULT_BTCP_POOL_HOST
#define DEFAULT_DGBP_POOL_PORT DEFAULT_BTCP_POOL_PORT

// Block rewards for jackpot calculation
static const float BLOCK_REWARD_BTC  = 3.125f;
static const float BLOCK_REWARD_BCH  = 3.125f;
static const float BLOCK_REWARD_DGB  = 665.0f;  // approximate current reward

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define POOL_CONNECT_TIMEOUT_MS 8000
#define STRATUM_RX_BUFFER_SIZE  4096
#define STRATUM_MAX_LINE_SIZE   2048
#define HASH_TASK_STACK         16384     // bigger stack - nerdSHA256 uses real stack
#define HASH_BATCH              4096      // hashes per chunk before yielding

#define NVS_NAMESPACE           "btcminer"

// ----------------------------------------------------------------------------
//  Module state (single instance - only one miner mode active at a time)
// ----------------------------------------------------------------------------
MinerState g_miner;

static Preferences   prefs;
static WiFiClient    poolClient;
static DNSServer     dnsServer;
static WebServer     webServer(80);
static TaskHandle_t  hashTaskHandle = nullptr;
static SemaphoreHandle_t jobMutex   = nullptr;

static volatile bool stopMinerSignal = false;
static volatile bool jobReady        = false;

// Current Stratum job. Guarded by jobMutex when written from the network
// thread / read from the hash task.
struct StratumJob {
    std::string job_id;
    std::string prevhash;
    std::string coinb1;
    std::string coinb2;
    std::vector<std::string> merkle_branch;
    std::string version;
    std::string nbits;
    std::string ntime;
    bool        clean_jobs;

    std::string extranonce1;
    int         extranonce2_size;
    std::string extranonce2;        // chosen randomly per job

    double      pool_difficulty;    // shares accepted at this diff
    Block       block;              // assembled 80-byte header
    uint8_t     target[32];         // 256-bit target for block solve

    bool        valid;
} g_job;

static uint64_t stratumMsgId  = 1;
static bool     subscribed    = false;
static bool     authorized    = false;
static uint32_t poolFailCount = 0;

// Forward declarations
static void hashTaskFn(void *arg);
static void buildJob();
static void stratumSubscribe();
static void stratumAuthorize();
static void stratumSuggestDifficulty();
static void stratumSubmitShare(const std::string &job_id,
                               const std::string &extranonce2,
                               const std::string &ntime,
                               uint32_t nonce);
static void stratumProcessLine(const char *line);
static bool poolConnect();
static void enterState(MinerPhase next);
static void loadConfig();
static void saveConfig();
static void portalStart();
static void portalStop();
static void wifiStaConnect();
static void wifiStaStop();
static const char* phaseLabel(MinerPhase p);
static std::string randomHex(size_t bytes);

// ----------------------------------------------------------------------------
//  NVS configuration
// ----------------------------------------------------------------------------
static void loadConfig() {
    prefs.begin(NVS_NAMESPACE, /*readonly*/ true);
    // Read each value into a named String first so its c_str() lifetime is
    // unambiguous; then copy into the std::string config field. (Avoids any
    // surprises around expression-temporary lifetimes on the ESP32 build.)
    String s_ssid     = prefs.getString("ssid",       "");
    String s_pass     = prefs.getString("pass",       "");
    String s_btc      = prefs.getString("btc_addr",   "");
    String s_bch      = prefs.getString("bch_addr",   "");
    String s_dgb      = prefs.getString("dgb_addr",   "");
    String s_btchost  = prefs.getString("btc_host",   DEFAULT_BTC_POOL_HOST);
    String s_btcphost = prefs.getString("btcp_host",  DEFAULT_BTCP_POOL_HOST);
    String s_bchhost  = prefs.getString("bch_host",   DEFAULT_BCH_POOL_HOST);
    String s_dgbhost  = prefs.getString("dgb_host",   DEFAULT_DGB_SOLO_HOST);
    g_miner.cfg.wifi_ssid     = s_ssid.c_str();
    g_miner.cfg.wifi_password = s_pass.c_str();
    g_miner.cfg.btc_address   = s_btc.c_str();
    g_miner.cfg.bch_address   = s_bch.c_str();
    g_miner.cfg.dgb_address   = s_dgb.c_str();
    g_miner.cfg.btc_host      = s_btchost.c_str();
    g_miner.cfg.btcp_host     = s_btcphost.c_str();
    g_miner.cfg.bch_host      = s_bchhost.c_str();
    g_miner.cfg.dgb_host      = s_dgbhost.c_str();
    g_miner.cfg.dgbp_host     = s_btcphost.c_str(); // alias
    g_miner.cfg.btc_port      = prefs.getUShort("btc_port",  DEFAULT_BTC_POOL_PORT);
    g_miner.cfg.btcp_port     = prefs.getUShort("btcp_port", DEFAULT_BTCP_POOL_PORT);
    g_miner.cfg.bch_port      = prefs.getUShort("bch_port",  DEFAULT_BCH_POOL_PORT);
    g_miner.cfg.dgb_port      = prefs.getUShort("dgb_port",  DEFAULT_DGB_SOLO_PORT);
    g_miner.cfg.dgbp_port     = g_miner.cfg.btcp_port; // alias
    g_miner.cfg.use_dgb_fallback = prefs.getBool("dgb_on",  true);
    g_miner.stats.all_time_best_diff = prefs.getDouble("best",   0.0);
    g_miner.stats.blocks_found       = prefs.getULong ("blocks", 0);
    prefs.end();
}

static void saveConfig() {
    prefs.begin(NVS_NAMESPACE, /*readonly*/ false);
    prefs.putString("ssid",      g_miner.cfg.wifi_ssid.c_str());
    prefs.putString("pass",      g_miner.cfg.wifi_password.c_str());
    prefs.putString("btc_addr",  g_miner.cfg.btc_address.c_str());
    prefs.putString("bch_addr",  g_miner.cfg.bch_address.c_str());
    prefs.putString("dgb_addr",  g_miner.cfg.dgb_address.c_str());
    prefs.putString("btc_host",  g_miner.cfg.btc_host.c_str());
    prefs.putUShort("btc_port",  g_miner.cfg.btc_port);
    prefs.putString("btcp_host", g_miner.cfg.btcp_host.c_str());
    prefs.putUShort("btcp_port", g_miner.cfg.btcp_port);
    prefs.putString("bch_host",  g_miner.cfg.bch_host.c_str());
    prefs.putUShort("bch_port",  g_miner.cfg.bch_port);
    prefs.putString("dgb_host",  g_miner.cfg.dgb_host.c_str());
    prefs.putUShort("dgb_port",  g_miner.cfg.dgb_port);
    prefs.putBool  ("dgb_on",    g_miner.cfg.use_dgb_fallback);
    prefs.end();
}

static void saveStats() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putDouble("best",   g_miner.stats.all_time_best_diff);
    prefs.putULong ("blocks", g_miner.stats.blocks_found);
    prefs.end();
}

bool minerConfigComplete(const MinerConfig &c) {
    // Minimum requirements: WiFi creds AND at least one payout address.
    if (c.wifi_ssid.empty() || c.wifi_password.empty()) return false;
    if (c.btc_address.empty() && c.bch_address.empty() && c.dgb_address.empty()) return false;
    return true;
}

// ----------------------------------------------------------------------------
//  Random hex helper (used for extranonce2)
// ----------------------------------------------------------------------------
static std::string randomHex(size_t bytes) {
    static const char H[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t v = (uint8_t)(esp_random() & 0xFF);
        s += H[v >> 4];
        s += H[v & 0xF];
    }
    return s;
}

// ----------------------------------------------------------------------------
//  Hex / endian helpers (kept local so this module is self-contained)
// ----------------------------------------------------------------------------
static inline uint8_t hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void hexToBytes(const char *hex, uint8_t *out, size_t outLen) {
    for (size_t i = 0; i < outLen; ++i) {
        out[i] = (hexNib(hex[i * 2]) << 4) | hexNib(hex[i * 2 + 1]);
    }
}

static std::string bytesToHex(const uint8_t *b, size_t n) {
    static const char H[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += H[b[i] >> 4];
        s += H[b[i] & 0xF];
    }
    return s;
}

// Reverse bytes in-place in 4-byte words (Stratum endian flip)
static void flipWords(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint8_t t0 = data[i], t1 = data[i + 1];
        data[i]     = data[i + 3];
        data[i + 1] = data[i + 2];
        data[i + 2] = t1;
        data[i + 3] = t0;
    }
}

// littleEndian compare for the 32-byte hash vs 32-byte target
static int cmpLE(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = n; i-- > 0; ) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

// Compute share difficulty from a 32-byte hash, same math as LeafMiner.
// TRUEDIFFONE / value-as-little-endian-256.
static const double TRUEDIFFONE = 26959535291011309493156476344723991336010898738574164086137773096960.0;
static const double B192 = 6277101735386680763835789423207666416102355444464034512896.0;
static const double B128 = 340282366920938463463374607431768211456.0;
static const double B64  = 18446744073709551616.0;

static double diffFromHash(const uint8_t *h) {
    double d = 0;
    const uint64_t *q;
    q = (const uint64_t *)(h + 24); d += (double)*q * B192;
    q = (const uint64_t *)(h + 16); d += (double)*q * B128;
    q = (const uint64_t *)(h + 8);  d += (double)*q * B64;
    q = (const uint64_t *)(h + 0);  d += (double)*q;
    if (d <= 0) return 0;
    return TRUEDIFFONE / d;
}

// ----------------------------------------------------------------------------
//  State helpers
// ----------------------------------------------------------------------------
static const char* phaseLabel(MinerPhase p) {
    switch (p) {
        case MINER_OFF:        return "OFF";
        case MINER_SETUP:      return "SETUP NEEDED";
        case MINER_WIFI_CONN:  return "WIFI";
        case MINER_POOL_CONN:  return "CONNECTING";
        case MINER_SUBSCRIBE:  return "SUBSCRIBING";
        case MINER_AUTHORIZE:  return "AUTHORIZING";
        case MINER_MINING:     return "MINING";
        case MINER_ERROR:      return "RECONNECT";
    }
    return "?";
}

static void enterState(MinerPhase next) {
    g_miner.phase = next;
    g_miner.phaseEnteredAt = millis();
    snprintf(g_miner.status_line, sizeof(g_miner.status_line), "%s", phaseLabel(next));
}

// ----------------------------------------------------------------------------
//  Captive portal
// ----------------------------------------------------------------------------
static const char SETUP_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport"
content="width=device-width,initial-scale=1"><title>Pinball Miner Setup</title>
<style>
body{background:#007373;color:#fff;font-family:system-ui;margin:0;padding:24px;}
h1{color:#00ff00;font-weight:900;letter-spacing:.05em;}
.card{background:rgba(0,0,0,.2);border:2px solid #5D005D;border-radius:12px;
  padding:20px;margin:12px 0;}
label{display:block;font-size:13px;margin:10px 0 4px;color:#a8ffd8;}
input[type=text],input[type=password],input[type=number]{
  width:100%;box-sizing:border-box;padding:9px;border-radius:6px;border:0;
  background:#003939;color:#fff;font-family:monospace;}
input[type=submit]{background:#00ff00;color:#000;font-weight:900;padding:14px;
  border:0;border-radius:6px;width:100%;font-size:16px;cursor:pointer;
  letter-spacing:.06em;margin-top:16px;}
.hint{color:#a8ffd8;font-size:12px;margin-top:4px;}
.warn{color:#ffd060;font-size:12px;}
</style></head><body>
<h1>PINBALL S3 MINER</h1>
<form method="POST" action="/save">
<div class="card">
<h3>WiFi</h3>
<label>Network name (SSID)</label>
<input type="text" name="ssid" value="%SSID%" required>
<label>Password</label>
<input type="password" name="pass" value="%PASS%">
</div>
<div class="card">
<h3>BTC Solo (lottery)</h3>
<label>Bitcoin address (bc1... or 1... or 3...)</label>
<input type="text" name="btc" value="%BTC%" placeholder="bc1q...">
<div class="hint">Native SegWit (bc1q...) recommended. Jackpot wins go here.</div>
<label>Pool host</label>
<input type="text" name="btchost" value="%BTCHOST%">
<label>Pool port</label>
<input type="number" name="btcport" value="%BTCPORT%">
</div>
<div class="card">
<h3>BCH Solo (realistic lottery)</h3>
<label>Bitcoin Cash address (q... or bitcoincash:q...)</label>
<input type="text" name="bch" value="%BCH%" placeholder="q...">
<div class="hint">BCH is 400x easier than BTC. pool.bchsolo.com verified for ESP32. Win ~$350+.</div>
<label>Pool host (default: pool.bchsolo.com)</label>
<input type="text" name="bchhost" value="%BCHHOST%">
<label>Pool port</label>
<input type="number" name="bchport" value="%BCHPORT%">
</div>
<div class="card">
<h3>DGB Solo</h3>
<label>DigiByte address (D... or dgb1...)</label>
<input type="text" name="dgb" value="%DGB%" placeholder="dgb1q...">
<div class="hint">dgb-stratum.solominer.net — verified ESP32 low-diff pool.</div>
<label>DGB solo pool host</label>
<input type="text" name="dgbhost" value="%DGBHOST%">
<label>DGB solo pool port</label>
<input type="number" name="dgbport" value="%DGBPORT%">
</div>
<div class="card">
<h3>BTC Proportional Pool (HashedMax)</h3>
<div class="hint">Uses your BTC address above. PPLNS payouts, 10K sat minimum. Right-hold to switch from BTC solo.</div>
<label>Pool host (default: btc.hmpool.io)</label>
<input type="text" name="btcphost" value="%BTCPHOST%">
<label>Pool port (3337 = ESP32/NerdMiner port)</label>
<input type="number" name="btcpport" value="%BTCPPORT%">
</div>
<input type="submit" value="SAVE  &amp;  START MINING">
<p class="warn">Bad addresses won't crash the device but no wins can be paid.
Double-check before saving.</p>
</form></body></html>
)HTML";

static std::string htmlEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
    return s;
}

static void handlePortalRoot() {
    std::string html = SETUP_HTML;
    html = replaceAll(html, "%SSID%",     htmlEscape(g_miner.cfg.wifi_ssid));
    html = replaceAll(html, "%PASS%",     htmlEscape(g_miner.cfg.wifi_password));
    html = replaceAll(html, "%BTC%",      htmlEscape(g_miner.cfg.btc_address));
    html = replaceAll(html, "%BCH%",      htmlEscape(g_miner.cfg.bch_address));
    html = replaceAll(html, "%DGB%",      htmlEscape(g_miner.cfg.dgb_address));
    html = replaceAll(html, "%BTCHOST%",  htmlEscape(g_miner.cfg.btc_host));
    html = replaceAll(html, "%BCHHOST%",  htmlEscape(g_miner.cfg.bch_host));
    html = replaceAll(html, "%DGBHOST%",  htmlEscape(g_miner.cfg.dgb_host));
    html = replaceAll(html, "%BTCPHOST%", htmlEscape(g_miner.cfg.btcp_host));
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_miner.cfg.btc_port);
    html = replaceAll(html, "%BTCPORT%", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_miner.cfg.bch_port);
    html = replaceAll(html, "%BCHPORT%", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_miner.cfg.dgb_port);
    html = replaceAll(html, "%DGBPORT%", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_miner.cfg.btcp_port);
    html = replaceAll(html, "%BTCPPORT%", buf);
    webServer.send(200, "text/html", html.c_str());
}

static void handlePortalSave() {
    if (webServer.hasArg("ssid"))     { String v = webServer.arg("ssid");     g_miner.cfg.wifi_ssid     = v.c_str(); }
    if (webServer.hasArg("pass"))     { String v = webServer.arg("pass");     g_miner.cfg.wifi_password = v.c_str(); }
    if (webServer.hasArg("btc"))      { String v = webServer.arg("btc");      g_miner.cfg.btc_address   = v.c_str(); }
    if (webServer.hasArg("bch"))      { String v = webServer.arg("bch");      g_miner.cfg.bch_address   = v.c_str(); }
    if (webServer.hasArg("dgb"))      { String v = webServer.arg("dgb");      g_miner.cfg.dgb_address   = v.c_str(); }
    if (webServer.hasArg("btchost"))  { String v = webServer.arg("btchost");  g_miner.cfg.btc_host      = v.c_str(); }
    if (webServer.hasArg("bchhost"))  { String v = webServer.arg("bchhost");  g_miner.cfg.bch_host      = v.c_str(); }
    if (webServer.hasArg("dgbhost"))  { String v = webServer.arg("dgbhost");  g_miner.cfg.dgb_host      = v.c_str(); }
    if (webServer.hasArg("btcphost")) { String v = webServer.arg("btcphost"); g_miner.cfg.btcp_host = v.c_str(); g_miner.cfg.dgbp_host = v.c_str(); }
    if (webServer.hasArg("btcport"))  g_miner.cfg.btc_port  = (uint16_t)atoi(webServer.arg("btcport").c_str());
    if (webServer.hasArg("bchport"))  g_miner.cfg.bch_port  = (uint16_t)atoi(webServer.arg("bchport").c_str());
    if (webServer.hasArg("dgbport"))  g_miner.cfg.dgb_port  = (uint16_t)atoi(webServer.arg("dgbport").c_str());
    if (webServer.hasArg("btcpport")) { g_miner.cfg.btcp_port = (uint16_t)atoi(webServer.arg("btcpport").c_str()); g_miner.cfg.dgbp_port = g_miner.cfg.btcp_port; }
    saveConfig();
    webServer.send(200, "text/html",
        "<html><body style='background:#007373;color:#00ff00;font-family:system-ui;"
        "text-align:center;padding-top:80px;'><h1>SAVED</h1>"
        "<p style='color:#fff'>Device is connecting to WiFi...</p></body></html>");
    g_miner.requestReconnect = true;
}

static void portalStart() {
    if (g_miner.portalActive) return;
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_AP);
    char apName[32];
    uint16_t suffix = (uint16_t)(esp_random() & 0xFFFF);
    snprintf(apName, sizeof(apName), "pinball-miner-%04X", suffix);
    g_miner.ap_ssid = apName;
    WiFi.softAP(apName);
    IPAddress ip = WiFi.softAPIP();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    g_miner.ap_ip = buf;
    dnsServer.start(53, "*", ip);
    webServer.on("/",     HTTP_GET,  handlePortalRoot);
    webServer.on("/save", HTTP_POST, handlePortalSave);
    webServer.onNotFound([]() { handlePortalRoot(); });
    webServer.begin();
    g_miner.portalActive = true;
}

static void portalStop() {
    if (!g_miner.portalActive) return;
    webServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    g_miner.portalActive = false;
}

static void portalTick() {
    if (!g_miner.portalActive) return;
    dnsServer.processNextRequest();
    webServer.handleClient();
}

// ----------------------------------------------------------------------------
//  WiFi station
// ----------------------------------------------------------------------------
static void wifiStaConnect() {
    portalStop();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);
    delay(50);
    WiFi.begin(g_miner.cfg.wifi_ssid.c_str(), g_miner.cfg.wifi_password.c_str());
    enterState(MINER_WIFI_CONN);
}

static void wifiStaStop() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

// ----------------------------------------------------------------------------
//  Stratum client - sync WiFiClient, line-delimited JSON-RPC
// ----------------------------------------------------------------------------
static char rxBuf[STRATUM_RX_BUFFER_SIZE];
static size_t rxLen = 0;

static bool poolConnect() {
    const char *host;
    uint16_t port;
    switch (g_miner.activePool) {
        case POOL_BTC_POOL:
            host = g_miner.cfg.btcp_host.c_str();
            port = g_miner.cfg.btcp_port;
            break;
        case POOL_BCH:
            host = g_miner.cfg.bch_host.c_str();
            port = g_miner.cfg.bch_port;
            break;
        case POOL_DGB_SOLO:
            host = g_miner.cfg.dgb_host.c_str();
            port = g_miner.cfg.dgb_port;
            break;
        default: // POOL_BTC
            host = g_miner.cfg.btc_host.c_str();
            port = g_miner.cfg.btc_port;
            break;
    }
    poolClient.stop();
    poolClient.setTimeout(POOL_CONNECT_TIMEOUT_MS / 1000);
    rxLen = 0;
    subscribed = false;
    authorized = false;
    stratumMsgId = 1;
    if (!poolClient.connect(host, port)) return false;
    poolClient.setNoDelay(true);
    return true;
}

static bool stratumSendLine(const char *line) {
    if (!poolClient.connected()) return false;
    size_t n = strlen(line);
    if (poolClient.write((const uint8_t *)line, n) != n) return false;
    if (poolClient.write((const uint8_t *)"\n", 1) != 1) return false;
    return true;
}

static void stratumSubscribe() {
    char payload[200];
    snprintf(payload, sizeof(payload),
        "{\"id\":%llu,\"method\":\"mining.subscribe\",\"params\":[\"pinball-s3/1.0\"]}",
        (unsigned long long)stratumMsgId++);
    stratumSendLine(payload);
    enterState(MINER_SUBSCRIBE);
}

static void stratumAuthorize() {
    const char *addr;
    switch (g_miner.activePool) {
        case POOL_BCH:
            addr = g_miner.cfg.bch_address.c_str(); break;
        case POOL_DGB_SOLO:
            addr = g_miner.cfg.dgb_address.c_str(); break;
        default: // BTC solo and BTC pool both use BTC address
            addr = g_miner.cfg.btc_address.c_str(); break;
    }
    if (!addr || !*addr) addr = "x";    // pool will reject but we won't crash
    const char *pass;
    switch (g_miner.activePool) {
        case POOL_BTC_POOL: pass = STRATUM_PASS_BTCP;     break;
        case POOL_BCH:      pass = STRATUM_PASS_BCH;      break;
        case POOL_DGB_SOLO: pass = STRATUM_PASS_DGB_SOLO; break;
        default:            pass = STRATUM_PASS_BTC;      break;
    }
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"id\":%llu,\"method\":\"mining.authorize\",\"params\":[\"%s.pinball\",\"%s\"]}",
        (unsigned long long)stratumMsgId++, addr, pass);
    stratumSendLine(payload);
    enterState(MINER_AUTHORIZE);
}

static void stratumSuggestDifficulty() {
    // Suggest a very low difficulty so we get shares at our ~30 KH/s hashrate.
    // At 30 KH/s, with the quick-filter firing ~1/65536, we get a candidate
    // every ~2 seconds. We want pool diff low enough that those candidates
    // actually meet the threshold. 0.0001 is achievable; most pools will
    // honour it or set their own floor (usually 0.001 for small miners).
    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"id\":%llu,\"method\":\"mining.suggest_difficulty\",\"params\":[0.00001]}",
        (unsigned long long)stratumMsgId++);
    stratumSendLine(payload);
}

static void stratumSubmitShare(const std::string &job_id,
                               const std::string &extranonce2,
                               const std::string &ntime,
                               uint32_t nonce) {
    const char *addr;
    switch (g_miner.activePool) {
        case POOL_BCH:
            addr = g_miner.cfg.bch_address.c_str(); break;
        case POOL_DGB_SOLO:
            addr = g_miner.cfg.dgb_address.c_str(); break;
        default: // BTC solo and BTC pool both use BTC address
            addr = g_miner.cfg.btc_address.c_str(); break;
    }
    char payload[384];
    snprintf(payload, sizeof(payload),
        "{\"id\":%llu,\"method\":\"mining.submit\","
        "\"params\":[\"%s.pinball\",\"%s\",\"%s\",\"%s\",\"%08x\"]}",
        (unsigned long long)stratumMsgId++, addr,
        job_id.c_str(), extranonce2.c_str(), ntime.c_str(), nonce);
    stratumSendLine(payload);
    g_miner.stats.shares_submitted++;
}

// Pull a string field from a flat JSON object substring. Very forgiving;
// we don't need a full parser for the small set of fields we read.
static bool jsonStr(const char *src, const char *key, std::string &out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    out.assign(p, end - p);
    return true;
}

static bool jsonBool(const char *src, const char *key, bool &out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0)  { out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { out = false; return true; }
    return false;
}

static bool jsonInt(const char *src, const char *key, long &out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *endp;
    out = strtol(p, &endp, 10);
    return endp != p;
}

// Parse "params":[...] for mining.notify. The format is:
//   [job_id, prevhash, coinb1, coinb2, [merkle...], version, nbits, ntime, clean]
static bool parseNotifyParams(const char *line, StratumJob &j) {
    const char *p = strstr(line, "\"params\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    auto skipWS = [&]() { while (*p == ' ' || *p == '\t') p++; };
    auto readStr = [&](std::string &out) {
        skipWS();
        if (*p != '"') return false;
        p++;
        const char *e = strchr(p, '"');
        if (!e) return false;
        out.assign(p, e - p);
        p = e + 1;
        return true;
    };
    auto eatComma = [&]() {
        skipWS();
        if (*p == ',') p++;
    };
    if (!readStr(j.job_id))   return false; eatComma();
    if (!readStr(j.prevhash)) return false; eatComma();
    if (!readStr(j.coinb1))   return false; eatComma();
    if (!readStr(j.coinb2))   return false; eatComma();
    // merkle branch array
    skipWS();
    if (*p != '[') return false;
    p++;
    j.merkle_branch.clear();
    while (*p && *p != ']') {
        skipWS();
        if (*p == ']') break;
        std::string m;
        if (!readStr(m)) return false;
        j.merkle_branch.push_back(m);
        skipWS();
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
    eatComma();
    if (!readStr(j.version)) return false; eatComma();
    if (!readStr(j.nbits))   return false; eatComma();
    if (!readStr(j.ntime))   return false; eatComma();
    skipWS();
    j.clean_jobs = (strncmp(p, "true", 4) == 0);
    return true;
}

// Parse "result":[...] from a mining.subscribe response. Real-world pool
// variations:
//   [[["mining.set_difficulty","x"],["mining.notify","y"]], extranonce1, ex2_size]
//   [[["mining.notify","y"]],                                extranonce1, ex2_size]
//   [null,                                                   extranonce1, ex2_size]
//
// We tolerate all three by stepping past the first comma-separated element
// (whether array or null), then reading the extranonce1 string + size.
static bool parseSubscribeResult(const char *line) {
    const char *p = strstr(line, "\"result\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;   // past the outer [
    // Skip whitespace
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '[') {
        // First element is the subscriptions sub-array; walk to matching ]
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            if (depth > 0) p++;
        }
        if (*p != ']') return false;
        p++;
    } else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        p += 4;     // some pools send null
    } else {
        return false;
    }
    // Now expect: , "extranonce1" , ex2_size
    while (*p == ' ' || *p == ',') p++;
    if (*p != '"') return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    g_job.extranonce1.assign(p, e - p);
    p = e + 1;
    while (*p == ' ' || *p == ',') p++;
    char *endp;
    long sz = strtol(p, &endp, 10);
    if (endp == p) return false;
    g_job.extranonce2_size = (int)sz;
    return true;
}

// Build the 80-byte block header from the current job + a fresh extranonce2,
// compute the SHA256 midstate, store the target.
static void buildJob() {
    if (jobMutex) xSemaphoreTake(jobMutex, portMAX_DELAY);

    // Random extranonce2
    g_job.extranonce2 = randomHex(g_job.extranonce2_size);

    // coinbase = coinb1 + extranonce1 + extranonce2 + coinb2  (all hex)
    std::string coinbase = g_job.coinb1 + g_job.extranonce1 + g_job.extranonce2 + g_job.coinb2;
    size_t cbLen = coinbase.size() / 2;
    std::vector<uint8_t> cbBytes(cbLen);
    hexToBytes(coinbase.c_str(), cbBytes.data(), cbLen);

    // Double-SHA256 the coinbase -> 32 bytes
    uint8_t coinbaseHash[32];
    sha256_double(cbBytes.data(), cbLen, coinbaseHash);

    // Walk the merkle branch: hash = sha256d(hash || branch_i)
    uint8_t cur[32];
    memcpy(cur, coinbaseHash, 32);
    uint8_t merklePair[64];
    for (const auto &branchHex : g_job.merkle_branch) {
        memcpy(merklePair, cur, 32);
        hexToBytes(branchHex.c_str(), merklePair + 32, 32);
        sha256_double(merklePair, 64, cur);
    }
    // cur is now the merkle root in display-byte order. Build the header.

    uint8_t versionBytes[4];
    hexToBytes(g_job.version.c_str(), versionBytes, 4);
    uint32_t versionLE = ((uint32_t)versionBytes[3]      )
                      | ((uint32_t)versionBytes[2] <<  8)
                      | ((uint32_t)versionBytes[1] << 16)
                      | ((uint32_t)versionBytes[0] << 24);

    uint8_t prevBytes[32];
    hexToBytes(g_job.prevhash.c_str(), prevBytes, 32);

    uint8_t ntimeBytes[4];
    hexToBytes(g_job.ntime.c_str(), ntimeBytes, 4);
    uint32_t ntimeLE = ((uint32_t)ntimeBytes[3]      )
                    | ((uint32_t)ntimeBytes[2] <<  8)
                    | ((uint32_t)ntimeBytes[1] << 16)
                    | ((uint32_t)ntimeBytes[0] << 24);

    uint8_t nbitsBytes[4];
    hexToBytes(g_job.nbits.c_str(), nbitsBytes, 4);
    uint32_t nbitsLE = ((uint32_t)nbitsBytes[3]      )
                    | ((uint32_t)nbitsBytes[2] <<  8)
                    | ((uint32_t)nbitsBytes[1] << 16)
                    | ((uint32_t)nbitsBytes[0] << 24);

    // Block struct is packed: version(4) + prev(32) + merkle(32) + ntime(4) + nbits(4) + nonce(4)
    g_job.block.version = versionLE;
    // The Stratum prevhash arrives "as displayed" but Bitcoin block headers
    // want it byte-swapped per 32-bit word.
    memcpy(g_job.block.previous_block, prevBytes, 32);
    flipWords(g_job.block.previous_block, 32);
    memcpy(g_job.block.merkle_root, cur, 32);
    g_job.block.ntime = ntimeLE;
    g_job.block.nbits = nbitsLE;
    g_job.block.nonce = 0;

    // Calculate target from nbits
    Target t;
    t.calculate(g_job.nbits.c_str());
    memcpy(g_job.target, t.value, 32);

    g_job.valid = true;
    jobReady    = true;

    if (jobMutex) xSemaphoreGive(jobMutex);
}

// Process a single newline-terminated JSON message from the pool.
static void stratumProcessLine(const char *line) {
    if (!line || !*line) return;

    // Stratum server messages come in two flavors:
    //   - server-pushed notifications: have "method":"<name>" (no result)
    //   - RPC responses to our requests: have "result":<value> (no method)
    //
    // Critical: the SUBSCRIBE response embeds the string "mining.notify"
    // inside its result array, so we cannot dispatch on substring alone.
    // Look for the actual "method" field as a token.
    bool hasMethod = (strstr(line, "\"method\"") != nullptr);
    bool hasResult = (strstr(line, "\"result\"") != nullptr);

    // ===== Server-pushed notifications =====
    if (hasMethod) {
        if (strstr(line, "\"mining.notify\"")) {
            StratumJob tmp;
            tmp.extranonce1      = g_job.extranonce1;
            tmp.extranonce2_size = g_job.extranonce2_size;
            tmp.pool_difficulty  = g_job.pool_difficulty;
            if (parseNotifyParams(line, tmp)) {
                if (jobMutex) xSemaphoreTake(jobMutex, portMAX_DELAY);
                g_job.job_id        = tmp.job_id;
                g_job.prevhash      = tmp.prevhash;
                g_job.coinb1        = tmp.coinb1;
                g_job.coinb2        = tmp.coinb2;
                g_job.merkle_branch = tmp.merkle_branch;
                g_job.version       = tmp.version;
                g_job.nbits         = tmp.nbits;
                g_job.ntime         = tmp.ntime;
                g_job.clean_jobs    = tmp.clean_jobs;
                if (jobMutex) xSemaphoreGive(jobMutex);
                buildJob();
                g_miner.stats.jobs_received++;
                g_miner.uiSpinTrigger = true;
            }
            return;
        }
        if (strstr(line, "\"mining.set_difficulty\"")) {
            const char *p = strstr(line, "\"params\"");
            if (p) {
                p = strchr(p, '[');
                if (p) {
                    char *endp;
                    double d = strtod(p + 1, &endp);
                    if (endp != p + 1) {
                        g_job.pool_difficulty = d;
                    }
                }
            }
            return;
        }
        // Other server-pushed methods (set_extranonce, etc.) - ignore.
        return;
    }

    if (!hasResult) return;     // neither method nor result, nothing to do

    // ===== RPC responses =====

    // Response to mining.subscribe is an array result.
    if (!subscribed && strchr(line, '[')) {
        if (parseSubscribeResult(line)) {
            subscribed = true;
            stratumAuthorize();
            stratumSuggestDifficulty();
        }
        return;
    }

    // Response to mining.authorize (result is true/false)
    if (!authorized) {
        bool ok = false;
        if (jsonBool(line, "result", ok) && ok) {
            authorized = true;
            enterState(MINER_MINING);
        } else if (jsonBool(line, "result", ok) && !ok) {
            snprintf(g_miner.status_line, sizeof(g_miner.status_line),
                     "AUTH FAILED - check address");
            enterState(MINER_ERROR);
        }
        return;
    }

    // Share submit result - pools vary on spacing: "result":true vs "result": true
    bool resultTrue  = strstr(line, "\"result\":true")  || strstr(line, "\"result\": true");
    bool resultFalse = strstr(line, "\"result\":false") || strstr(line, "\"result\": false");
    if (resultTrue)  g_miner.stats.shares_accepted++;
    if (resultFalse) g_miner.stats.shares_rejected++;
}

static void stratumPoll() {
    if (!poolClient.connected()) return;
    while (poolClient.available()) {
        char c = (char)poolClient.read();
        if (c == '\n' || c == '\r') {
            if (rxLen > 0) {
                rxBuf[rxLen] = '\0';
                stratumProcessLine(rxBuf);
                rxLen = 0;
            }
        } else if (rxLen < STRATUM_MAX_LINE_SIZE - 1) {
            rxBuf[rxLen++] = c;
        } else {
            rxLen = 0; // overflow guard
        }
    }
}

// ----------------------------------------------------------------------------
//  Hash task - runs pinned to Core 0 to keep the UI on Core 1 buttery
// ----------------------------------------------------------------------------
struct ShareCandidate {
    bool         present;
    std::string  job_id;
    std::string  extranonce2;
    std::string  ntime;
    uint32_t     nonce;
    double       diff;
    bool         block_solved;
};
static volatile bool         pendingShare = false;
static ShareCandidate        pendingShareData;
static portMUX_TYPE          shareMux = portMUX_INITIALIZER_UNLOCKED;

static void hashTaskFn(void *arg) {
    (void)arg;
    nerdSHA256_context ctx;
    uint8_t hash[32];

    while (!stopMinerSignal) {
        if (!jobReady || g_miner.phase != MINER_MINING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Snapshot the job inputs while protected
        Block localBlock;
        std::string localJobId;
        std::string localExtranonce2;
        std::string localNtime;
        uint8_t     localTarget[32];
        double      localPoolDiff;

        if (jobMutex) xSemaphoreTake(jobMutex, portMAX_DELAY);
        if (!g_job.valid) {
            if (jobMutex) xSemaphoreGive(jobMutex);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        localBlock        = g_job.block;
        localJobId        = g_job.job_id;
        localExtranonce2  = g_job.extranonce2;
        localNtime        = g_job.ntime;
        memcpy(localTarget, g_job.target, 32);
        localPoolDiff     = g_job.pool_difficulty;
        if (jobMutex) xSemaphoreGive(jobMutex);

        // Compute midstate from the first 64 bytes of the header
        uint8_t headerCopy[80];
        memcpy(headerCopy, &localBlock, 80);
        nerd_mids(&ctx, headerCopy);

        // Iterate nonces in this batch
        uint32_t startNonce = (uint32_t)esp_random();
        uint8_t  *tail = headerCopy + 64;  // last 16 bytes (merkle tail + ntime + nbits + nonce)
        uint32_t  endNonce = startNonce + HASH_BATCH;
        for (uint32_t n = startNonce; n != endNonce && !stopMinerSignal; ++n) {
            // Write the nonce into the tail (last 4 bytes)
            tail[12] = (uint8_t)(n      );
            tail[13] = (uint8_t)(n >>  8);
            tail[14] = (uint8_t)(n >> 16);
            tail[15] = (uint8_t)(n >> 24);

            if (nerd_sha256d(&ctx, tail, hash)) {
                // Quick filter passed - now compute actual difficulty
                double d = diffFromHash(hash);
                g_miner.stats.hashes_total += 1;

                // Track best-share-this-job dopamine numbers
                if (d > g_miner.stats.session_best_diff) {
                    g_miner.stats.session_best_diff = d;
                }
                if (d > g_miner.stats.all_time_best_diff) {
                    g_miner.stats.all_time_best_diff = d;
                    saveStats();
                }
                g_miner.stats.last_share_diff = d;

                // Block-target solve check
                bool block_solved = (cmpLE(hash, localTarget, 32) < 0);

                // Spin the reels on every quick-filter pass.
                g_miner.uiSpinTrigger = true;
                g_miner.stats.spins_total++;

                // Gate on pool_difficulty exactly as the pool sent it.
                // localPoolDiff is the value from the pool's mining.set_difficulty
                // message — never overridden or floored by us. If the pool hasn't
                // sent one yet (0.0), submit anyway to catch early shares.
                if (localPoolDiff > 0.0 && d < localPoolDiff) {
                    continue;  // skip — hash doesn't meet what pool requires
                }
                // Copy strings BEFORE entering the critical section.
                // std::string assignment calls malloc which is unsafe with
                // interrupts disabled (portENTER_CRITICAL on ESP32).
                pendingShareData.job_id      = localJobId;
                pendingShareData.extranonce2 = localExtranonce2;
                pendingShareData.ntime       = localNtime;
                // Now take the spinlock only for the POD fields + flag
                portENTER_CRITICAL(&shareMux);
                pendingShareData.present     = true;
                pendingShareData.nonce       = n;
                pendingShareData.diff        = d;
                pendingShareData.block_solved = block_solved;
                pendingShare = true;
                portEXIT_CRITICAL(&shareMux);
            }
        }
        // Update hashrate counter
        g_miner.stats.hashes_since_rate += HASH_BATCH;

        // Yield briefly between batches for watchdog/idle housekeeping
        vTaskDelay(1);
    }

    hashTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// ----------------------------------------------------------------------------
//  Public API: enter / tick / draw / shutdown
// ----------------------------------------------------------------------------
extern bool fbReady;   // main.cpp - cleared while miner owns the framebuffer

void minerEnter() {
    if (g_miner.running) return;
    fbReady = false;   // miner will take over fb; prevent main.cpp drawing into it
    g_miner.running = true;
    stopMinerSignal = false;
    jobReady        = false;

    loadConfig();

    memset(&g_miner.stats, 0, sizeof(g_miner.stats));
    g_miner.stats.session_started_at = millis();
    // Restore persisted best
    prefs.begin(NVS_NAMESPACE, true);
    g_miner.stats.all_time_best_diff = prefs.getDouble("best",   0.0);
    g_miner.stats.blocks_found       = prefs.getULong ("blocks", 0);
    prefs.end();

    g_miner.activePool = POOL_BTC;
    g_miner.btcConfigured = !g_miner.cfg.btc_address.empty();
    g_miner.dgbConfigured = !g_miner.cfg.dgb_address.empty();

    if (!jobMutex) jobMutex = xSemaphoreCreateMutex();

    if (!minerConfigComplete(g_miner.cfg)) {
        portalStart();
        enterState(MINER_SETUP);
    } else {
        wifiStaConnect();
    }

    // Start the hash task pinned to Core 0
    if (!hashTaskHandle) {
        xTaskCreatePinnedToCore(hashTaskFn, "miner_hash", HASH_TASK_STACK,
                                nullptr, 1, &hashTaskHandle, 0);
    }
}

extern void minerUiShutdown();   // implemented in btc_ui.cpp

void minerShutdown() {
    if (!g_miner.running) return;
    stopMinerSignal = true;
    // Allow hash task to drain
    uint32_t waitStart = millis();
    while (hashTaskHandle && (millis() - waitStart) < 500) {
        delay(10);
    }
    if (poolClient.connected()) poolClient.stop();
    portalStop();
    wifiStaStop();
    minerUiShutdown();   // restores portrait sprite; sets fbReady-able state
    fbReady     = true;  // portrait sprite is live again after minerUiShutdown
    jobReady    = false;
    g_job.valid = false;
    g_miner.running = false;
    g_miner.portalActive = false;
    enterState(MINER_OFF);
}

bool minerIsRunning() { return g_miner.running; }

// ----------------------------------------------------------------------------
//  Price + jackpot + odds fetch (HTTPS, non-blocking cadence)
// ----------------------------------------------------------------------------
// Network difficulty constants for odds calculation.
// These are approximate and will drift; good enough for a display ticker.
// BTC: ~90 trillion, BCH: ~450 billion, DGB SHA256: ~680 million
static const double NET_DIFF_BTC = 90.0e12;
static const double NET_DIFF_BCH = 450.0e9;
static const double NET_DIFF_DGB = 680.0e6;

static void fetchPriceAndOdds() {
    if (g_miner.priceFetching) return;
    g_miner.priceFetching = true;

    WiFiClientSecure https;
    https.setInsecure();
    https.setTimeout(6);

    if (!https.connect("api.coingecko.com", 443)) {
        g_miner.priceFetching = false;
        return;
    }

    const char *ids;
    switch (g_miner.activePool) {
        case POOL_BCH:      ids = "bitcoin-cash"; break;
        case POOL_DGB_SOLO: ids = "digibyte";     break;
        default:            ids = "bitcoin";      break;
    }

    char req[256];
    snprintf(req, sizeof(req),
        "GET /api/v3/simple/price?ids=%s&vs_currencies=usd HTTP/1.1\r\n"
        "Host: api.coingecko.com\r\n"
        "User-Agent: pinball-s3/1.0\r\n"
        "Connection: close\r\n\r\n", ids);
    https.print(req);

    uint32_t t0 = millis();
    String body = "";
    bool inBody = false;
    while (https.connected() || https.available()) {
        if (millis() - t0 > 6000) break;
        if (!https.available()) { delay(10); continue; }
        String line = https.readStringUntil('\n');
        if (!inBody) {
            if (line == "\r" || line.length() == 0) inBody = true;
        } else {
            body += line;
            if (body.length() > 200) break;
        }
    }
    https.stop();

    // Response body: {"bitcoin":{"usd":105000}} - find "usd": and parse number
    int idx = body.indexOf("\"usd\":");
    if (idx >= 0) {
        float price = body.substring(idx + 6).toFloat();
        if (price > 0.0f) {
            g_miner.coin_price_usd = price;
            float reward;
            double netDiff;
            switch (g_miner.activePool) {
                case POOL_BCH:
                    reward = BLOCK_REWARD_BCH; netDiff = NET_DIFF_BCH; break;
                case POOL_DGB_SOLO:
                    reward = BLOCK_REWARD_DGB; netDiff = NET_DIFF_DGB; break;
                default:
                    reward = BLOCK_REWARD_BTC; netDiff = NET_DIFF_BTC; break;
            }
            g_miner.jackpot_usd = reward * price;
            double hrHs = (double)g_miner.stats.hashrate_kHs * 1000.0;
            if (hrHs > 0.0) {
                // Correct formula: expected hashes = difficulty * 2^32
                // (SHA-256 difficulty is defined relative to 2^32 hash attempts)
                double expectedSec = (netDiff * 4294967296.0) / hrHs;
                g_miner.odds_days = (float)(expectedSec / 86400.0);
            }
            g_miner.priceLastFetch = millis();
        }
    }
    g_miner.priceFetching = false;
}


// Helper: immediately disconnect and reconnect to g_miner.activePool.
static void doPoolReconnect() {
    // Clear stale price data so the UI shows "syncing..." immediately
    // and fetchPriceAndOdds fires on the next tick for the new coin.
    g_miner.coin_price_usd = 0.0f;
    g_miner.jackpot_usd    = 0.0f;
    g_miner.odds_days      = 0.0f;
    g_miner.priceLastFetch = 0;
    stopMinerSignal = true;
    uint32_t t = millis();
    while (hashTaskHandle && (millis() - t) < 500) delay(10);
    stopMinerSignal = false;
    if (poolClient.connected()) poolClient.stop();
    subscribed = false;
    authorized = false;
    jobReady   = false;
    g_job.valid = false;
    rxLen = 0;

    // Reset per-session stats for the new coin (keep all-time best and blocks)
    uint64_t savedHashes = g_miner.stats.hashes_total;
    double   savedBest   = g_miner.stats.all_time_best_diff;
    uint32_t savedBlocks = g_miner.stats.blocks_found;
    memset(&g_miner.stats, 0, sizeof(g_miner.stats));
    g_miner.stats.session_started_at  = millis();
    g_miner.stats.all_time_best_diff  = savedBest;
    g_miner.stats.blocks_found        = savedBlocks;
    g_miner.stats.hashes_total        = savedHashes;
    g_miner.stats.last_rate_at        = millis();   // prevent first-tick division by huge elapsed

    // Restart the hash task - it self-deleted when stopMinerSignal fired
    if (!hashTaskHandle) {
        xTaskCreatePinnedToCore(hashTaskFn, "miner_hash", HASH_TASK_STACK,
                                nullptr, 1, &hashTaskHandle, 0);
    }

    if (WiFi.status() == WL_CONNECTED) {
        enterState(MINER_POOL_CONN);
    } else {
        wifiStaConnect();
    }
}

// Tick: handles input, state transitions, network polling. UI compose lives
// in btc_ui.cpp (kept separate so this file stays focused on protocol).
void minerTick() {
    if (!g_miner.running) return;

    extern bool btnL, btnR;

    // ===== RIGHT BUTTON - long press: cycle coin/mode =====
    // Hold RIGHT for 1.5s to advance to the next pool slot.
    // The UI draws a progress bar while the button is held (via g_miner.*).
    // After the hold commits, we finish the current job then reconnect.
    // ===== LEFT BUTTON - long press: reset setup confirmation =====
    // Hold LEFT for 1.5s to show the RESET SETUP MODE? confirmation overlay.
    // Hold LEFT again for 1.5s to confirm: wipes WiFi+wallet NVS and opens portal.
    {
        static constexpr uint32_t HOLD_MS = 1500;
        static uint32_t rHoldStart = 0;
        static uint32_t lHoldStart = 0;
        static bool rFired = false;
        static bool lFired = false;
        uint32_t now = millis();

        // --- RIGHT long press: coin cycle ---
        if (btnR && !btnL) {
            if (rHoldStart == 0) { rHoldStart = now; rFired = false; }
            g_miner.rightHoldMs = now - rHoldStart;
            if (!rFired && (now - rHoldStart) >= HOLD_MS) {
                rFired = true;
                // Advance to the next pool slot in the cycle
                int next = ((int)g_miner.activePool + 1) % POOL_COUNT;
                g_miner.nextPool = (ActivePool)next;
                g_miner.requestCoinSwitch = true;
                g_miner.coinSwitchPending = true;
                snprintf(g_miner.status_line, sizeof(g_miner.status_line),
                         "SWITCHING -> %s", POOL_LABELS[next]);
            }
        } else {
            rHoldStart = 0;
            rFired     = false;
            g_miner.rightHoldMs = 0;
        }

        // --- LEFT long press: reset setup ---
        if (btnL && !btnR) {
            if (lHoldStart == 0) { lHoldStart = now; lFired = false; }
            g_miner.leftHoldMs = now - lHoldStart;
            if (!lFired && (now - lHoldStart) >= HOLD_MS) {
                lFired = true;
                if (!g_miner.resetConfirmPending) {
                    // First long press: show the confirm overlay
                    g_miner.resetConfirmPending = true;
                    g_miner.resetConfirmShownAt = now;
                } else {
                    // Second long press while confirm is showing: execute reset
                    g_miner.resetConfirmPending = false;
                    stopMinerSignal = true;
                    uint32_t t2 = millis();
                    while (hashTaskHandle && (millis() - t2) < 300) delay(10);
                    stopMinerSignal = false;
                    if (poolClient.connected()) poolClient.stop();
                    wifiStaStop();
                    prefs.begin(NVS_NAMESPACE, false);
                    prefs.clear();
                    prefs.end();
                    loadConfig();
                    memset(&g_miner.stats, 0, sizeof(g_miner.stats));
                    g_miner.stats.session_started_at = millis();
                    g_miner.activePool = POOL_BTC;
                    portalStart();
                    enterState(MINER_SETUP);
                    return;
                }
            }
        } else {
            lHoldStart = 0;
            lFired     = false;
            g_miner.leftHoldMs = 0;
        }

        // Dismiss reset confirm if neither button pressed for 5s
        if (g_miner.resetConfirmPending &&
            (now - g_miner.resetConfirmShownAt) > 5000 &&
            !btnL) {
            g_miner.resetConfirmPending = false;
        }
    }

    // ===== COIN SWITCH: wait for job boundary then reconnect =====
    // We committed to switching after the next clean_jobs=true notify or
    // after 8 seconds max (so the user doesn't wait forever on a quiet pool).
    if (g_miner.coinSwitchPending && g_miner.phase == MINER_MINING) {
        static uint32_t switchWaitStart = 0;
        if (switchWaitStart == 0) switchWaitStart = millis();
        bool timeout = (millis() - switchWaitStart) > 8000;
        // jobReady going false briefly between jobs is the boundary signal;
        // or fall through on timeout
        if (!jobReady || timeout) {
            switchWaitStart = 0;
            g_miner.coinSwitchPending   = false;
            g_miner.requestCoinSwitch   = false;
            g_miner.activePool          = g_miner.nextPool;
            g_miner.btcConfigured  = !g_miner.cfg.btc_address.empty();
            g_miner.dgbConfigured  = !g_miner.cfg.dgb_address.empty();
            doPoolReconnect();
            return;
        }
    } else if (g_miner.coinSwitchPending && g_miner.phase != MINER_MINING) {
        // Not yet mining (still connecting) - just switch immediately
        g_miner.coinSwitchPending = false;
        g_miner.requestCoinSwitch = false;
        g_miner.activePool        = g_miner.nextPool;
        doPoolReconnect();
        return;
    }

    portalTick();

    // Handle a reconnect request from the captive portal save button
    if (g_miner.requestReconnect) {
        g_miner.requestReconnect = false;
        if (minerConfigComplete(g_miner.cfg)) {
            wifiStaConnect();
        }
    }

    // Submit any pending shares from the hash task
    if (pendingShare) {
        // Snapshot POD fields under the spinlock, then copy strings outside it.
        // std::string copy calls malloc which must not happen with interrupts off.
        ShareCandidate snap;
        snap.present = false;
        portENTER_CRITICAL(&shareMux);
        if (pendingShare) {
            snap.present      = pendingShareData.present;
            snap.nonce        = pendingShareData.nonce;
            snap.diff         = pendingShareData.diff;
            snap.block_solved = pendingShareData.block_solved;
            pendingShare             = false;
            pendingShareData.present = false;
        }
        portEXIT_CRITICAL(&shareMux);
        // Copy strings outside critical section (safe for malloc)
        if (snap.present) {
            snap.job_id      = pendingShareData.job_id;
            snap.extranonce2 = pendingShareData.extranonce2;
            snap.ntime       = pendingShareData.ntime;
        }
        if (snap.present && poolClient.connected() && authorized) {
            stratumSubmitShare(snap.job_id, snap.extranonce2, snap.ntime, snap.nonce);
            if (snap.block_solved) {
                g_miner.stats.blocks_found++;
                saveStats();
                g_miner.blockWinFlash = millis();
            }
        }
    }

    uint32_t now = millis();
    switch (g_miner.phase) {
        case MINER_OFF:
        case MINER_SETUP:
            break;

        case MINER_WIFI_CONN:
            if (WiFi.status() == WL_CONNECTED) {
                // Sync time from NTP so the desk clock shows real wall time
                // instead of session uptime. Non-blocking: configTime kicks
                // off the SNTP background task and we move on to the pool.
                configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
                enterState(MINER_POOL_CONN);
            } else if (now - g_miner.phaseEnteredAt > WIFI_CONNECT_TIMEOUT_MS) {
                // Couldn't join WiFi - bounce back to setup portal so the
                // user can fix their credentials.
                portalStart();
                enterState(MINER_SETUP);
            }
            break;

        case MINER_POOL_CONN:
            if (poolConnect()) {
                stratumSubscribe();
            } else {
                // Try alternate pool if BTC fails and DGB configured
                if (g_miner.activePool == POOL_BTC && g_miner.dgbConfigured
                    && g_miner.cfg.use_dgb_fallback) {
                    g_miner.activePool = POOL_DGB;
                    enterState(MINER_POOL_CONN);
                } else if (g_miner.activePool == POOL_DGB && g_miner.btcConfigured) {
                    g_miner.activePool = POOL_BTC;
                    enterState(MINER_ERROR);
                } else {
                    enterState(MINER_ERROR);
                }
                poolFailCount++;
            }
            break;

        case MINER_SUBSCRIBE:
        case MINER_AUTHORIZE:
        case MINER_MINING:
            stratumPoll();
            if (!poolClient.connected()) {
                // Dropped - reconnect
                enterState(MINER_ERROR);
            }
            break;

        case MINER_ERROR:
            // Wait a bit then retry
            if (now - g_miner.phaseEnteredAt > 5000) {
                if (WiFi.status() == WL_CONNECTED) {
                    enterState(MINER_POOL_CONN);
                } else {
                    wifiStaConnect();
                }
            }
            break;
    }

    // Update hashrate (KH/s) once per second
    if (now - g_miner.stats.last_rate_at > 1000) {
        uint32_t elapsed = now - g_miner.stats.last_rate_at;
        if (elapsed > 0) {
            g_miner.stats.hashrate_kHs =
                (float)g_miner.stats.hashes_since_rate / (float)elapsed;
            g_miner.stats.hashes_since_rate = 0;
            g_miner.stats.last_rate_at      = now;
        }
    }

    // Fetch coin price + recalculate jackpot/odds every 90 seconds.
    // Only fetch while actually mining (WiFi connected, pool up).
    // The HTTP call is synchronous but short (~200ms on good WiFi).
    if (g_miner.phase == MINER_MINING &&
        !g_miner.priceFetching &&
        (g_miner.priceLastFetch == 0 || (now - g_miner.priceLastFetch) > 90000)) {
        fetchPriceAndOdds();
    }
    // Recalculate odds whenever hashrate updates (no network needed)
    if (g_miner.stats.hashrate_kHs > 0.0f && g_miner.coin_price_usd > 0.0f) {
        double netDiff;
        switch (g_miner.activePool) {
            case POOL_BCH:      netDiff = NET_DIFF_BCH; break;
            case POOL_DGB_SOLO: netDiff = NET_DIFF_DGB; break;
            default:            netDiff = NET_DIFF_BTC; break;
        }
        double hrHs = (double)g_miner.stats.hashrate_kHs * 1000.0;
        if (hrHs > 0.0) {
            double expectedSec = (netDiff * 4294967296.0) / hrHs;
            g_miner.odds_days = (float)(expectedSec / 86400.0);
        }
    }
}
