// FILE: src/mantischat/mantischat_mode.cpp
// MANTIS CHAT - text chat on the MantisNow ESP-NOW mesh, so this board can talk
// to the Core2 and the other Mantis Chat devices.
//
// PROTOCOL (MantisNow v1, implemented to the published spec):
//   ESP-NOW broadcast FF:FF:FF:FF:FF:FF, no encryption, channel 1 locked
//   28-byte little-endian header: magic 'M', version 1, kind, flags,
//   srcNode, dstNode, pktId, seq, ttl(0), chanOrSub, callsign[8], payload
//   kinds used: TEXT 0x01 (channel chat + DMs to us), PING 0x02 / PONG 0x03
//   (who's online), ACK 0x05 (sent when a unicast to us asks for one).
//   Everything else - including FRAG, which TEXT (<=191 bytes) never needs -
//   is dropped silently, as the spec requires of unknown kinds.
//   Dedup: 32-entry ring of (srcNode, pktId), checked before dispatch.
//
// LEGACY: MantisOS <= 1.5.8 watches speak "MNW1" (197 bytes: magic, type
// 0 beacon / 1 group / 2 direct, callsign[12], text[180]), byte-identical to
// the 1337 watch's mantis_chat.cpp. Like the Core2 MantisChat reference we
// receive it, beacon MNW1 every 5 s so old watches list us, and send each
// group message in BOTH formats. The two never collide: MNW1's byte 1 is 'N',
// never version 1. A dual-format sender's message arrives twice (v1 + MNW1),
// so the same sender + same text inside 3 s is shown once.
//
// RADIO: brought up on Enter and torn down on Shutdown exactly like Versus
// (house pattern), plus the standard channel lock. The receive callback only
// copies the frame into a ring and returns; all parsing happens in the tick.
//
// INPUT (arcade keyboard): L = next key (hold to race through), R = press key,
// hold R = send. Pages: ABC / abc / 123+symbols. Space, backspace, send keys.
// The two-button exit chord is guarded.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include "mantischat_mode.h"
#include "../theme_colors.h"
#include "../chord_guard.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170, H = 320;

// ===========================================================================
//  MantisNow wire format
// ===========================================================================
static constexpr uint8_t  MN_MAGIC = 0x4D, MN_VERSION = 0x01, MN_CHANNEL = 1;
static constexpr uint8_t  K_TEXT = 0x01, K_PING = 0x02, K_PONG = 0x03, K_ACK = 0x05, K_WB = 0x40;
static constexpr uint8_t  FL_FRAG = 0x01, FL_ACKREQ = 0x02;
static constexpr uint32_t DST_ALL = 0xFFFFFFFFu;
static constexpr int      HDR = 28, MAX_FRAME = 250, MAX_TEXT = 191;
static const uint8_t      BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct __attribute__((packed)) MnHeader {
    uint8_t  magic, version, kind, flags;
    uint32_t srcNode, dstNode, pktId;
    uint16_t seq;
    uint8_t  ttl, chanOrSub;
    char     callsign[8];
};
static_assert(sizeof(MnHeader) == HDR, "MantisNow header must be 28 bytes");
struct __attribute__((packed)) MnwPacket {          // legacy MantisOS chat
    char magic[4]; uint8_t type; char callsign[12]; char text[180];
};
static_assert(sizeof(MnwPacket) == 197, "MNW1 must stay 197 bytes");
enum : uint8_t { MNW_BEACON = 0, MNW_GROUP = 1, MNW_DIRECT = 2 };
static uint32_t nodeFromMac(const uint8_t* m) {     // == ESP.getEfuseMac() & 0xFFFFFFFF
    return (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
}

// ===========================================================================
//  state (one heap block: taken on Enter, freed on Shutdown)
// ===========================================================================
static constexpr int RX_SLOTS = 8, LOG_N = 28, PEERS = 12, DEDUP = 32;
static constexpr int COMPOSE_MAX = 64;          // typing is slow; keep it chat-sized

struct RxSlot { volatile uint8_t full; uint8_t len; uint8_t mac[6]; uint8_t data[MAX_FRAME]; };
struct LogLine { char who[13]; char text[COMPOSE_MAX + 1]; uint8_t kind; };   // 0 them, 1 me, 2 DM, 3 system
struct Peer { uint32_t node; char call[13]; uint32_t seen; bool legacy; };
struct Recent { uint32_t node; uint32_t hash; uint32_t at; };
struct ChatState {
    RxSlot   rx[RX_SLOTS];
    LogLine  log[LOG_N];
    int      logHead, logCount;
    Peer     peers[PEERS];
    uint32_t dedupNode[DEDUP], dedupPkt[DEDUP];
    int      dedupHead;
    Recent   recent[12];                          // cross-format duplicate filter
    int      recentHead;
    char     compose[COMPOSE_MAX + 1];
    char     call[9];
};
static ChatState* S = nullptr;
static volatile uint8_t rxWrite = 0;            // producer: WiFi task
static uint8_t  rxRead = 0;                     // consumer: our tick
static bool     radioUp = false;
static uint32_t myNode = 0;
static uint16_t mySeq = 0;
static uint8_t  chatChan = 0;                   // 0 = "general"

// ---- receive: copy and return, nothing else (house rule) ------------------
static void mnOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
    if (!S || len < HDR || len > MAX_FRAME) return;
    bool v1 = data[0] == MN_MAGIC && data[1] == MN_VERSION;
    bool legacy = len >= (int)sizeof(MnwPacket) && !memcmp(data, "MNW1", 4);
    if (!v1 && !legacy) return;
    RxSlot& s = S->rx[rxWrite % RX_SLOTS];
    if (s.full) return;                          // ring full: drop (sender doesn't retry either)
    if (mac) memcpy(s.mac, mac, 6); else memset(s.mac, 0, 6);
    memcpy(s.data, data, (size_t)len);
    s.len = (uint8_t)len;
    s.full = 1;
    rxWrite = (uint8_t)(rxWrite + 1);
}

