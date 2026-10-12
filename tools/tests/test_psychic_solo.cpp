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
static bool gL=false,gR=false; void chordGuardTick(){} void chordGuardReset(){} bool chordLEdge(){ bool e=gL; gL=false; return e; } bool chordREdge(){ bool e=gR; gR=false; return e; } bool chordLHeld(){return false;} bool chordRHeld(){return false;}
void qpAddXp(int){} void qpGrantPetXp(int){}
#include "mantis/mantis_psychic.cpp"
static void tick(int n=1){ for(int i=0;i<n;i++){ g_ms+=16; mantisPsychicTick(); mantisPsychicDraw(); } }
static int nthWordMatches(const char* text,int n,const char* rev){ int w=0; const char* p=text; while(*p){ while(*p==' ')p++; if(!*p)break; const char* s=p; while(*p&&*p!=' ')p++; if(++w==n){ std::string word(s,p-s); while(!word.empty()&&strchr(".,!?",word.back())) word.pop_back(); for(auto&c:word) c=toupper(c); std::string r(rev); for(auto&c:r) c=toupper(c); return word==r; } } return 0; }
int main(){
  srand(5); int plays=0, right=0, wrong=0; int per[3]={0,0,0}; std::string seen[3][12]; int nseen[3]={0,0,0};
  mantisPsychicEnter();
  for(int game=0; game<240; game++){
    tick(30); scr=SC_START; modeSel=0; gR=true; tick(2);          // SOLO: the mantis picks a trick
    if(scr!=SC_PLAY){ printf("did not start\n"); return 1; }
    const char* title=fxIdx[0].title; (void)title;
    int trick=-1; int userVal=-1; std::string expectSym, text; int countN=0;
    for(int guard=0; guard<4000 && scr==SC_PLAY; guard++){
      tick();
      char k=beats[beatIdx].kind;
      if(awaitPress){
        if(k=='M'){ trick=0; int n=10+rand()%90; userVal=n-(n/10)-(n%10); expectSym=SYM_NAME[chart[userVal]]; }  // the player reads THEIR cell
        if(k=='Q'){ text=qText; const char* a=argOf(beatIdx); const char* b=strchr(a,'|'); countN=atoi(b+1); trick=(countN==18)?1:2; }
        gR=true; continue; }
      if(k=='R' && revealTxt){
        plays++;
        bool ok=false;
        if(trick==0) ok = std::string(revealTxt)==("THE "+expectSym);
        else ok = nthWordMatches(text.c_str(),countN,revealTxt);
        ok?right++:wrong++; per[trick]++;
        std::string r(revealTxt); bool have=false; for(int i=0;i<nseen[trick];i++) if(seen[trick][i]==r) have=true;
        if(!have && nseen[trick]<12) seen[trick][nseen[trick]++]=r;
        if(!ok) printf("  MISMATCH trick %d: reveal %s\n", trick, revealTxt);
        gR=true; tick(40); break; }
    }
  }
  const char* nm[3]={"CRYSTAL BALL","MANTIS BOOK","BUG JAR"};
  printf("  %d solo plays through the real firmware + real pack: %d correct, %d wrong\n", plays, right, wrong);
  for(int t=0;t<3;t++){ printf("  %-13s %3d plays, %d different reveals:", nm[t], per[t], nseen[t]); for(int i=0;i<nseen[t];i++) printf(" %s%s", seen[t][i].c_str(), i<nseen[t]-1?",":""); printf("\n"); }
  return wrong;
}
