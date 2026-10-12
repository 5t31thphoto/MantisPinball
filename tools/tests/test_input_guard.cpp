#include <cstdio>
#include <cstdint>
enum AppState { STATE_PLAYING, STATE_WEATHER, STATE_SPACE, STATE_RABBIT, STATE_MINER, STATE_APOD, STATE_MOON, STATE_DICE,
 STATE_CLICKER, STATE_SETUP, STATE_SHARE, STATE_RUBIKS, STATE_SCOUT, STATE_SURVIVAL, STATE_FIRSTAID, STATE_FRUIT, STATE_PARTY };
static uint32_t g_ms=1000; static uint32_t millis(){ return g_ms; }
bool btnL=false,btnR=false,btnL_prev=false,btnR_prev=false; static AppState appState=STATE_WEATHER;
// ---------------- APP INPUT GUARD ----------------
// Wrapped around the app dispatch ONLY. The exit double-click and the sleep
// hold above still read the raw buttons; the raw values are restored after
// the app's tick, so those detectors never see the mask.
//
//  * ENTRY LATCH (every app): the menu selects on PRESS, so an app starts with
//    R still held. Until both buttons have been released once, the app sees
//    neither - the selecting press can never leak into it.
//  * CHORD GUARD (apps that never need both buttons and have no guard of their
//    own): a both-buttons chord - the menu double-click - is hidden from the
//    app, plus 350 ms after release to swallow the second click. Same rule as
//    chord_guard.cpp. Pinball and the other both-button games are not listed,
//    so both buttons always work there right up until you leave.
static bool     g_entryLatch = false;
static bool     g_cgPoison = false;
static uint32_t g_cgReleasedAt = 0;
static bool     g_rawL, g_rawR, g_rawLp, g_rawRp, g_appL, g_appR;

static bool centralChordGuard(AppState s) {
    switch (s) {
    case STATE_WEATHER: case STATE_SPACE: case STATE_RABBIT: case STATE_MINER:
    case STATE_APOD:    case STATE_MOON:  case STATE_DICE:   case STATE_CLICKER:
    case STATE_SETUP:   case STATE_SHARE: case STATE_RUBIKS: case STATE_SCOUT:
    case STATE_SURVIVAL: case STATE_FIRSTAID: case STATE_FRUIT: case STATE_PARTY:
        return true;
    default:
        return false;      // pinball family, action games, and apps with their own guard
    }
}

static void appInputGuardBegin() {
    g_rawL = btnL; g_rawR = btnR; g_rawLp = btnL_prev; g_rawRp = btnR_prev;
    bool vl = btnL, vr = btnR;
    if (g_entryLatch) {
        if (!g_rawL && !g_rawR) g_entryLatch = false;
        else vl = vr = false;
    }
    if (centralChordGuard(appState)) {
        const uint32_t now = millis();
        if (g_rawL && g_rawR) g_cgPoison = true;
        if (g_cgPoison) {
            if (!g_rawL && !g_rawR) {
                if (!g_cgReleasedAt) g_cgReleasedAt = now;
                if (now - g_cgReleasedAt > 350) { g_cgPoison = false; g_cgReleasedAt = 0; }
            } else g_cgReleasedAt = 0;
            vl = vr = false;
        }
    } else { g_cgPoison = false; g_cgReleasedAt = 0; }
    btnL_prev = g_appL; btnR_prev = g_appR;   // the app's edges come from what IT saw
    btnL = vl; btnR = vr;
    g_appL = vl; g_appR = vr;
}

static void appInputGuardEnd() {
    btnL = g_rawL; btnR = g_rawR; btnL_prev = g_rawLp; btnR_prev = g_rawRp;
}


static int appEdgesL=0, appEdgesR=0, appHeldBoth=0;
static void frame(bool l, bool r){
  btnL_prev=btnL; btnR_prev=btnR; btnL=l; btnR=r; g_ms+=16;          // readInput()
  bool rawEdge = (btnL&&!btnL_prev)||(btnR&&!btnR_prev);            // what the exit detector sees
  (void)rawEdge;
  appInputGuardBegin();
  if (btnL && !btnL_prev) appEdgesL++; if (btnR && !btnR_prev) appEdgesR++; if (btnL && btnR) appHeldBoth++;
  appInputGuardEnd();
}
static void reset(AppState s){ appState=s; appEdgesL=appEdgesR=appHeldBoth=0; }
int main(){
  int fails=0; auto check=[&](bool ok,const char* w){ printf("  %s %s\n", ok?"ok  ":"FAIL", w); if(!ok) fails++; };
  // 1. entry: the menu selected on R press, R still held as the app starts
  reset(STATE_WEATHER); btnR=true; btnR_prev=false; g_entryLatch=true;
  for(int i=0;i<10;i++) frame(false,true); for(int i=0;i<5;i++) frame(false,false);
  check(appEdgesR==0, "the menu's selecting R press never reaches the app");
  for(int i=0;i<3;i++) frame(false,true); for(int i=0;i<3;i++) frame(false,false);
  check(appEdgesR==1, "the next real R tap does");
  // 2. double-click exit in a guarded app: no stray actions
  reset(STATE_WEATHER);
  for(int c=0;c<2;c++){ frame(true,false); for(int i=0;i<4;i++) frame(true,true); for(int i=0;i<4;i++) frame(false,false); }
  check(appEdgesR==0 && appHeldBoth==0, "double-click both buttons: the guarded app sees no R and no chord");
  for(int i=0;i<30;i++) frame(false,false); for(int i=0;i<3;i++) frame(false,true); for(int i=0;i<3;i++) frame(false,false);
  check(appEdgesR==1, "after the cooldown, a normal tap works again");
  // 3. pinball: both buttons are game controls, never hidden
  reset(STATE_PLAYING);
  for(int i=0;i<5;i++) frame(true,true); for(int i=0;i<3;i++) frame(false,false);
  check(appHeldBoth==5 && appEdgesL==1 && appEdgesR==1, "pinball sees both flippers held together (not guarded)");
  // 4. raw values are restored for the exit/sleep detectors
  reset(STATE_WEATHER); frame(true,true);
  check(btnL && btnR, "after the app tick, main.cpp sees the RAW buttons again (exit gesture intact)");
  printf(fails?"\n%d FAILED\n":"\nENTRY LATCH + CHORD GUARD BEHAVE\n",fails); return fails; }