// ---- send -------------------------------------------------------------------
static void mnSend(uint8_t kind, uint32_t dst, uint8_t chan, const void* payload, int plen, uint8_t flags = 0) {
    if (!radioUp || plen < 0 || HDR + plen > MAX_FRAME) return;
    uint8_t buf[MAX_FRAME];
    MnHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = MN_MAGIC; h.version = MN_VERSION; h.kind = kind; h.flags = flags;
    h.srcNode = myNode; h.dstNode = dst; h.pktId = esp_random(); h.seq = ++mySeq;
    h.ttl = 0; h.chanOrSub = chan;
    memcpy(h.callsign, S->call, strnlen(S->call, 8));     // null-padded by the memset
    memcpy(buf, &h, HDR);
    if (plen) memcpy(buf + HDR, payload, (size_t)plen);
    esp_now_send(BCAST, buf, (size_t)(HDR + plen));
}

// ---- legacy MNW1 send (197 bytes, byte-identical to the 1337 watch) ---------
static uint32_t lastLegacyBeacon = 0;
static void mnwSend(uint8_t type, const char* text) {
    if (!radioUp) return;
    MnwPacket p; memset(&p, 0, sizeof(p));
    memcpy(p.magic, "MNW1", 4);
    p.type = type;
    strncpy(p.callsign, S->call, sizeof(p.callsign) - 1);
    if (text) strncpy(p.text, text, sizeof(p.text) - 1);
    esp_now_send(BCAST, (const uint8_t*)&p, sizeof(p));
}

// ---- cross-format duplicate filter ------------------------------------------
// A dual-format sender (Core2, another T-Display) sends each group message as
// v1 TEXT and as MNW1. Same sender + same text inside 3 s = one message.
static uint32_t textHash(const char* t) {
    uint32_t h = 2166136261u;
    for (; *t; ++t) { h ^= (uint8_t)*t; h *= 16777619u; }
    return h;
}
static bool seenRecently(uint32_t node, const char* text) {
    uint32_t h = textHash(text), now = millis();
    for (int i = 0; i < 12; ++i)
        if (S->recent[i].at && S->recent[i].node == node && S->recent[i].hash == h && now - S->recent[i].at < 3000) return true;
    Recent& r = S->recent[S->recentHead]; r.node = node; r.hash = h; r.at = now ? now : 1;
    S->recentHead = (S->recentHead + 1) % 12;
    return false;
}

