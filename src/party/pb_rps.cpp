// FILE: src/party/pb_rps.cpp
// EXTREME MANTIS RPS - five throws instead of three, best-of-five rounds.
// Throws: STRIKE (rock) / SPREAD (paper) / SLICE (scissors) / SNAP (beats
// spread+slice, loses to strike+stare) / STARE (beats strike+snap, loses to
// spread+slice). Every throw beats two and loses to two, so it stays fair.
//
// Picks are HIDDEN until both are in: on a shared device the screen masks your
// choice while you scroll, and over the link only the committed move goes out.
// Move payload: data[0] = throw index.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W=170, H=320;

enum { T_STRIKE=0, T_SPREAD, T_SLICE, T_SNAP, T_STARE, T_COUNT };
static const char* TNAME[T_COUNT]={"STRIKE","SPREAD","SLICE","SNAP","STARE"};

// beats[a][b] = true if a beats b
static bool beats(int a,int b){
    if(a==b) return false;
    // each throw beats the next two cyclically (5-cycle keeps it balanced)
    return ((a+1)%T_COUNT==b) || ((a+2)%T_COUNT==b);
}

static int  sel=0;                 // local scroll position
static int8_t thrown[PB_MAX_PLAYERS];
static int  wins[PB_MAX_PLAYERS];
static int  roundNo=1, revealT=0;
static bool revealing=false;
static uint32_t animT=0;

static void rpsStart(uint32_t s,int n,int seat){
    (void)s;(void)n;(void)seat;
    memset(wins,0,sizeof(wins));
    for(int i=0;i<PB_MAX_PLAYERS;i++) thrown[i]=-1;
    sel=0; roundNo=1; revealing=false; revealT=0; animT=0;
}
static void rpsApply(const PbMove* m){
    if(m->len<1) return;
    int t=m->data[0]; if(t<0||t>=T_COUNT) return;
    if(m->player<PB_MAX_PLAYERS) thrown[m->player]=(int8_t)t;
    // in RPS everyone throws each round, so the submitter keeps the turn until
    // all throws are in; the framework advances seat-by-seat which is what we
    // want for pass-and-play (each player commits in turn, hidden).
}
static void rpsTick(){
    animT++;
    if(revealing){
        revealT++;
        if(revealT>110){
            // score the round
            int n=pbPlayerCount();
            for(int a=0;a<n;a++)for(int b=0;b<n;b++) if(a!=b&&thrown[a]>=0&&thrown[b]>=0)
                if(beats(thrown[a],thrown[b])) wins[a]++;
            for(int i=0;i<PB_MAX_PLAYERS;i++) thrown[i]=-1;
            revealing=false; revealT=0; roundNo++;
        }
        return;
    }
    // every device checks independently whether all throws are in, so the
    // reveal fires the same way whether the last throw was local or remote
    bool all=true;
    for(int i=0;i<pbPlayerCount();i++) if(thrown[i]<0){ all=false; break; }
    if(all){ revealing=true; revealT=0; return; }

    if(!pbMyTurn()) return;
    if(btnL&&!btnL_prev) sel=(sel+1)%T_COUNT;
    if(btnR&&!btnR_prev){
        PbMove mv; memset(&mv,0,sizeof(mv));
        mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=(uint8_t)sel;
        pbSubmitMove(&mv);
    }
}
static bool rpsOver(int* winner){
    for(int i=0;i<pbPlayerCount();i++) if(wins[i]>=3){ *winner=i; return true; }
    return false;
}
static void rpsEnd(int w){ (void)w; }

