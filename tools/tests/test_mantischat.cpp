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
void chordGuardTick(){} void chordGuardReset(){} bool chordLEdge(){return false;} bool chordREdge(){return false;} bool chordLHeld(){return btnL;} bool chordRHeld(){return btnR;}
#include "mantischat/mantischat_mode.cpp"
// ---- a frame exactly as the spec describes it, built field by field ----
static std::vector<uint8_t> frame(uint8_t kind,uint8_t flags,uint32_t src,uint32_t dst,uint32_t pkt,uint8_t chan,const char* call,const char* text){
  std::vector<uint8_t> f(28,0); f[0]=0x4D; f[1]=1; f[2]=kind; f[3]=flags;
  auto le=[&](int o,uint32_t v){f[o]=v;f[o+1]=v>>8;f[o+2]=v>>16;f[o+3]=v>>24;};
  le(4,src); le(8,dst); le(12,pkt); f[16]=1; f[17]=0; f[18]=0; f[19]=chan; strncpy((char*)&f[20],call,8);
  if(text) f.insert(f.end(),text,text+strlen(text)); return f; }
static void deliver(const std::vector<uint8_t>& f){ g_cb(nullptr,f.data(),(int)f.size()); }
int main(){
  int fails=0; auto check=[&](bool ok,const char* what){printf("  %s %s\n",ok?"ok  ":"FAIL",what); if(!ok)fails++;};
  mantischatEnter();
  check(S && !strcmp(S->call,"TDISP"), "loads callsign, radio up");
  bool ping=false; for(auto& f: sent) if(f.size()==28 && f[2]==0x02) ping=true;
  check(ping, "announces itself with a 28-byte PING (plus an MNW1 beacon for old watches)");
  { auto it=sent.begin(); while(it!=sent.end() && !(it->size()==28 && (*it)[2]==0x02)) ++it; std::vector<uint8_t> p0=*it; sent.clear(); sent.push_back(p0); }
  uint32_t me = (uint32_t)(0x0000A1B2C3D4E5F6ull & 0xFFFFFFFF);
  { const auto& f=sent[0]; uint32_t src=f[4]|f[5]<<8|f[6]<<16|(uint32_t)f[7]<<24, dst=f[8]|f[9]<<8|f[10]<<16|(uint32_t)f[11]<<24;
    check(f[0]==0x4D && f[1]==1 && src==me && dst==0xFFFFFFFF && f[18]==0 && !memcmp(&f[20],"TDISP\0\0\0",8), "header fields at the spec offsets, LE, null-padded callsign"); }
  sent.clear();
  deliver(frame(0x01,0,0x11111111,0xFFFFFFFF,0xAAAA0001,0,"CORE2","hello from core2")); mantischatTick();
  check(S->logCount==1 && !strcmp(S->log[0].who,"CORE2") && !strcmp(S->log[0].text,"hello from core2"), "receives channel-0 TEXT");
  deliver(frame(0x01,0,0x11111111,0xFFFFFFFF,0xAAAA0001,0,"CORE2","hello from core2")); mantischatTick();
  check(S->logCount==1, "drops a duplicate (srcNode, pktId)");
  deliver(frame(0x01,0,0x11111111,0xFFFFFFFF,0xAAAA0002,5,"CORE2","other channel")); mantischatTick();
  check(S->logCount==1, "ignores other chat channels");
  deliver(frame(0x01,0,0x22222222,0x33333333,0xAAAA0003,0,"WATCH","not for us")); mantischatTick();
  check(S->logCount==1, "ignores DMs to someone else");
  deliver(frame(0x01,0x02,0x22222222,me,0xAAAA0004,0,"WATCH","dm for you")); mantischatTick();
  check(S->logCount==2 && S->log[1].kind==2, "receives a DM to us");
  check(sent.size()==1 && sent[0][2]==0x05 && sent[0].size()==32 && (sent[0][28]|sent[0][29]<<8|sent[0][30]<<16|(uint32_t)sent[0][31]<<24)==0xAAAA0004u, "ACKs it with ackedPktId (asked + unicast)");
  sent.clear();
  deliver(frame(0x02,0,0x44444444,0xFFFFFFFF,0xBBBB0001,0,"FINDER",nullptr)); mantischatTick();
  check(sent.size()==1 && sent[0][2]==0x03 && sent[0].size()==36, "answers PING with an 8-byte PONG");
  check(peersOnline()==3, "counts 3 peers online (CORE2, WATCH, FINDER)");
  deliver(frame(0x40,0,0x55555555,0xFFFFFFFF,0xCCCC0001,0,"WEIRD","x")); mantischatTick();
  check(S->logCount==2, "drops an unknown kind silently");
  std::vector<uint8_t> bad=frame(0x01,0,1,0xFFFFFFFF,9,0,"BAD","v2"); bad[1]=2; deliver(bad); mantischatTick();
  check(S->logCount==2, "rejects a wrong protocol version");
  strcpy(S->compose,"hi mesh"); sent.clear(); sendCompose();
  bool txt=false; for(auto& f: sent) if(f.size()==28+7 && f[2]==0x01 && !memcmp(&f[28],"hi mesh",7) && f[19]==0) txt=true;
  check(txt, "sends TEXT on channel 0 (and an MNW1 copy for old watches)");
  mantischatShutdown();
  check(S==nullptr && g_cb==nullptr, "frees state and unregisters the radio on exit");
  printf(fails?"\n%d FAILED\n":"\nALL PROTOCOL CHECKS PASSED\n", fails); return fails;
}