// ---- radio up/down: Versus' pattern + MantisNow's channel lock --------------
static bool radioStart() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);              // the usual way to pin the channel
    esp_wifi_set_channel(MN_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    myNode = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFull);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(mnOnRecv));
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = 0; p.encrypt = false;            // 0 = whatever channel we're on (1)
    if (!esp_now_is_peer_exist(BCAST) && esp_now_add_peer(&p) != ESP_OK) return false;
    radioUp = true;
    return true;
}
static void radioStop() {
    if (radioUp) {
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        radioUp = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ---- log / peers / dedup ----------------------------------------------------
static void logAdd(const char* who, const char* text, uint8_t kind) {
    LogLine& l = S->log[S->logHead];
    strncpy(l.who, who, 12); l.who[12] = 0;
    int n = 0;
    for (const char* p = text; *p && n < COMPOSE_MAX; ++p)      // printable ASCII only
        l.text[n++] = (*p >= 32 && *p < 127) ? *p : '?';
    l.text[n] = 0;
    l.kind = kind;
    S->logHead = (S->logHead + 1) % LOG_N;
    if (S->logCount < LOG_N) S->logCount++;
}
static bool dedupSeen(uint32_t node, uint32_t pkt) {
    for (int i = 0; i < DEDUP; ++i)
        if (S->dedupNode[i] == node && S->dedupPkt[i] == pkt) return true;
    S->dedupNode[S->dedupHead] = node; S->dedupPkt[S->dedupHead] = pkt;
    S->dedupHead = (S->dedupHead + 1) % DEDUP;
    return false;
}
static void peerSeen(uint32_t node, const char* call, int callLen, bool legacy) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < PEERS; ++i) {
        if (S->peers[i].node == node) { slot = i; break; }
        if (S->peers[i].seen < S->peers[oldest].seen) oldest = i;
    }
    if (slot < 0) slot = oldest;
    Peer& p = S->peers[slot];
    p.node = node; strncpy(p.call, call, (size_t)callLen); p.call[callLen < 12 ? callLen : 12] = 0;
    if (legacy) p.legacy = true;
    uint32_t t = millis(); p.seen = t ? t : 1;          // 0 means "never seen"
}
static int peersOnline() {
    int n = 0; uint32_t now = millis();
    for (int i = 0; i < PEERS; ++i) if (S->peers[i].seen && now - S->peers[i].seen < 90000) ++n;
    return n;
}


// ===========================================================================
//  WHITEBOARD VIEWER (read-only) - MantisNow kind 0x40, WHITEBOARD.md
// ===========================================================================
// The Core2 board is 320 x 206. We show it scaled to 170 x 109 and never draw
// on it. Strokes are kept as compact records (<= 24 points, 8-bit view coords)
// in an 8 KB pool that exists ONLY while the board is on screen. Rules are the
// reference's: dedup (owner,sid,seq); DEL/CLEAR only touch the SENDER's own
// strokes; tombstones stop late SEGs; CLEAR's sid watermark; SYNC on open.
enum : uint8_t { WB_HELLO = 0, WB_SEG = 1, WB_DEL = 2, WB_CLEAR = 3, WB_SYNC = 4 };
static constexpr int WB_W = 320, WB_H = 206, VIEW_W = 170, VIEW_H = 109, VIEW_Y = 24;
static constexpr int WB_RECS = 128, WB_PTS = 24, WB_TOMBS = 64, WB_CLEARS = 8;
struct WbRec { uint32_t owner, sid; uint16_t seq, color; uint8_t width, n; uint8_t pts[WB_PTS * 2]; };
struct WbTomb { uint32_t owner, sid; };
struct WbClear { uint32_t owner, nextSid; };
struct Board {
    WbRec   rec[WB_RECS]; int nrec, head;
    WbTomb  tomb[WB_TOMBS]; int tombHead;
    WbClear clr[WB_CLEARS]; int clrHead;
};
static Board* B = nullptr;
static bool   boardView = false;
static uint32_t lastInk = 0;

static uint8_t vx(int16_t x) { int v = (int)x * VIEW_W / WB_W; return (uint8_t)(v < 0 ? 0 : v > VIEW_W - 1 ? VIEW_W - 1 : v); }
static uint8_t vy(int16_t y) { int v = (int)y * VIEW_W / WB_W; return (uint8_t)(v < 0 ? 0 : v > VIEW_H - 1 ? VIEW_H - 1 : v); }

