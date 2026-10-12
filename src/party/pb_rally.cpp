// FILE: src/party/pb_rally.cpp
// MANTIS RALLY - a Mario-Party style board game. Everyone sees the same board,
// turns rotate, you roll and move, and the space you land on does something.
//
// Uses the shared modules: pbdice for rolls (deterministic from the match seed +
// turn counter, so every device agrees without sending the number) and pbdeck
// for card draws. Turn flow is the framework's; a REROLL space calls pbKeepTurn
// so the framework doesn't pass the turn on.
//
// Spaces: COIN (+3), THORN (-3), CARD (draw an action card), NEST (buy a star
// for 10 coins), SWAP (trade coin piles with the leader), REROLL (go again).
// First to 3 stars wins the rally.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W=170, H=320;
static constexpr int RL_SPACES=24;
static constexpr int RL_STARS_TO_WIN=3;
static constexpr int STAR_COST=10;

enum SpaceKind : uint8_t { SK_BLANK=0, SK_COIN, SK_THORN, SK_CARD, SK_NEST, SK_SWAP, SK_REROLL };
enum : uint8_t { OP_ROLL=1, OP_BUY=2, OP_SKIP=3 };

static uint8_t kindOf[RL_SPACES];
static int     posn[PB_MAX_PLAYERS];
static int     coins[PB_MAX_PLAYERS];
static int     stars[PB_MAX_PLAYERS];
static int     lastRoll=0, lastMover=-1;
static char    banner[36];
static uint32_t bannerT=0;
static uint32_t animT=0;
static bool    awaitBuy=false;      // landed on a nest with enough coins

// ---- board layout: 24 spaces around a rounded ring --------------------------
static void spaceXY(int i,int&x,int&y){
    // 8 across the top, 4 down each side, 8 across the bottom
    const int L=16, R=W-16, T=96, B=H-52;
    if(i<8){          x=L+(R-L)*i/7;              y=T; }
    else if(i<12){    x=R;                        y=T+(B-T)*(i-7)/5; }
    else if(i<20){    x=R-(R-L)*(i-12)/7;         y=B; }
    else {            x=L;                        y=B-(B-T)*(i-19)/5; }
}

static void rlStart(uint32_t sd,int n,int seat){
    (void)n;(void)seat;
    // deterministic board from the seed so every device draws the same rally
    uint32_t s=sd^0x2545F491u;
    for(int i=0;i<RL_SPACES;i++){
        s=s*1664525u+1013904223u;
        int r=(int)((s>>8)%100);
        if(i%6==0)        kindOf[i]=SK_NEST;      // a nest every sixth space
        else if(r<26)     kindOf[i]=SK_COIN;
        else if(r<44)     kindOf[i]=SK_THORN;
        else if(r<62)     kindOf[i]=SK_CARD;
        else if(r<72)     kindOf[i]=SK_SWAP;
        else if(r<82)     kindOf[i]=SK_REROLL;
        else              kindOf[i]=SK_BLANK;
    }
    for(int i=0;i<PB_MAX_PLAYERS;i++){ posn[i]=0; coins[i]=5; stars[i]=0; }
    lastRoll=0; lastMover=-1; awaitBuy=false;
    banner[0]='\0'; bannerT=0; animT=0;
    pbdice::reset(); pbdeck::shuffle();
}

static int leaderSeat(){
    int best=-1,bs=-1;
    for(int s=0;s<pbPlayerCount();s++){
        int v=stars[s]*1000+coins[s];
        if(v>bs){ bs=v; best=s; }
    }
    return best;
}
static void say(const char* t){ snprintf(banner,sizeof(banner),"%s",t); bannerT=millis(); }

