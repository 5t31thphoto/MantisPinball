// FILE: src/party/pb_battleship.cpp
// MANTIS FLEET (Battleship) - turn-based FREE-FOR-ALL on ONE common hidden board.
//
// THE DESIGN, per spec:
//   * Every player places their own fleet locally on a shared 8x8 grid. A
//     layout is sent ONLY to the host device - never peer-to-peer - so no
//     opponent's screen can ever show it.
//   * The HOST DEVICE is an invisible referee: it holds the full board in
//     memory but renders exactly the same view as everyone else (own ships +
//     own shot history). The host PLAYER gains no information advantage.
//   * Turn-based, one shot per turn in rotation. A shot goes to the referee,
//     which adjudicates and then announces "HIT" or "MISS" at that cell to the
//     whole table WITHOUT saying whose ship it was, and privately tells the
//     owner they were hit so they can announce it or bluff.
//   * Last player with a boat afloat wins, then the referee reveals the whole
//     board and everyone sees it.
//
// Controls (house idiom, via the shared pbgrid module): L scrolls the letter
// then the number, long-L rotates the ship being placed, R places / fires.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W=170, H=320;
static constexpr int BS_N=8;                 // 8x8 common board
static constexpr int BS_CELL=17;
static constexpr int BS_X=(W-BS_N*BS_CELL)/2;    // 17
static constexpr int BS_Y=88;
static constexpr int BS_FLEET=3;             // ships per player
static const int SHIP_LEN[BS_FLEET]={3,2,2};

// ---- move opcodes (data[0]) -------------------------------------------------
enum : uint8_t { OP_LAYOUT=1, OP_SHOT=2, OP_RESULT=3, OP_SUNK=4, OP_HITME=5, OP_REVEAL=6 };

// ---- local (private) state --------------------------------------------------
static uint8_t myCells[BS_FLEET*3];   // my ship cells, 3 max per ship, 0xFF unused
static uint8_t myHit[BS_FLEET*3];     // 1 once that cell has been hit
static int     placeIdx=0;            // which ship I'm placing
static bool    placing=true, horiz=true;
static bool    layoutSent=false;

// what THIS device knows about the shared board (public knowledge)
static uint8_t shotMark[BS_N*BS_N];   // 0 unknown, 1 miss, 2 hit (owner unknown)
static bool    iWasHitFlash=false; static uint32_t hitFlashT=0;
static int     aliveCount=0;          // referee's view; others learn from SUNK

// ---- referee-only state (host device) --------------------------------------
static uint8_t refCell[PB_MAX_PLAYERS][BS_FLEET*3];  // every player's ship cells
static uint8_t refHit [PB_MAX_PLAYERS][BS_FLEET*3];
static bool    refHave[PB_MAX_PLAYERS];              // layout received?
static bool    refAlive[PB_MAX_PLAYERS];
static bool    revealed=false;
static uint8_t revealOwner[BS_N*BS_N];               // filled at reveal: seat+1

static uint32_t animT=0;
static int  winner=-1;

static inline int cellOf(int x,int y){ return y*BS_N+x; }

static void bsStart(uint32_t sd,int n,int seat){
    (void)sd;(void)n;(void)seat;
    memset(myCells,0xFF,sizeof(myCells)); memset(myHit,0,sizeof(myHit));
    memset(shotMark,0,sizeof(shotMark));
    memset(refHave,0,sizeof(refHave)); memset(refHit,0,sizeof(refHit));
    memset(revealOwner,0,sizeof(revealOwner));
    for(int i=0;i<PB_MAX_PLAYERS;i++) refAlive[i]=true;
    placeIdx=0; placing=true; horiz=true; layoutSent=false;
    revealed=false; winner=-1; iWasHitFlash=false;
    aliveCount=pbPlayerCount();
    pbgrid::begin(BS_N,BS_N);
}

// ---- placement helpers ------------------------------------------------------
static bool myOccupies(int c){
    for(int i=0;i<BS_FLEET*3;i++) if(myCells[i]==c) return true;
    return false;
}
static bool canPlace(int x,int y,int len,bool h){
    for(int k=0;k<len;k++){
        int cx=h?x+k:x, cy=h?y:y+k;
        if(cx>=BS_N||cy>=BS_N) return false;
        if(myOccupies(cellOf(cx,cy))) return false;
    }
    return true;
}
static void doPlace(int x,int y,int len,bool h){
    for(int k=0;k<len;k++){
        int cx=h?x+k:x, cy=h?y:y+k;
        myCells[placeIdx*3+k]=(uint8_t)cellOf(cx,cy);
    }
}