static bool wbTombstoned(uint32_t owner, uint32_t sid) {
    for (int i = 0; i < WB_TOMBS; ++i) if (B->tomb[i].owner == owner && B->tomb[i].sid == sid && (owner | sid)) return true;
    for (int i = 0; i < WB_CLEARS; ++i) {                       // CLEAR watermark: same session, older sid
        const WbClear& c = B->clr[i];
        if (c.owner == owner && (c.owner | c.nextSid) && (sid >> 16) == (c.nextSid >> 16) && sid < c.nextSid) return true;
    }
    return false;
}
static void wbRemove(uint32_t owner, bool all, uint32_t sid) {
    int w = 0;
    for (int r = 0; r < B->nrec; ++r) {
        const WbRec& x = B->rec[r];
        bool kill = x.owner == owner && (all || x.sid == sid);
        if (!kill) { if (w != r) B->rec[w] = x; ++w; }
    }
    B->nrec = w; B->head = w % WB_RECS;
}
static void wbOnFrame(uint32_t owner, uint8_t sub, const uint8_t* pl, int plen) {
    if (!B) return;                                             // board not open: nothing kept
    lastInk = millis();
    if (sub == WB_SEG && plen >= 10) {
        uint32_t sid; uint16_t seq, color; memcpy(&sid, pl, 4); memcpy(&seq, pl + 4, 2); memcpy(&color, pl + 6, 2);
        uint8_t width = pl[8], n = pl[9];
        if (n < 1 || n > 48 || plen < 10 + 4 * n || wbTombstoned(owner, sid)) return;
        for (int r = 0; r < B->nrec; ++r) if (B->rec[r].owner == owner && B->rec[r].sid == sid && B->rec[r].seq == seq) return;
        WbRec rec; memset(&rec, 0, sizeof(rec));
        rec.owner = owner; rec.sid = sid; rec.seq = seq; rec.color = color;
        rec.width = (uint8_t)(width < 1 ? 1 : width > 32 ? 32 : width);
        int keep = n <= WB_PTS ? n : WB_PTS;                    // thin long chunks, keep both ends
        for (int k = 0; k < keep; ++k) {
            int src = keep == 1 ? 0 : (k * (n - 1)) / (keep - 1);
            int16_t x, y; memcpy(&x, pl + 10 + src * 4, 2); memcpy(&y, pl + 12 + src * 4, 2);
            rec.pts[k * 2] = vx(x); rec.pts[k * 2 + 1] = vy(y);
        }
        rec.n = (uint8_t)keep;
        if (B->nrec < WB_RECS) B->rec[B->nrec++] = rec;
        else { memmove(B->rec, B->rec + 1, sizeof(WbRec) * (WB_RECS - 1)); B->rec[WB_RECS - 1] = rec; }   // oldest falls off THIS board
    } else if (sub == WB_DEL && plen >= 4) {
        uint32_t sid; memcpy(&sid, pl, 4);
        wbRemove(owner, false, sid);
        B->tomb[B->tombHead] = { owner, sid }; B->tombHead = (B->tombHead + 1) % WB_TOMBS;
    } else if (sub == WB_CLEAR && plen >= 4) {
        uint32_t nextSid; memcpy(&nextSid, pl, 4);
        wbRemove(owner, true, 0);
        B->clr[B->clrHead] = { owner, nextSid }; B->clrHead = (B->clrHead + 1) % WB_CLEARS;
    }
    // HELLO / SYNC: nothing to keep; a viewer never replays (it owns no ink)
}
static void boardOpen() {
    if (!B) B = (Board*)malloc(sizeof(Board));
    if (!B) { logAdd("*", "not enough memory for the board", 3); return; }
    memset(B, 0, sizeof(Board));
    boardView = true; lastInk = 0;
    mnSend(K_WB, DST_ALL, WB_SYNC, nullptr, 0);                // ask every board to replay its ink
}
static void boardClose() {
    boardView = false;
    if (B) { free(B); B = nullptr; }
}
static uint16_t wbColor(uint16_t c) { return c; }               // RGB565 straight through
static void drawBoard() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0, 0, W, 18, PURPLE_DEEP);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME); fb.drawString("WHITEBOARD", 4, 9, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT); fb.drawString("view only", W - 4, 9, 1);
    fb.fillRect(0, VIEW_Y, VIEW_W, VIEW_H, LIME_PALE);          // the paper
    fb.drawRect(0, VIEW_Y - 1, VIEW_W, VIEW_H + 2, TEAL);
    if (B) for (int r = 0; r < B->nrec; ++r) {
        const WbRec& x = B->rec[r];
        int wv = (x.width * VIEW_W) / WB_W; if (wv < 1) wv = 1;
        uint16_t c = wbColor(x.color);
        if (x.n == 1) { fb.fillCircle(x.pts[0], VIEW_Y + x.pts[1], wv / 2 + (wv > 1), c); continue; }
        for (int k = 1; k < x.n; ++k) {
            int x0 = x.pts[(k-1)*2], y0 = VIEW_Y + x.pts[(k-1)*2+1], x1 = x.pts[k*2], y1 = VIEW_Y + x.pts[k*2+1];
            fb.drawLine(x0, y0, x1, y1, c);
            if (wv > 1) { fb.drawLine(x0 + 1, y0, x1 + 1, y1, c); fb.drawLine(x0, y0 + 1, x1, y1 + 1, c); }
            if (wv > 2) fb.fillCircle(x1, y1, wv / 2, c);
        }
    }
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
    char b[40];
    snprintf(b, sizeof(b), "%d strokes on the board", B ? B->nrec : 0);
    fb.drawString(b, W / 2, VIEW_Y + VIEW_H + 10, 1);
    fb.setTextColor(TEAL);
    fb.drawString("draw on a Core2 MantisChat", W / 2, VIEW_Y + VIEW_H + 26, 1);
    fb.drawString("and it shows up here live", W / 2, VIEW_Y + VIEW_H + 38, 1);
    fb.setTextDatum(BC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L or R: back to chat", W / 2, H - 4, 1);
}

