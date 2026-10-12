#include <cstdio>
#include <cstring>
#include <string>
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "LittleFS.h"
#include "Preferences.h"
static uint32_t g_ms = 100000; uint32_t millis(){ return g_ms; } uint32_t esp_random(){ static uint32_t s=99; s=s*1103515245u+12345u; return s; } void delay(uint32_t){}
static FILE* g_f=nullptr;
namespace fs { File::operator bool() const { return g_f!=nullptr; } bool File::available(){ int c=fgetc(g_f); if(c==EOF) return false; ungetc(c,g_f); return true; }
 int File::read(){ return fgetc(g_f); } size_t File::read(uint8_t* b,size_t n){ return fread(b,1,n,g_f); } bool File::seek(uint32_t o){ return fseek(g_f,o,SEEK_SET)==0; }
 size_t File::size(){ long p=ftell(g_f); fseek(g_f,0,SEEK_END); long n=ftell(g_f); fseek(g_f,p,SEEK_SET); return n; } void File::close(){ if(g_f){fclose(g_f); g_f=nullptr;} }
 size_t File::position(){ return ftell(g_f); }
 int File::readBytesUntil(char t,char* b,int n){ int i=0,c; while(i<n && (c=fgetc(g_f))!=EOF){ if(c==t) break; b[i++]=(char)c; } return i; } }
LittleFSClass LittleFS; bool LittleFSClass::begin(bool){ return true; } bool LittleFSClass::exists(const char*){ return true; }
fs::File LittleFSClass::open(const char*, const char*){ g_f=fopen(PACK,"rb"); return fs::File(); }
const char* String::c_str() const { return ""; }
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const char*){return 0;} String Preferences::getString(const char*,const char*){return String();}
TFT_eSprite fb; void TFT_eSprite::fillSprite(uint16_t){} void TFT_eSprite::drawPixel(int32_t,int32_t,uint16_t){} void TFT_eSprite::fillCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastVLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastHLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::setTextDatum(uint8_t){} void TFT_eSprite::setTextColor(uint16_t){} int16_t TFT_eSprite::drawString(const char*,int32_t,int32_t,uint8_t){return 0;} void TFT_eSprite::pushSprite(int32_t,int32_t){} void TFT_eSprite::setRotation(uint8_t){} int16_t TFT_eSprite::textWidth(const char* s,uint8_t f){ return (int16_t)(strlen(s)*(f==4?14:f==2?8:6)); }
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false;
static bool gL=false,gR=false; void chordGuardTick(){} void chordGuardReset(){} bool chordLEdge(){ bool e=gL; gL=false; return e; } bool chordREdge(){ bool e=gR; gR=false; return e; } bool chordLHeld(){return false;} bool chordRHeld(){return false;}
void qpAddXp(int){} void qpGrantPetXp(int){}
#include "mantis/mantis_psychic.cpp"
static void tick(int n=1){ for(int i=0;i<n;i++){ g_ms+=16; mantisPsychicTick(); mantisPsychicDraw(); } }
static int nthWordMatches(const char* text,int n,const char* rev){ int w=0; const char* p=text; while(*p){ while(*p==' ')p++; if(!*p)break; const char* s=p; while(*p&&*p!=' ')p++; if(++w==n){ std::string word(s,p-s); while(!word.empty()&&strchr(".,!?",word.back())) word.pop_back(); for(auto&c:word) c=toupper(c); std::string r(rev); for(auto&c:r) c=toupper(c); return word==r; } } return 0; }

static void enterDigits(long v){            // type v the way a performer would: L turns, R next
  int nd=askNd; char d[6]; snprintf(d,sizeof(d),"%0*ld",nd,v);
  for(int i=0;i<nd;i++){ for(int k=0;k<d[i]-'0';k++){ gL=true; tick(); } gR=true; tick(); }
}
static std::string runPerf(const char* id, long total){
  scr=SC_START; tick(30);
  int idx=-1; for(int i=0;i<fxCount;i++) if(fxIdx[i].performer && !strncmp(fxIdx[i].title, id, strlen(id))) idx=i;
  if(idx<0) return "no such effect";
  beginEffect(idx);
  for(int guard=0; guard<3000 && scr==SC_PLAY; guard++){
    tick();
    if(asking && askDigits){ enterDigits(total); continue; }
    if(awaitPress){ gR=true; continue; }
    if(beats[beatIdx].kind=='R' && revealTxt) return revealTxt;
  }
  return "no reveal";
}
int main(){
  mantisPsychicEnter(); tick(30);
  struct { const char* title; long total; const char* want; } T[] = {
    {"NAME THEIR CARD", (7*2+3)*5+2,  "SEVEN OF HEARTS"},
    {"NAME THEIR CARD", (13*2+3)*5+4, "KING OF DIAMONDS"},
    {"NAME THEIR CARD", (1*2+3)*5+1,  "ACE OF CLUBS"},
    {"NAME THEIR BIRTHDAY", ((((7*5+6)*4+9)*5+4)-165), "JUL 4"},
    {"NAME THEIR BIRTHDAY", ((((12*5+6)*4+9)*5+25)-165), "DEC 25"},
    {"NAME THEIR TIME", ((3*2+5)*50+45)-250, "3:45"},
    {"NAME THEIR TIME", ((12*2+5)*50+5)-250, "12:05"},
  };
  int bad=0;
  for(auto& t: T){ std::string r=runPerf(t.title,t.total); bool ok=(r==t.want); if(!ok) bad++;
    printf("  %-20s they say %4ld -> mantis: %-17s %s\n", t.title, t.total, r.c_str(), ok?"ok":"WRONG"); }
  // a total outside the range must ask to check, not accept
  scr=SC_START; tick(30); int idx=-1; for(int i=0;i<fxCount;i++) if(fxIdx[i].performer && !strncmp(fxIdx[i].title,"NAME THEIR CARD",15)) idx=i;
  beginEffect(idx); for(int g=0; g<2000 && !(asking&&askDigits); g++){ tick(); if(awaitPress) gR=true; }
  enterDigits(999); printf("  out-of-range total 999: %s\n", (asking && askBad) ? "asks to check it (ok)" : "ACCEPTED (bad)"); if(!(asking&&askBad)) bad++;
  return bad;
}