// ---- referee adjudication ---------------------------------------------------
static bool refShipSunk(int seat,int ship){
    for(int k=0;k<3;k++){
        uint8_t c=refCell[seat][ship*3+k];
        if(c==0xFF) continue;
        if(!refHit[seat][ship*3+k]) return false;
    }
    return true;
}
static bool refSeatAlive(int seat){
    for(int i=0;i<BS_FLEET*3;i++){
        if(refCell[seat][i]==0xFF) continue;
        if(!refHit[seat][i]) return true;
    }
    return false;
}
// The referee resolves a shot: mark it, announce hit/miss WITHOUT the owner,
// privately notify the owner, and check for eliminations.
static void refAdjudicate(int shooter,int cell){
    int ownerSeat=-1, ownerShip=-1;
    for(int s=0;s<pbPlayerCount()&&ownerSeat<0;s++){
        if(s==shooter||!refHave[s]) continue;
        for(int i=0;i<BS_FLEET*3;i++){
            if(refCell[s][i]==(uint8_t)cell){ ownerSeat=s; ownerShip=i/3; refHit[s][i]=1; break; }
        }
    }
    bool hit = ownerSeat>=0;
    // announce to the table: the cell and hit/miss, never the owner
    PbMove res; memset(&res,0,sizeof(res));
    res.player=(uint8_t)shooter; res.len=3;
    res.data[0]=OP_RESULT; res.data[1]=(uint8_t)cell; res.data[2]=hit?1:0;
    pbBroadcastInfo(&res);
    shotMark[cell]=hit?2:1;      // apply on the referee's own screen too

    if(hit){
        // privately tell the owner it was their ship
        PbMove pm; memset(&pm,0,sizeof(pm));
        pm.player=(uint8_t)ownerSeat; pm.len=2;
        pm.data[0]=OP_HITME; pm.data[1]=(uint8_t)cell;
        if(ownerSeat==pbMySeat()){ iWasHitFlash=true; hitFlashT=millis();
            for(int i=0;i<BS_FLEET*3;i++) if(myCells[i]==(uint8_t)cell) myHit[i]=1; }
        else pbSendToSeat(ownerSeat,&pm);

        // a sunk ship is public (everyone hears a boat go down), and so is an
        // elimination - but still not whose cell was where
        if(refShipSunk(ownerSeat,ownerShip)){
            bool stillAlive=refSeatAlive(ownerSeat);
            refAlive[ownerSeat]=stillAlive;
            PbMove sk; memset(&sk,0,sizeof(sk));
            sk.player=(uint8_t)ownerSeat; sk.len=2;
            sk.data[0]=OP_SUNK; sk.data[1]=stillAlive?0:1;
            pbBroadcastInfo(&sk);
            aliveCount=0; for(int s=0;s<pbPlayerCount();s++) if(refAlive[s]) aliveCount++;
        }
    }
}

static void bsApply(const PbMove* m){
    if(m->len<1) return;
    switch(m->data[0]){
    case OP_LAYOUT: {
        // referee stores a player's fleet; nobody else ever sees this message
        if(!pbIsHost()) return;
        int s=m->player; if(s<0||s>=PB_MAX_PLAYERS) return;
        for(int i=0;i<BS_FLEET*3 && i+1<PB_MOVE_BYTES;i++) refCell[s][i]=m->data[1+i];
        refHave[s]=true; refAlive[s]=true;
        break; }
    case OP_SHOT: {
        if(pbIsHost() && m->len>=2) refAdjudicate(m->player,m->data[1]);
        break; }
    case OP_RESULT: {
        if(m->len>=3){ int c=m->data[1]; if(c<BS_N*BS_N) shotMark[c]=m->data[2]?2:1; }
        break; }
    case OP_HITME: {
        // the referee telling ME my ship was hit
        if(m->len>=2 && (int)m->player==pbMySeat()){
            iWasHitFlash=true; hitFlashT=millis();
            for(int i=0;i<BS_FLEET*3;i++) if(myCells[i]==m->data[1]) myHit[i]=1;
        }
        break; }
    case OP_SUNK: {
        if(m->len>=2 && m->data[1]) { if(aliveCount>0) aliveCount--; }
        break; }
    case OP_REVEAL: {
        // The 64-cell owner map is sent in chunks (22 cells per 24-byte move),
        // so every device really does see the whole board at the end.
        if(m->len>=3){
            int chunk=m->data[1];
            int base=chunk*22;
            for(int i=0;i<22 && base+i<BS_N*BS_N && 2+i<PB_MOVE_BYTES;i++)
                revealOwner[base+i]=m->data[2+i];
        }
        revealed=true;
        break; }
    }
}