// ---- dispatch one received frame ------------------------------------------------
static void handleLegacy(const uint8_t* mac, const uint8_t* d, int len) {
    if (len < (int)sizeof(MnwPacket)) return;
    MnwPacket p; memcpy(&p, d, sizeof(p));
    p.callsign[11] = 0; p.text[179] = 0;
    uint32_t node = nodeFromMac(mac);
    if (node == myNode) return;                               // our own echo
    peerSeen(node, p.callsign[0] ? p.callsign : "?", 11, true);
    if (p.type == MNW_GROUP || p.type == MNW_DIRECT) {
        if (!p.text[0] || seenRecently(node, p.text)) return;
        logAdd(p.callsign, p.text, p.type == MNW_DIRECT ? 2 : 0);
    }
}

static void handleFrame(const uint8_t* mac, const uint8_t* d, int len) {
    if (!memcmp(d, "MNW1", 4)) { handleLegacy(mac, d, len); return; }
    MnHeader h; memcpy(&h, d, HDR);
    if (h.srcNode == myNode) return;                          // our own echo
    if (dedupSeen(h.srcNode, h.pktId)) return;
    char call[9]; memcpy(call, h.callsign, 8); call[8] = 0;
    if (!call[0]) snprintf(call, sizeof(call), "%08lX", (unsigned long)h.srcNode);
    peerSeen(h.srcNode, call, 8, false);
    bool toMe = h.dstNode == myNode, toAll = h.dstNode == DST_ALL;
    if ((h.flags & FL_ACKREQ) && toMe) {                      // ACK only unicasts to us
        uint32_t acked = h.pktId;
        mnSend(K_ACK, h.srcNode, 0, &acked, 4);
    }
    if (h.flags & FL_FRAG) return;                            // TEXT never needs fragments
    const uint8_t* pl = d + HDR; int plen = len - HDR;
    switch (h.kind) {
    case K_TEXT: {
        if (!toMe && !(toAll && h.chanOrSub == chatChan)) return;
        char t[MAX_TEXT + 1]; int n = plen < MAX_TEXT ? plen : MAX_TEXT;
        memcpy(t, pl, (size_t)n); t[n] = 0;
        if (seenRecently(h.srcNode, t)) break;                // already shown via MNW1
        logAdd(call, t, toMe ? 2 : 0);
        break; }
    case K_PING: {
        // PONG's 8-byte reply token: the PING's pktId, then the pinger's node
        uint8_t tok[8]; memcpy(tok, &h.pktId, 4); memcpy(tok + 4, &h.srcNode, 4);
        mnSend(K_PONG, h.srcNode, 0, tok, 8);
        break; }
    case K_WB: wbOnFrame(h.srcNode, h.chanOrSub, pl, plen); break;
    default: break;                                           // PONG/ACK/others: peer noted above
    }
}