// draw a mantis pose per throw
static uint16_t seatColSafe(){ return pbMySeat()==0?LIME:PURPLE_LIGHT; }
static void drawThrow(int cx,int cy,int t,uint16_t c,float wig){
    int a=(int)(sinf(wig)*3);
    fb.fillRoundRect(cx-10,cy-4,20,10,3,c);                 // thorax
    fb.fillTriangle(cx+6,cy-6, cx+16,cy-10, cx+16,cy-2, c); // head
    fb.fillCircle(cx+13,cy-7,1,TEAL_DEEP);
    switch(t){
        case T_STRIKE:  // both arms cocked back, fist-like
            fb.fillRoundRect(cx-2,cy-14,7,7,2,c); fb.fillRoundRect(cx+4,cy-12,7,7,2,c); break;
        case T_SPREAD:  // arms wide open
            fb.drawLine(cx-2,cy-2,cx-18,cy-12+a,c); fb.drawLine(cx+2,cy-2,cx+18,cy-14-a,c);
            fb.drawLine(cx-2,cy,cx-16,cy+6,c); fb.drawLine(cx+2,cy,cx+16,cy+4,c); break;
        case T_SLICE:   // two blades out
            fb.drawLine(cx,cy-4,cx+14,cy-16+a,c); fb.drawLine(cx+2,cy-2,cx+18,cy-8,c);
            fb.drawLine(cx+14,cy-16+a,cx+10,cy-12,c); break;
        case T_SNAP:    // one arm forward snapping
            fb.fillRoundRect(cx+8,cy-8+a,10,5,2,c); fb.drawLine(cx+2,cy-2,cx+8,cy-6+a,c); break;
        default:        // STARE - reared up, still
            fb.drawLine(cx-2,cy-4,cx-6,cy-16,c); fb.drawLine(cx+2,cy-4,cx+6,cy-16,c);
            fb.drawCircle(cx+13,cy-7,3,LIME_PALE); break;
    }
    fb.drawLine(cx-6,cy+6,cx-10,cy+12,c); fb.drawLine(cx+4,cy+6,cx+8,cy+12,c);
}

static void rpsDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("EXTREME RPS",W/2,8,2);
    char b[28];
    fb.setTextColor(TEAL_LIGHT); snprintf(b,sizeof(b),"ROUND %d   first to 3",roundNo);
    fb.drawString(b,W/2,28,1);
    // score strip
    for(int i=0;i<pbPlayerCount()&&i<4;i++){
        int x=8+i*40;
        fb.fillRoundRect(x,44,36,18,3,i==pbMySeat()?PURPLE_DARK:TEAL_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        snprintf(b,sizeof(b),"%d",wins[i]); fb.drawString(b,x+18,53,2);
    }
    if(revealing){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
        fb.drawString("THROW!",W/2,74,4);
        int n=pbPlayerCount();
        for(int i=0;i<n&&i<2;i++){
            int cy=140+i*80;
            drawThrow(W/2-20+ (i?40:0), cy, thrown[i]<0?0:thrown[i],
                      i==0?LIME:PURPLE_LIGHT, (float)animT*0.2f+i);
            fb.setTextDatum(TC_DATUM); fb.setTextColor(i==0?LIME:PURPLE_LIGHT);
            fb.drawString(thrown[i]<0?"?":TNAME[thrown[i]],W/2,cy+22,1);
        }
    } else if(pbMyTurn()){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("pick in secret",W/2,74,1);
        drawThrow(W/2,150,sel,seatColSafe(),(float)animT*0.2f);
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
        fb.drawString(TNAME[sel],W/2,190,4);
        // what it beats / loses to, so the 5-throw wheel is learnable
        fb.setTextColor(TEAL_LIGHT);
        snprintf(b,sizeof(b),"beats %s, %s",TNAME[(sel+1)%T_COUNT],TNAME[(sel+2)%T_COUNT]);
        fb.drawString(b,W/2,220,1);
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"loses to %s, %s",TNAME[(sel+3)%T_COUNT],TNAME[(sel+4)%T_COUNT]);
        fb.drawString(b,W/2,234,1);
    } else {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("hiding your pick...",W/2,150,2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString(pbPlayerName(pbWhoseTurn()),W/2,175,1);
        fb.drawString("is choosing",W/2,190,1);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString(revealing?"":(pbMyTurn()?"L: cycle   R: throw":"pass the device"),W/2,H-6,1);
}

BoardGame RPS_GAME = {
    "EXTREME RPS", 3, 2, 4,
    rpsStart, rpsTick, rpsApply, rpsDraw, rpsOver, rpsEnd
};