// an action card: uses the shared deck so both devices draw the same one
static void drawActionCard(int seat){
    int c=pbdeck::draw();
    if(c<0){ pbdeck::shuffle(); c=pbdeck::draw(); }
    int kind=c%5;
    switch(kind){
    case 0: coins[seat]+=6; say("CARD: found a cache +6"); break;
    case 1: coins[seat]-=4; if(coins[seat]<0)coins[seat]=0; say("CARD: molted, lost 4"); break;
    case 2: { int j=1+(c/5)%4; posn[seat]=(posn[seat]+j)%RL_SPACES; say("CARD: leap forward"); break; }
    case 3: { int l=leaderSeat(); if(l>=0&&l!=seat&&coins[l]>=3){ coins[l]-=3; coins[seat]+=3; say("CARD: ambush the leader"); } else say("CARD: nothing to ambush"); break; }
    default: coins[seat]+=2; say("CARD: small forage +2"); break;
    }
}

static void landOn(int seat){
    int sp=posn[seat];
    switch(kindOf[sp]){
    case SK_COIN:  coins[seat]+=3; say("+3 coins"); break;
    case SK_THORN: coins[seat]-=3; if(coins[seat]<0)coins[seat]=0; say("-3 coins, ouch"); break;
    case SK_CARD:  drawActionCard(seat); break;
    case SK_SWAP: {
        int l=leaderSeat();
        if(l>=0&&l!=seat){ int t=coins[seat]; coins[seat]=coins[l]; coins[l]=t; say("swapped piles!"); }
        else say("nobody to swap with");
        break; }
    case SK_REROLL: say("roll again!"); pbKeepTurn(); break;
    case SK_NEST:
        if(coins[seat]>=STAR_COST){
            if(seat==pbMySeat()){ awaitBuy=true; say("a nest! buy a star?"); }
            else say("nest reached");
            pbKeepTurn();      // hold the turn while the owner decides
        } else say("a nest, but too few coins");
        break;
    default: say("quiet ground"); break;
    }
}

static void rlApply(const PbMove* m){
    if(m->len<1) return;
    int seat=m->player; if(seat<0||seat>=PB_MAX_PLAYERS) return;
    switch(m->data[0]){
    case OP_ROLL: {
        int r=m->len>=2?m->data[1]:1;
        lastRoll=r; lastMover=seat;
        posn[seat]=(posn[seat]+r)%RL_SPACES;
        landOn(seat);
        break; }
    case OP_BUY:
        if(coins[seat]>=STAR_COST){ coins[seat]-=STAR_COST; stars[seat]++; say("bought a star!"); }
        awaitBuy=false;
        break;
    case OP_SKIP:
        awaitBuy=false; say("left the nest");
        break;
    }
}

static void rlTick(){
    animT++;
    bool lE=btnL&&!btnL_prev, rE=btnR&&!btnR_prev;
    if(!pbMyTurn()) return;

    if(awaitBuy){
        if(rE){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_BUY;
            pbSubmitMove(&mv);
        } else if(lE){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_SKIP;
            pbSubmitMove(&mv);
        }
        return;
    }
    if(rE){
        int r=pbdice::roll(6);
        PbMove mv; memset(&mv,0,sizeof(mv));
        mv.player=(uint8_t)pbMySeat(); mv.len=2;
        mv.data[0]=OP_ROLL; mv.data[1]=(uint8_t)r;
        pbSubmitMove(&mv);
    }
}

static bool rlOver(int* winner){
    for(int s=0;s<pbPlayerCount();s++) if(stars[s]>=RL_STARS_TO_WIN){ *winner=s; return true; }
    return false;
}
static void rlEnd(int w){ (void)w; }