// ===========================================================================
//  arcade keyboard
// ===========================================================================
// Three pages of 30 keys (3 rows x 10). The last row's tail is the same on
// every page: PAGE, SPACE, DEL, SEND.
enum : char { KY_PAGE = 1, KY_SPACE = 2, KY_DEL = 3, KY_SEND = 4, KY_NAME = 5, KY_BOARD = 6 };
static const char PAGES[3][31] = {
    { 'A','B','C','D','E','F','G','H','I','J',
      'K','L','M','N','O','P','Q','R','S','T',
      'U','V','W','X','Y','Z',KY_PAGE,KY_SPACE,KY_DEL,KY_SEND, 0 },
    { 'a','b','c','d','e','f','g','h','i','j',
      'k','l','m','n','o','p','q','r','s','t',
      'u','v','w','x','y','z',KY_PAGE,KY_SPACE,KY_DEL,KY_SEND, 0 },
    { '1','2','3','4','5','6','7','8','9','0',
      '.',',','!','?','\'','-',':',')','(','@',
      '#','+','=','/',KY_BOARD,KY_NAME,KY_PAGE,KY_SPACE,KY_DEL,KY_SEND, 0 },
};
static constexpr int KEYS = 30, KCOLS = 10;
static constexpr uint32_t ENTER_GUARD = 250, HOLD_MS = 420, REPEAT_MS = 90;

enum Screen : uint8_t { SC_NAME = 0, SC_CHAT };
static Screen   scr = SC_CHAT;
static uint8_t  page = 0, keyIdx = 0;
static uint32_t enterAt = 0, lAt = 0, rAt = 0, lastRepeat = 0, lastPing = 0, frameNo = 0;
static bool     lDown = false, rDown = false, lHeld = false, rHeld = false;
static char     nameBuf[9];
static uint8_t  logScroll = 0;

static void saveCall() {
    Preferences p;
    if (p.begin("mchat", false)) { p.putString("call", S->call); p.end(); }
}
static void loadCall() {
    S->call[0] = 0;
    Preferences p;
    if (p.begin("mchat", true)) {
        String c = p.getString("call", "");
        strncpy(S->call, c.c_str(), 8); S->call[8] = 0;
        p.end();
    }
}

static char* activeBuf(int& maxLen) {
    if (scr == SC_NAME) { maxLen = 8; return nameBuf; }
    maxLen = COMPOSE_MAX; return S->compose;
}

static void sendCompose() {
    int n = (int)strlen(S->compose);
    if (!n) return;
    mnSend(K_TEXT, DST_ALL, chatChan, S->compose, n);
    if (chatChan == 0) mnwSend(MNW_GROUP, S->compose);       // older watches hear it too
    logAdd(S->call, S->compose, 1);
    S->compose[0] = 0;
    logScroll = 0;
}

static void pressKey() {
    char k = PAGES[page][keyIdx];
    int maxLen; char* buf = activeBuf(maxLen);
    int n = (int)strlen(buf);
    switch (k) {
    case KY_PAGE:  page = (uint8_t)((page + 1) % 3); if (keyIdx >= KEYS) keyIdx = 0; break;
    case KY_SPACE: if (n < maxLen && n > 0) { buf[n] = ' '; buf[n + 1] = 0; } break;
    case KY_DEL:   if (n) buf[n - 1] = 0; break;
    case KY_BOARD: if (scr == SC_CHAT) boardOpen(); break;
    case KY_NAME:  if (scr == SC_CHAT) { strncpy(nameBuf, S->call, 8); nameBuf[8] = 0; scr = SC_NAME; page = 0; keyIdx = 0; } break;
    case KY_SEND:
        if (scr == SC_NAME) {
            if (nameBuf[0]) { strncpy(S->call, nameBuf, 8); S->call[8] = 0; saveCall();   // a discrete event: one write
                              logAdd("*", "you are on the mesh", 3); scr = SC_CHAT; page = 0; keyIdx = 0; }
        } else sendCompose();
        break;
    default:       if (n < maxLen) { buf[n] = k; buf[n + 1] = 0; } break;
    }
}