static void bsTick(){
    animT++;
    if(revealed) return;

    // --- placement phase: everyone places at once, then waits ---
    if(placing){
        pbgrid::tick();
        if(pbgrid::rotated()) horiz=!horiz;
        if(pbgrid::selected()){
            int x=pbgrid::cx(), y=pbgrid::cy(), len=SHIP_LEN[placeIdx];
            if(canPlace(x,y,len,horiz)){
                doPlace(x,y,len,horiz);
                placeIdx++;
                if(placeIdx>=BS_FLEET){
                    placing=false;
                    // send my fleet to the REFEREE ONLY
                    PbMove mv; memset(&mv,0,sizeof(mv));
                    mv.player=(uint8_t)pbMySeat(); mv.len=1+BS_FLEET*3;
                    mv.data[0]=OP_LAYOUT;
                    for(int i=0;i<BS_FLEET*3;i++) mv.data[1+i]=myCells[i];
                    if(pbIsHost()) bsApply(&mv);   // host IS the referee
                    else pbSendToHost(&mv);        // secret: host only, never peer-to-peer
                    layoutSent=true;
                    pbgrid::begin(BS_N,BS_N);
                }
            }
        }
        return;
    }
    // --- firing phase ---
    if(!pbMyTurn()) return;
    pbgrid::tick();
    if(pbgrid::selected()){
        int c=cellOf(pbgrid::cx(),pbgrid::cy());
        if(shotMark[c]==0 && !myOccupies(c)){     // no re-shooting, no self-fire
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=2;
            mv.data[0]=OP_SHOT; mv.data[1]=(uint8_t)c;
            pbSubmitMove(&mv);                     // referee adjudicates on receipt
        }
    }
}

static bool bsOver(int* w){
    if(!pbIsHost()) return false;      // only the referee can know
    int alive=0, last=-1;
    for(int s=0;s<pbPlayerCount();s++) if(refHave[s]&&refSeatAlive(s)){ alive++; last=s; }
    bool allIn=true; for(int s=0;s<pbPlayerCount();s++) if(!refHave[s]) allIn=false;
    if(allIn && alive<=1){ *w=last; winner=last; return true; }
    return false;
}
static void bsEnd(int w){
    winner=w;
    // the referee reveals the full board so everyone sees where every fleet was
    if(pbIsHost()){
        for(int s=0;s<pbPlayerCount();s++) if(refHave[s])
            for(int i=0;i<BS_FLEET*3;i++){
                uint8_t c=refCell[s][i];
                if(c<BS_N*BS_N) revealOwner[c]=(uint8_t)(s+1);
            }
        for(int chunk=0;chunk<3;chunk++){
            PbMove rv; memset(&rv,0,sizeof(rv));
            rv.player=(uint8_t)pbMySeat(); rv.data[0]=OP_REVEAL; rv.data[1]=(uint8_t)chunk;
            int base=chunk*22, n=0;
            for(int i=0;i<22 && base+i<BS_N*BS_N;i++){ rv.data[2+i]=revealOwner[base+i]; n++; }
            rv.len=(uint8_t)(2+n);
            pbBroadcastInfo(&rv);
            delay(6);   // let each chunk clear the air
        }
    }
    revealed=true;
}