// ---- draw -------------------------------------------------------------------
static uint16_t kindCol(uint8_t k){
    switch(k){
    case SK_COIN:   return LIME;
    case SK_THORN:  return PURPLE_BRIGHT;
    case SK_CARD:   return TEAL_BRIGHT;
    case SK_NEST:   return LIME_PALE;
    case SK_SWAP:   return PURPLE_LIGHT;
    case SK_REROLL: return TEAL_LIGHT;
    default:        return TEAL;
    }
}
static uint16_t seatCol(int s){
    switch(s%4){ case 0:return LIME; case 1:return PURPLE_LIGHT; case 2:return TEAL_BRIGHT; default:return LIME_MED; }
}
static void drawDie(int cx,int cy,int v){
    fb.fillRoundRect(cx-11,cy-11,22,22,4,LIME_PALE);
    fb.drawRoundRect(cx-11,cy-11,22,22,4,TEAL_DEEP);
    const int8_t P[7][6][2]={
        {{0,0}},{{0,0}},{{-5,-5},{5,5}},{{-5,-5},{0,0},{5,5}},
        {{-5,-5},{5,-5},{-5,5},{5,5}},{{-5,-5},{5,-5},{0,0},{-5,5},{5,5}},
        {{-5,-5},{5,-5},{-5,0},{5,0},{-5,5},{5,5}}};
    int n = v<1?1:(v>6?6:v);
    int cnt = n==1?1:n==2?2:n==3?3:n==4?4:n==5?5:6;
    for(int i=0;i<cnt;i++) fb.fillCircle(cx+P[n][i][0],cy+P[n][i][1],2,TEAL_DEEP);
}

static void rlDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS RALLY",W/2,6,2);
    // player strip: stars + coins
    for(int s=0;s<pbPlayerCount()&&s<4;s++){
        int x=4+s*41;
        bool turn=(s==pbWhoseTurn());
        fb.fillRoundRect(x,26,38,30,3,turn?PURPLE_DARK:TEAL_DARK);
        if(s==pbMySeat()) fb.drawRoundRect(x,26,38,30,3,LIME);
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(seatCol(s));
        char b[10]; snprintf(b,sizeof(b),"P%d",s+1); fb.drawString(b,x+19,28,1);
        fb.setTextColor(LIME_PALE); snprintf(b,sizeof(b),"*%d",stars[s]); fb.drawString(b,x+11,40,1);
        fb.setTextColor(LIME);      snprintf(b,sizeof(b),"%d",coins[s]);  fb.drawString(b,x+28,40,1);
    }
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_DARK);
    char hd[30]; snprintf(hd,sizeof(hd),"first to %d stars   *%d = star",RL_STARS_TO_WIN,STAR_COST);
    fb.drawString(hd,W/2,60,1);

    // the ring of spaces
    for(int i=0;i<RL_SPACES;i++){
        int x,y; spaceXY(i,x,y);
        uint16_t c=kindCol(kindOf[i]);
        if(kindOf[i]==SK_NEST){ fb.fillCircle(x,y,7,c); fb.drawCircle(x,y,7,TEAL_DEEP);
            fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DEEP); fb.drawString("*",x,y,1); }
        else { fb.fillCircle(x,y,5,c); fb.drawCircle(x,y,5,TEAL_DEEP); }
    }
    // tokens
    for(int s=0;s<pbPlayerCount();s++){
        int x,y; spaceXY(posn[s],x,y);
        int off=(s%2? 6:-6), off2=(s<2? -6:6);
        fb.fillCircle(x+off,y+off2,3,seatCol(s));
        fb.drawCircle(x+off,y+off2,3,TEAL_DEEP);
    }
    // die + banner in the middle
    if(lastMover>=0) drawDie(W/2,(H-52+96)/2-14,lastRoll);
    if(banner[0] && millis()-bannerT<2600){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(banner,W/2,(H-52+96)/2+14,1);
    }

    fb.setTextDatum(BC_DATUM);
    if(pbMyTurn()){
        if(awaitBuy){ fb.setTextColor(LIME_PALE); fb.drawString("R: buy star   L: skip",W/2,H-6,1); }
        else { fb.setTextColor(LIME); fb.drawString("R: roll",W/2,H-6,1); }
    } else {
        fb.setTextColor(TEAL_DARK);
        char b[30]; snprintf(b,sizeof(b),"%s is rolling",pbPlayerName(pbWhoseTurn()));
        fb.drawString(b,W/2,H-6,1);
    }
}

BoardGame RALLY_GAME = {
    "MANTIS RALLY", 8, 2, 4,
    rlStart, rlTick, rlApply, rlDraw, rlOver, rlEnd
};