// ===========================================================================
//  tick
// ===========================================================================
void mantischatTick() {
    chordGuardTick();
    if (!S) return;
    ++frameNo;
    uint32_t now = millis();

    // drain the receive ring
    for (int guard = 0; guard < RX_SLOTS; ++guard) {
        RxSlot& s = S->rx[rxRead % RX_SLOTS];
        if (!s.full) break;
        handleFrame(s.mac, s.data, s.len);
        s.full = 0;
        rxRead = (uint8_t)(rxRead + 1);
    }
    // who's online: ping now and then
    if (radioUp && S->call[0] && now - lastPing > 30000) { lastPing = now; mnSend(K_PING, DST_ALL, 0, nullptr, 0); }
    if (radioUp && S->call[0] && now - lastLegacyBeacon >= 5000) { lastLegacyBeacon = now; mnwSend(MNW_BEACON, ""); }   // old watches list us

    if (now - enterAt < ENTER_GUARD) return;
    if (boardView) { if (chordLEdge() || chordREdge()) boardClose(); return; }
    // LEFT: tap = next key, hold = race through the keys
    if (chordLEdge()) { lDown = true; lHeld = false; lAt = now; }
    if (lDown && btnL && !chordLHeld()) lDown = false;            // part of the exit chord
    if (lDown && btnL && now - lAt > HOLD_MS) {
        lHeld = true;
        if (now - lastRepeat > REPEAT_MS) { lastRepeat = now; keyIdx = (uint8_t)((keyIdx + 1) % KEYS); }
    }
    if (lDown && !btnL) { if (!lHeld) keyIdx = (uint8_t)((keyIdx + 1) % KEYS); lDown = false; }
    // RIGHT: tap = press the key, hold = send
    if (chordREdge()) { rDown = true; rHeld = false; rAt = now; }
    if (rDown && btnR && !chordRHeld()) rDown = false;
    if (rDown && btnR && !rHeld && now - rAt > HOLD_MS) {
        rHeld = true;
        uint8_t save = keyIdx; keyIdx = KEYS - 1;                   // SEND
        pressKey(); keyIdx = save;
    }
    if (rDown && !btnR) { if (!rHeld) pressKey(); rDown = false; }
}

// ===========================================================================
//  draw
// ===========================================================================
static const char* keyLabel(char k, char* tmp) {
    switch (k) {
    case KY_PAGE:  return page == 0 ? "ab" : page == 1 ? "12" : "AB";
    case KY_SPACE: return "_";
    case KY_DEL:   return "<";
    case KY_SEND:  return scr == SC_NAME ? "OK" : ">>";
    case KY_NAME:  return "ME";
    case KY_BOARD: return "BD";
    default:       tmp[0] = k; tmp[1] = 0; return tmp;
    }
}

static void drawKeyboard(int y0) {
    char tmp[2];
    for (int i = 0; i < KEYS; ++i) {
        int x = 1 + (i % KCOLS) * 17, y = y0 + (i / KCOLS) * 22;
        char k = PAGES[page][i];
        bool sel = i == keyIdx, special = k < 32;
        fb.fillRoundRect(x, y, 16, 20, 3, sel ? PURPLE_DARK : (special ? TEAL : TEAL_DARK));
        if (sel) fb.drawRoundRect(x, y, 16, 20, 3, LIME);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(sel ? LIME_PALE : (special ? LIME : TEAL_LIGHT));
        fb.drawString(keyLabel(k, tmp), x + 8, y + 10, special && strlen(keyLabel(k, tmp)) > 1 ? 1 : 2);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL);
    fb.drawString("L next  holdL fast  R type  holdR send", W / 2, H - 2, 1);
}

// word-wrap one message into lines of <= 27 chars; returns line count
static int wrapLines(const char* who, const char* text, char out[6][29]) {
    char full[COMPOSE_MAX + 12];
    snprintf(full, sizeof(full), "%s: %s", who, text);
    int n = 0, len = (int)strlen(full), p = 0;
    while (p < len && n < 6) {
        int take = len - p > 27 ? 27 : len - p;
        if (len - p > 27) { int k = take; while (k > 10 && full[p + k] != ' ') --k; if (k > 10) take = k; }
        memcpy(out[n], full + p, (size_t)take); out[n][take] = 0; ++n;
        p += take; while (full[p] == ' ') ++p;
    }
    return n;
}

