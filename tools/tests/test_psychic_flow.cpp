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
LittleFSClass LittleFS; bool LittleFSClass::begin(bool, const char*, uint8_t, const char*){ return true; } bool LittleFSClass::exists(const char*){ return true; }
fs::File LittleFSClass::open(const char*, const char*){ g_f=fopen(PACK,"rb"); return fs::File(); }
const char* String::c_str() const { return ""; }
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const char*){return 0;} String Preferences::getString(const char*,const char*){return String();}
TFT_eSprite fb; void TFT_eSprite::fillSprite(uint16_t){} void TFT_eSprite::drawPixel(int32_t,int32_t,uint16_t){} void TFT_eSprite::fillCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawCircle(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawLine(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRect(int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::fillTriangle(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastVLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::drawFastHLine(int32_t,int32_t,int32_t,uint16_t){} void TFT_eSprite::setTextDatum(uint8_t){} void TFT_eSprite::setTextColor(uint16_t){} int16_t TFT_eSprite::drawString(const char*,int32_t,int32_t,uint8_t){return 0;} void TFT_eSprite::pushSprite(int32_t,int32_t){} void TFT_eSprite::setRotation(uint8_t){} int16_t TFT_eSprite::textWidth(const char* s,uint8_t f){ return (int16_t)(strlen(s)*(f==4?14:f==2?8:6)); }
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false;

void qpAddXp(int){} void qpGrantPetXp(int){}
#include "mantis/mantis_psychic.cpp"
static void tick(int n=1){ for(int i=0;i<n;i++){ g_ms+=16; mantisPsychicTick(); mantisPsychicDraw(); } }
static int nthWordMatches(const char* text,int n,const char* rev){ int w=0; const char* p=text; while(*p){ while(*p==' ')p++; if(!*p)break; const char* s=p; while(*p&&*p!=' ')p++; if(++w==n){ std::string word(s,p-s); while(!word.empty()&&strchr(".,!?",word.back())) word.pop_back(); for(auto&c:word) c=toupper(c); std::string r(rev); for(auto&c:r) c=toupper(c); return word==r; } } return 0; }
#include "chord_guard.cpp"          // the REAL chord guard, not a stand-in
// one 16 ms frame with the given physical button state, exactly like main.cpp
static void frame(bool l, bool r){ btnL_prev=btnL; btnR_prev=btnR; btnL=l; btnR=r; g_ms+=16; mantisPsychicTick(); mantisPsychicDraw(); }
static void tapR(){ for(int i=0;i<4;i++) frame(false,true); for(int i=0;i<4;i++) frame(false,false); }
static void tapL(){ for(int i=0;i<4;i++) frame(true,false); for(int i=0;i<4;i++) frame(false,false); }
int main(){
  int fails=0; auto check=[&](bool ok,const char* w){ printf("  %s %s\n", ok?"ok  ":"FAIL", w); if(!ok) fails++; };
  // arrive from the menu still holding R (the press that selected the app)
  btnR=true; btnR_prev=false; mantisPsychicEnter();
  for(int i=0;i<10;i++) frame(false,true); for(int i=0;i<30;i++) frame(false,false);
  check(fxCount==22 && soloCount==12 && perfCount==10, "pack indexed from the content partition: 22 tricks (12 solo, 10 performer)");
  check(scr==SC_START, "the menu's R press is swallowed - still on the welcome screen");
  tapR();
  check(scr==SC_PLAY && beatCount>0, "SOLO: R on the welcome screen starts a trick (real chord guard, real frames)");
  int frames=0; while(scr==SC_PLAY && frames<4000){ frame(false,false); frames++; if(awaitPress || (beats[beatIdx].kind=='R' && revealTxt && millis()-revealAt>800)) tapR(); }
  check(scr==SC_DONE || scr==SC_START, "the trick plays through to its reveal");
  for(int i=0;i<60;i++) frame(false,false); if(scr==SC_DONE) tapR(); for(int i=0;i<10;i++) frame(false,false);
  tapL(); tapR();
  check(scr==SC_LIST, "PERFORM: L then R opens the trick list");
  tapR();
  check(scr==SC_PLAY && beatCount>0, "R on a performer trick starts it");
  printf(fails?"\n%d FAILED\n":"\nPSYCHIC STARTS AND PLAYS WITH THE REAL BUTTONS\n",fails); return fails; }
