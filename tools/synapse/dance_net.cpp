// dance_net.cpp - the lightest ESP-NOW sender we can build on this core.
//
// No <WiFi.h>: that pulled the whole Arduino WiFi library (WiFiGeneric,
// WiFiSTA, lwIP setup) and overflowed DRAM by 15 KB. ESP-NOW only needs the
// radio driver, so this talks to ESP-IDF directly: init the driver with small
// buffers, no NVS, no AMPDU, station mode, channel 1, send-only. The radio is
// up only while party mode is on and is fully torn down when it goes off.
//
// FRAME RATE: bringing the radio up/down takes 100-300 ms, so it never runs
// on the frame loop. A one-shot task does it on core 0 at priority 1 - below
// the display task (2) and the FX worker (3), so it only uses idle time and
// cannot preempt either; the main loop on core 1 is never touched. Sending a
// beat is a non-blocking queue call (microseconds), and happens only once the
// radio reports ready.
#ifdef MANTIS_DANCE_NET
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_event.h>
#include <esp_system.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "dance_net.h"

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static volatile bool s_up = false;       // set only when the radio is fully ready
static volatile bool s_busy = false;     // a bring-up/teardown task is running
static uint16_t s_seq = 0;
static uint8_t s_lastMove = 0xFF;
static uint32_t s_t0 = 0, s_node = 0;

struct __attribute__((packed)) MnHeader {          // MANTISNOW.md, 28 bytes
    uint8_t magic, version, kind, flags;
    uint32_t srcNode, dstNode, pktId;
    uint16_t seq;
    uint8_t ttl, chanOrSub;
    char callsign[8];
};

static void send(uint8_t sub, const void* pl, int n) {
    if (!s_up) return;
    uint8_t buf[28 + 16];
    MnHeader h; memset(&h, 0, sizeof(h));
    h.magic = 0x4D; h.version = 1; h.kind = 0x30;
    h.srcNode = s_node; h.dstNode = 0xFFFFFFFFu; h.pktId = esp_random(); h.seq = ++s_seq; h.chanOrSub = sub;
    memcpy(h.callsign, "SYNAPSE", 7);
    memcpy(buf, &h, 28);
    if (n > 0) memcpy(buf + 28, pl, (size_t)n);
    esp_now_send(BCAST, buf, (size_t)(28 + n));
}

static void up() {
    // MANTISNOW.md decision 1: nodeId = low 32 bits of the factory MAC, mac[0] lowest
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    s_node = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;                  // never touch NVS
    cfg.static_rx_buf_num = 4;           // send-only: keep the driver small
    cfg.dynamic_rx_buf_num = 8;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    if (esp_wifi_init(&cfg) != ESP_OK) return;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) { esp_wifi_deinit(); return; }
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { esp_wifi_stop(); esp_wifi_deinit(); return; }
    esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, BCAST, 6); p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
    esp_now_add_peer(&p);
    s_up = true; s_t0 = millis(); s_lastMove = 0xFF;
    uint16_t bpm = 0; send(2, &bpm, 2);                                   // START
}

static void down() {
    if (!s_up) return;
    send(3, nullptr, 0);                                                   // STOP
    delay(20);
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_up = false;
}

static void radioTask(void* arg) {
    if (arg) up(); else down();
    s_busy = false;
    vTaskDelete(nullptr);
}

void danceNetService(bool partyOn) {
    if (s_busy || partyOn == (bool)s_up) return;            // nothing to do, or already working on it
    s_busy = true;
    if (xTaskCreatePinnedToCore(radioTask, "dnet", 3072, partyOn ? (void*)1 : nullptr,
                                1, nullptr, 0) != pdPASS)
        s_busy = false;                                     // couldn't start: try again next loop
}
bool danceNetUp() { return s_up; }

void danceNetBeat(uint16_t bpm, uint8_t beatInBar) {
    uint8_t pl[7]; uint32_t ms = millis() - s_t0;
    memcpy(pl, &bpm, 2); pl[2] = beatInBar & 3; memcpy(pl + 3, &ms, 4);
    send(0, pl, 7);
}

void danceNetMove(uint8_t moveId, uint8_t intensity) {
    if (moveId == s_lastMove) return;
    s_lastMove = moveId;
    uint8_t pl[4] = { moveId, intensity, 0, 0 };
    send(1, pl, 4);
}
#endif  // MANTIS_DANCE_NET