void mantischatDraw() {
    if (boardView && B) { drawBoard(); fb.pushSprite(0, 0); return; }
    fb.fillSprite(TEAL_DEEP);
    if (!S) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("not enough memory", W / 2, H / 2, 2); fb.pushSprite(0, 0); return;
    }
    // header
    fb.fillRect(0, 0, W, 18, PURPLE_DEEP);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME);
    fb.drawString("MANTIS CHAT", 4, 9, 1);
    char b[32];
    fb.setTextDatum(MR_DATUM); fb.setTextColor(radioUp ? TEAL_LIGHT : PURPLE_LIGHT);
    snprintf(b, sizeof(b), radioUp ? "%d online" : "radio off", peersOnline());
    fb.drawString(b, W - 4, 9, 1);

    if (scr == SC_NAME) {
        fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("pick a callsign", W / 2, 40, 2);
        fb.drawString("(up to 8 letters)", W / 2, 60, 1);
        fb.fillRoundRect(20, 90, W - 40, 34, 6, TEAL_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        snprintf(b, sizeof(b), "%s%s", nameBuf, (frameNo / 20) % 2 ? "_" : " ");
        fb.drawString(b, W / 2, 107, 2);
        fb.setTextColor(TEAL); fb.drawString("press OK when done", W / 2, 140, 1);
        drawKeyboard(240);
        fb.pushSprite(0, 0);
        return;
    }
    // message log, newest at the bottom
    int y = 196;
    for (int i = 0; i < S->logCount && y > 20; ++i) {
        const LogLine& l = S->log[(S->logHead - 1 - i - logScroll + LOG_N * 4) % LOG_N];
        char lines[6][29]; int n = wrapLines(l.who, l.text, lines);
        uint16_t c = l.kind == 1 ? LIME : l.kind == 2 ? PURPLE_LIGHT : l.kind == 3 ? TEAL : TEAL_LIGHT;
        for (int k = n - 1; k >= 0 && y > 20; --k) {
            fb.setTextDatum(TL_DATUM); fb.setTextColor(c);
            fb.drawString(lines[k], 3, y, 1);
            y -= 10;
        }
        y -= 2;
    }
    if (!S->logCount) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL);
        fb.drawString("listening on the mesh...", W / 2, 110, 1);
    }
    // compose line
    fb.fillRect(0, 208, W, 24, TEAL_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    const char* c = S->compose; int len = (int)strlen(c);
    const char* shown = len > 25 ? c + (len - 25) : c;            // keep the tail visible
    snprintf(b, sizeof(b), "%s%s", shown, (frameNo / 20) % 2 ? "_" : " ");
    fb.drawString(b, 3, 220, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL);
    snprintf(b, sizeof(b), "%d/%d", len, COMPOSE_MAX);
    fb.drawString(b, W - 3, 220, 1);
    drawKeyboard(236);
    fb.pushSprite(0, 0);
}

// ===========================================================================
//  lifecycle
// ===========================================================================
void mantischatEnter() {
    fb.setRotation(0);
    chordGuardReset();
    if (!S) S = (ChatState*)malloc(sizeof(ChatState));
    if (!S) return;
    memset(S, 0, sizeof(ChatState));
    rxWrite = 0; rxRead = 0;
    loadCall();
    page = 0; keyIdx = 0; lDown = rDown = false; logScroll = 0; frameNo = 0;
    if (S->call[0]) scr = SC_CHAT;
    else { scr = SC_NAME; nameBuf[0] = 0; }
    if (radioStart()) {
        lastPing = millis();
        if (S->call[0]) { mnSend(K_PING, DST_ALL, 0, nullptr, 0); mnwSend(MNW_BEACON, ""); lastLegacyBeacon = millis(); }
    } else {
        logAdd("*", "radio failed to start", 3);
    }
    enterAt = millis();
}

void mantischatShutdown() {
    boardClose();
    radioStop();
    if (S) { free(S); S = nullptr; }
}
