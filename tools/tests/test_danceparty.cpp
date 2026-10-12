#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
// ---- fakes the app links against ----
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "WiFi.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "Preferences.h"
static uint32_t g_ms=1000; uint32_t millis(){return g_ms;} uint32_t esp_random(){static uint32_t s=7; s=s*1664525u+1013904223u; return s;} void delay(uint32_t){}
WiFiClass WiFi; void WiFiClass::mode(int){} void WiFiClass::disconnect(bool){} void WiFiClass::macAddress(uint8_t*){}
EspClass ESP; uint64_t EspClass::getEfuseMac(){ return 0x0000A1B2C3D4E5F6ull; }
static esp_now_recv_cb_t g_cb=nullptr; static std::vector<std::vector<uint8_t>> sent;
esp_err_t esp_now_init(){return 0;} esp_err_t esp_now_deinit(){return 0;} esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t c){g_cb=c;return 0;}
esp_err_t esp_now_unregister_recv_cb(){g_cb=nullptr;return 0;} esp_err_t esp_now_add_peer(const esp_now_peer_info_t*){return 0;}
bool esp_now_is_peer_exist(const uint8_t*){return false;} esp_err_t esp_now_send(const uint8_t*,const uint8_t* d,size_t n){sent.emplace_back(d,d+n);return 0;}
int esp_wifi_set_promiscuous(bool){return 0;} int esp_wifi_set_channel(int,int){return 0;}
const char* String::c_str() const { return "TDISP"; }
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const char*){return 0;} String Preferences::getString(const char*,const char*){return String();}
TFT_eSprite fb; void TFT_eSprite::fillSprite(uint16_t){} void TFT_eSprite::drawPixel(int32_t,int32_t,uint16_t){} void TFT_eSprite::fillCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastVLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastHLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::setTextDatum(uint8_t){} void TFT_eSprite::setTextColor(uint16_t){} int16_t TFT_eSprite::drawString(const char*,int32_t,int32_t,uint8_t){return 0;} void TFT_eSprite::pushSprite(int32_t,int32_t){} void TFT_eSprite::setRotation(uint8_t){}
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false;
void chordGuardTick(){} void chordGuardReset(){} static bool gL=false,gR=false; bool chordLEdge(){bool e=gL; gL=false; return e;} bool chordREdge(){bool e=gR; gR=false; return e;} bool chordLHeld(){return btnL;} bool chordRHeld(){return btnR;}
#include "danceparty/danceparty_mode.cpp"
static std::vector<uint8_t> dframe(uint8_t sub, uint32_t pkt, std::vector<uint8_t> pl){
  std::vector<uint8_t> f(28,0); f[0]=0x4D; f[1]=1; f[2]=0x30; uint32_t src=0x5E5E5E5E, dst=0xFFFFFFFF;
  memcpy(&f[4],&src,4); memcpy(&f[8],&dst,4); memcpy(&f[12],&pkt,4); f[19]=sub; memcpy(&f[20],"SYNAPSE\0",8);
  f.insert(f.end(),pl.begin(),pl.end()); return f; }
static void feed(const std::vector<uint8_t>& f){ g_cb(nullptr,f.data(),(int)f.size()); }
static void step(int n=1){ for(int i=0;i<n;i++){ g_ms+=16; dancepartyTick(); dancepartyDraw(); } }
static void tapL(){ gL=true; btnL=true; step(); btnL=false; step(); }
static void tapR(){ gR=true; step(); }
int main(){
  int fails=0; auto check=[&](bool ok,const char* w){ printf("  %s %s\n", ok?"ok  ":"FAIL", w); if(!ok) fails++; };
  dancepartyEnter(); step(20);
  check(dscr==DS_WELCOME && radioUp, "opens on the welcome screen, already listening");
  check(!synced() && bpm==0 && !beatLive(), "no host: no tempo of its own");
  tapL(); check(menuSel==1, "L moves to TAP TEMPO"); tapL(); check(menuSel==0, "L wraps back to DANCE WITH CORE2");
  tapR(); check(dscr==DS_LOBBY, "R on DANCE WITH CORE2 opens the lobby");
  uint32_t pk=1;
  feed(dframe(2, pk++, {0,0})); step(); check(dscr==DS_LOBBY, "START alone (no beat yet) keeps waiting");
  feed(dframe(0, pk++, {128,0, 2, 0,0,0,0})); step();
  check(dscr==DS_DANCE && beatLive() && (int)bpm==128 && barAnchor==2, "first Synapse beat -> dancing at its 128 bpm");
  feed(dframe(1, pk++, {3, 200, 0,0})); step(); check(move==3, "MOVE from the Synapse -> SPIN");
  feed(dframe(1, pk-1, {6, 200, 0,0})); step(); check(move==3, "duplicate pktId ignored");
  std::vector<uint8_t> chat(28,0); chat[0]=0x4D; chat[1]=1; chat[2]=0x01; feed(chat); step(); check(move==3, "chat traffic ignored");
  for(int i=0;i<250;i++) step(); check(dscr==DS_LOBBY, "Synapse silent 3+ s -> back to the lobby");
  // from the WELCOME screen a party pulls us straight in
  gL=true; btnL=true; for(int i=0;i<40;i++) step(); btnL=false; step(); check(dscr==DS_WELCOME, "hold L -> back to welcome");
  feed(dframe(0, pk++, {100,0, 0, 0,0,0,0})); step(); check(dscr==DS_DANCE && (int)bpm==100, "a party starting while on WELCOME jumps straight to dancing");
  for(int i=0;i<250;i++) step(); gL=true; btnL=true; for(int i=0;i<40;i++) step(); btnL=false; step();
  // tap tempo: four taps 500 ms apart = 120 bpm
  tapL(); tapR(); check(dscr==DS_TAP && bpm==0, "TAP TEMPO opens, waiting for taps");
  for(int k=0;k<4;k++){ tapR(); for(int i=0;i<30;i++) step(); g_ms += 500 - 31*16; }
  check(fabsf(bpm-120.0f) < 3.0f && beatLive(), "4 taps 500 ms apart -> ~120 bpm, dancing");
  float b120=bpm; for(int i=0;i<400;i++) step(); check(beatLive() && bpm==b120, "tap tempo keeps going without more taps");
  tapL(); check(move==1, "L changes the move in tap mode");
  int drawn=0; for(int mv=0; mv<10; mv++){ move=mv; for(int i=0;i<60;i++){ step(); drawn++; } }
  check(drawn==600, "every move drawn without fault");
  dancepartyShutdown(); check(!radioUp && g_cb==nullptr, "radio torn down on exit");
  printf(fails?"\n%d FAILED\n":"\nALL DANCE CHECKS PASSED\n",fails); return fails; }