// ---- draw -------------------------------------------------------------------
static uint16_t seatTint(int s){
    switch(s%4){ case 0: return LIME; case 1: return PURPLE_LIGHT;
                 case 2: return TEAL_BRIGHT; default: return LIME_MED; }
}
static void bsDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS FLEET",W/2,6,2);
    char b[30];
    if(placing){
        fb.setTextColor(TEAL_LIGHT);
        snprintf(b,sizeof(b),"place boat %d of %d  (len %d)",placeIdx+1,BS_FLEET,SHIP_LEN[placeIdx]);
        fb.drawString(b,W/2,26,1);
        fb.setTextColor(horiz?LIME:PURPLE_LIGHT);
        fb.drawString(horiz?"across":"down",W/2,40,1);
    } else if(!revealed){
        fb.setTextColor(pbMyTurn()?LIME:TEAL_LIGHT);
        fb.drawString(pbMyTurn()?"YOUR SHOT":pbPlayerName(pbWhoseTurn()),W/2,26,1);
        fb.setTextColor(TEAL_DARK);
        snprintf(b,sizeof(b),"%d fleets afloat",aliveCount);
        fb.drawString(b,W/2,40,1);
    } else {
        fb.setTextColor(LIME); fb.drawString("FULL REVEAL",W/2,26,1);
    }
    // hit flash on my own fleet
    if(iWasHitFlash && millis()-hitFlashT<1400){
        fb.setTextColor((animT/6)%2?PURPLE_BRIGHT:LIME_PALE);
        fb.setTextDatum(TC_DATUM); fb.drawString("YOUR BOAT IS HIT!",W/2,54,1);
    }
    // column letters / row numbers
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL);
    for(int i=0;i<BS_N;i++){
        char cl[2]={(char)('A'+i),0};
        fb.drawString(cl,BS_X+i*BS_CELL+BS_CELL/2,BS_Y-8,1);
        char rn[3]; snprintf(rn,sizeof(rn),"%d",i+1);
        fb.drawString(rn,BS_X-9,BS_Y+i*BS_CELL+BS_CELL/2,1);
    }
    // the grid
    for(int y=0;y<BS_N;y++)for(int x=0;x<BS_N;x++){
        int c=cellOf(x,y);
        int px=BS_X+x*BS_CELL, py=BS_Y+y*BS_CELL;
        uint16_t fill=TEAL_DARK;
        if(shotMark[c]==1) fill=TEAL;              // known miss
        if(shotMark[c]==2) fill=PURPLE_DARK;       // known hit (owner unknown)
        fb.fillRect(px+1,py+1,BS_CELL-2,BS_CELL-2,fill);
        // my own ships are always visible to me
        if(myOccupies(c)){
            bool wounded=false;
            for(int i=0;i<BS_FLEET*3;i++) if(myCells[i]==c&&myHit[i]) wounded=true;
            fb.fillRoundRect(px+3,py+3,BS_CELL-6,BS_CELL-6,2,wounded?PURPLE_BRIGHT:LIME_DARK);
        }
        if(shotMark[c]==2) { fb.drawLine(px+3,py+3,px+BS_CELL-4,py+BS_CELL-4,PURPLE_LIGHT);
                             fb.drawLine(px+BS_CELL-4,py+3,px+3,py+BS_CELL-4,PURPLE_LIGHT); }
        else if(shotMark[c]==1) fb.fillCircle(px+BS_CELL/2,py+BS_CELL/2,2,TEAL_LIGHT);
        // the end-of-game reveal shows every fleet, tinted per player
        if(revealed && revealOwner[c]) fb.drawRoundRect(px+2,py+2,BS_CELL-4,BS_CELL-4,2,seatTint(revealOwner[c]-1));
        fb.drawRect(px,py,BS_CELL,BS_CELL,TEAL_DEEP);
    }
    // cursor + placement preview
    if(!revealed && (placing || pbMyTurn())){
        int x=pbgrid::cx(), y=pbgrid::cy();
        if(placing){
            int len=SHIP_LEN[placeIdx]; bool ok=canPlace(x,y,len,horiz);
            for(int k=0;k<len;k++){
                int cx=horiz?x+k:x, cy=horiz?y:y+k;
                if(cx>=BS_N||cy>=BS_N) continue;
                fb.drawRoundRect(BS_X+cx*BS_CELL+1,BS_Y+cy*BS_CELL+1,BS_CELL-2,BS_CELL-2,2,
                                 ok?LIME:PURPLE_BRIGHT);
            }
        } else {
            uint16_t c=(animT/8)%2?LIME_PALE:LIME;
            fb.drawRect(BS_X+x*BS_CELL,BS_Y+y*BS_CELL,BS_CELL,BS_CELL,c);
        }
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    if(placing)       fb.drawString("L: move  hold L: turn  R: place",W/2,H-4,1);
    else if(revealed) fb.drawString("R: rematch   L: game list",W/2,H-4,1);
    else if(pbMyTurn())fb.drawString("L: move   R: fire",W/2,H-4,1);
    else              fb.drawString(layoutSent?"waiting for your turn":"waiting",W/2,H-4,1);
}

BoardGame BATTLESHIP_GAME = {
    "MANTIS FLEET", 4, 2, PB_MAX_PLAYERS,
    bsStart, bsTick, bsApply, bsDraw, bsOver, bsEnd
};
