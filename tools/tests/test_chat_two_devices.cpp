#include <sys/socket.h>
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
EspClass ESP; static uint64_t g_mac=0; uint64_t EspClass::getEfuseMac(){ return g_mac; }
static esp_now_recv_cb_t g_cb=nullptr; static std::vector<std::vector<uint8_t>> sent;
esp_err_t esp_now_init(){return 0;} esp_err_t esp_now_deinit(){return 0;} esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t c){g_cb=c;return 0;}
esp_err_t esp_now_unregister_recv_cb(){g_cb=nullptr;return 0;} esp_err_t esp_now_add_peer(const esp_now_peer_info_t*){return 0;}
bool esp_now_is_peer_exist(const uint8_t*){return false;} static int g_air=-1; static uint8_t g_myMac[6];
esp_err_t esp_now_send(const uint8_t*,const uint8_t* d,size_t n){ uint8_t b[256]; memcpy(b,g_myMac,6); memcpy(b+6,d,n); send(g_air,b,n+6,0); return 0; }   // the air carries the sender MAC
int esp_wifi_set_promiscuous(bool){return 0;} int esp_wifi_set_channel(int,int){return 0;}
static const char* g_call="TDISP"; const char* String::c_str() const { return g_call; }
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const char*){return 0;} String Preferences::getString(const char*,const char*){return String();}
TFT_eSprite fb; void TFT_eSprite::fillSprite(uint16_t){} void TFT_eSprite::drawPixel(int32_t,int32_t,uint16_t){} void TFT_eSprite::fillCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastVLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastHLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::setTextDatum(uint8_t){} void TFT_eSprite::setTextColor(uint16_t){} int16_t TFT_eSprite::drawString(const char*,int32_t,int32_t,uint8_t){return 0;} void TFT_eSprite::pushSprite(int32_t,int32_t){} void TFT_eSprite::setRotation(uint8_t){}
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false;
void chordGuardTick(){} void chordGuardReset(){} static bool gL=false,gR=false; bool chordLEdge(){bool e=gL; gL=false; return e;} bool chordREdge(){bool e=gR; gR=false; return e;} bool chordLHeld(){return btnL;} bool chordRHeld(){return btnR;}
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "mantischat/mantischat_mode.cpp"
static void pump(){ uint8_t b[256]; for(;;){ ssize_t n=recv(g_air,b,sizeof b,MSG_DONTWAIT); if(n<=6) break; if(g_cb) g_cb(b,b+6,(int)n-6); } }
static void step(int n=1){ for(int i=0;i<n;i++){ g_ms+=16; pump(); mantischatTick(); mantischatDraw(); } }
static void press(bool left){ (left?gL:gR)=true; (left?btnL:btnR)=true; step(); (left?btnL:btnR)=false; step(); }
// type text with the real arcade keyboard: L to walk to each key, R to press it
static void typeText(const char* t){
  for(const char* p=t; *p; p++){
    int target=-1, pg=-1;
    for(int q=0;q<3 && target<0;q++) for(int i=0;i<KEYS;i++) if(PAGES[q][i]==(*p==' '?KY_SPACE:*p)){ target=i; pg=q; break; }
    if(target<0) continue;
    while(page!=pg){ while(keyIdx!=26) press(true); press(false); }          // PAGE key
    while(keyIdx!=target) press(true);
    press(false);
  }
}
static int device(int fd, uint64_t mac, const char* call, const char* say, const char* expectFrom, const char* expectText, bool speakFirst){
  g_air=fd; g_mac=mac; g_call=call; for(int i=0;i<6;i++) g_myMac[i]=(uint8_t)(mac>>(8*i));   // STA MAC = efuse MAC g_ms=1000+(uint32_t)(mac&0xFF)*7;
  mantischatEnter(); step(30);
  auto heard=[&](){ for(int i=0;i<S->logCount;i++){ const LogLine& l=S->log[i]; if(!strcmp(l.who,expectFrom)&&!strcmp(l.text,expectText)&&l.kind==0) return true;} return false; };
  if(!speakFirst){ for(int i=0;i<20000 && !heard();i++){ step(); usleep(1000); } }   // wait in REAL time
  typeText(say);
  while(keyIdx!=KEYS-1) press(true); press(false);           // SEND key
  if(speakFirst){ for(int i=0;i<20000 && !heard();i++){ step(); usleep(1000); } }
  step(60);
  bool ok=heard();
  printf("  [%s] typed \"%s\" on the keyboard and sent it; heard \"%s: %s\" -> %s  (%d online)\n", call, say, expectFrom, expectText, ok?"YES":"NO", peersOnline());
  mantischatShutdown();
  return ok?0:1;
}
int main(){
  int sv[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
  pid_t a=fork();
  if(a==0){ close(sv[1]); return device(sv[0], 0x0000AAAAAAAAAA01ull, "KYLER", "HI FROM KYLER", "NEKODA", "HEY KYLER", true); }
  close(sv[0]);
  int rb=device(sv[1], 0x0000BBBBBBBBBB02ull, "NEKODA", "HEY KYLER", "KYLER", "HI FROM KYLER", false);
  int st=0; waitpid(a,&st,0); int ra=WEXITSTATUS(st);
  printf(ra==0&&rb==0 ? "\nTWO DEVICES CHATTED BOTH WAYS\n" : "\nCHAT FAILED\n"); return ra|rb;
}
