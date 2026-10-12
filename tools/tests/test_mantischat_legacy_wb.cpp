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
namespace ref {                       // the Core2 MantisChat's own wire definitions
#include "ref/mc_proto.h"
}
static void air(const uint8_t* mac, const void* d, int n){ g_cb(mac,(const uint8_t*)d,n); mantischatTick(); }
static std::vector<uint8_t> v1(uint8_t kind, uint8_t sub, uint32_t src, uint32_t pkt, const char* call, const void* pl, int n){
  ref::MnHdr h; memset(&h,0,sizeof h); h.magic=0x4D; h.version=1; h.kind=kind; h.srcNode=src; h.dstNode=0xFFFFFFFF; h.pktId=pkt; h.chanOrSub=sub;
  strncpy(h.callsign,call,8); std::vector<uint8_t> f((uint8_t*)&h,(uint8_t*)&h+28); if(n) f.insert(f.end(),(const uint8_t*)pl,(const uint8_t*)pl+n); return f; }
static ref::MnwPacket mnw(uint8_t type, const char* call, const char* text){
  ref::MnwPacket p; memset(&p,0,sizeof p); memcpy(p.magic,"MNW1",4); p.type=type; strncpy(p.callsign,call,11); strncpy(p.text,text,179); return p; }
int main(){
  int fails=0; auto check=[&](bool ok,const char* w){ printf("  %s %s\n", ok?"ok  ":"FAIL", w); if(!ok) fails++; };
  mantischatEnter();
  // --- legacy MantisOS watch (MNW1, built with the reference struct) ----------
  uint8_t watchMac[6]={0x24,0x6F,0x28,0x11,0x22,0x33}; uint32_t watchNode=nodeFromMac(watchMac);
  bool sawBeacon=false; for(auto& f: sent) if(f.size()==197 && !memcmp(f.data(),"MNW1",4) && f[4]==0) sawBeacon=true;
  check(sawBeacon, "announces itself to old watches with an MNW1 beacon on start");
  auto b=mnw(0,"MANTIS-2233",""); air(watchMac,&b,sizeof b);
  check(peersOnline()==1 && S->logCount==0, "legacy beacon: watch counted online, nothing printed");
  auto g=mnw(1,"MANTIS-2233","hi from my 1337 watch"); air(watchMac,&g,sizeof g);
  check(S->logCount==1 && !strcmp(S->log[0].who,"MANTIS-2233") && !strcmp(S->log[0].text,"hi from my 1337 watch"), "legacy group message shown with the full 11-char callsign");
  auto dmm=mnw(2,"MANTIS-2233","psst"); air(watchMac,&dmm,sizeof dmm);
  check(S->logCount==2 && S->log[1].kind==2, "legacy DM shown as a DM");
  // --- a dual-format sender (Core2 / T-Display): same message twice -----------
  uint8_t c2Mac[6]={0x0A,0x0B,0x0C,0x0D,0x0E,0x0F}; uint32_t c2=nodeFromMac(c2Mac);
  const char* t="dinner is ready";
  auto f1=v1(0x01,0,c2,0x1111,"CORE2",t,(int)strlen(t)); air(c2Mac,f1.data(),(int)f1.size());
  auto f2=mnw(1,"CORE2",t); air(c2Mac,&f2,sizeof f2);
  int n=0; for(int i=0;i<S->logCount;i++) if(!strcmp(S->log[i].text,t)) n++;
  check(n==1, "Core2 sends it as v1 AND MNW1 -> shown ONCE (the Core2 reference shows it twice)");
  g_ms+=4000; auto f3=v1(0x01,0,c2,0x2222,"CORE2",t,(int)strlen(t)); air(c2Mac,f3.data(),(int)f3.size());
  n=0; for(int i=0;i<S->logCount;i++) if(!strcmp(S->log[i].text,t)) n++;
  check(n==2, "the same words said again later are NOT swallowed");
  // --- what we send -------------------------------------------------------------
  sent.clear(); strcpy(S->compose,"HELLO MESH"); sendCompose();
  bool gotV1=false, gotLegacy=false;
  for(auto& f: sent){ if(f.size()==28+10 && f[0]==0x4D && f[1]==1 && f[2]==0x01) gotV1=true;
    if(f.size()==sizeof(ref::MnwPacket)){ ref::MnwPacket p; memcpy(&p,f.data(),sizeof p); if(!memcmp(p.magic,"MNW1",4)&&p.type==1&&!strcmp(p.text,"HELLO MESH")&&!strcmp(p.callsign,"TDISP")) gotLegacy=true; } }
  check(gotV1 && gotLegacy, "our group message goes out as v1 TEXT and as a 197-byte MNW1 group packet");
  sent.clear(); g_ms+=5100; mantischatTick(); bool beacon=false; for(auto& f: sent) if(f.size()==197 && f[4]==0) beacon=true;
  check(beacon, "MNW1 beacon repeats every 5 s");
  // --- whiteboard (built with the reference WbSeg) --------------------------------
  sent.clear(); boardOpen();
  bool sync=false; for(auto& f: sent) if(f.size()==28 && f[2]==0x40 && f[19]==4) sync=true;
  check(boardView && B && sync, "opening the board sends WB SYNC so boards replay their ink");
  auto seg=[&](uint32_t owner, uint8_t* mac, uint32_t sid, uint16_t seq, int npts, uint32_t pkt){
    ref::WbSeg w; memset(&w,0,sizeof w); w.sid=sid; w.seq=seq; w.color=0x001F; w.width=4; w.n=(uint8_t)npts;
    for(int i=0;i<npts;i++){ w.pts[i*2]=(int16_t)(10+i*6); w.pts[i*2+1]=(int16_t)(20+i*3); }
    int len=ref::WB_SEG_BASE+npts*4; auto f=v1(0x40,1,owner,pkt,"CORE2",&w,len); air(mac,f.data(),(int)f.size()); };
  uint32_t sidA=(0x1234u<<16)|5;
  seg(c2,c2Mac,sidA,0,48,0x3001); check(B->nrec==1 && B->rec[0].n==24, "48-point SEG kept (thinned to 24 points, ends preserved)");
  check(B->rec[0].pts[0]==vx(10) && B->rec[0].pts[46]==vx(10+47*6), "first and last points survive the thinning");
  seg(c2,c2Mac,sidA,0,48,0x3002); check(B->nrec==1, "duplicate (owner,sid,seq) ignored");
  seg(c2,c2Mac,sidA,1,10,0x3003); check(B->nrec==2, "next chunk of the stroke added");
  uint32_t del=sidA; auto foreign=v1(0x40,2,watchNode,0x3004,"OTHER",&del,4); air(watchMac,foreign.data(),(int)foreign.size());
  check(B->nrec==2, "a DEL from someone else can't erase the Core2's ink");
  auto own=v1(0x40,2,c2,0x3005,"CORE2",&del,4); air(c2Mac,own.data(),(int)own.size());
  check(B->nrec==0, "the owner's DEL removes its stroke");
  seg(c2,c2Mac,sidA,2,10,0x3006); check(B->nrec==0, "late SEG for a deleted stroke stays deleted (tombstone)");
  uint32_t sidB=(0x1234u<<16)|9, sidC=(0x1234u<<16)|20;
  seg(c2,c2Mac,sidB,0,5,0x3007); seg(c2,c2Mac,sidC,0,5,0x3008); check(B->nrec==2, "two more strokes");
  uint32_t wm=(0x1234u<<16)|21; auto clr=v1(0x40,3,c2,0x3009,"CORE2",&wm,4); air(c2Mac,clr.data(),(int)clr.size());
  check(B->nrec==0, "CLEAR removes all of the owner's strokes");
  seg(c2,c2Mac,sidB,1,5,0x300A); check(B->nrec==0, "CLEAR watermark blocks older strokes from that session");
  seg(c2,c2Mac,(0x9999u<<16)|1,0,5,0x300B); check(B->nrec==1, "a new boot session draws again");
  for(int i=0;i<30;i++) mantischatDraw();
  check(true, "board renders");
  boardClose(); check(!boardView && B==nullptr, "closing the board frees its 8 KB");
  mantischatShutdown(); check(S==nullptr && g_cb==nullptr, "chat shuts down cleanly");
  printf(fails?"\n%d FAILED\n":"\nLEGACY + WHITEBOARD CHECKS PASSED\n",fails); return fails; }
