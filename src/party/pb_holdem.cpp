// FILE: src/party/pb_holdem.cpp
// MANTIS HOLD'EM - arcade-simplified Texas Hold'em for the party stack.
//
// Simplified on purpose (this is a party game, not a gambling sim): a fixed ante
// and a fixed bet size, and on each street you either STAY (check/call) or FOLD,
// with a long-press to RAISE once. Hole cards are private to your own device.
//
// CURRENCY: chips are SESSION-ONLY, held in RAM by the pbwager module - they are
// how the game keeps score and settles, never persisted and never an inventory
// item. WAGER MODE (opt-in) additionally stakes ONE real quest-inventory item
// per player: it is escrowed with qpUseItem at the start and the whole pot of
// items is handed to the winner with qpAddItem, so a real item genuinely
// transfers loser -> winner, exactly like a trade.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W=170, H=320;
static constexpr int START_CHIPS=200, ANTE=10, BET=20;
static constexpr int WIN_CHIPS=START_CHIPS*2;

enum : uint8_t { OP_SETUP=1, OP_ACT=2, OP_STREET=3, OP_SHOWDOWN=4, OP_HAND=5 };
enum Street : uint8_t { ST_SETUP=0, ST_PRE, ST_FLOP, ST_TURN, ST_RIVER, ST_SHOW, ST_PAYOUT };

static Street street=ST_SETUP;
static int    chips[PB_MAX_PLAYERS];
static bool   folded[PB_MAX_PLAYERS];
static bool   acted[PB_MAX_PLAYERS];
static int    pot=0;
static int    hole[PB_MAX_PLAYERS][2];      // only my own two are shown
static int    board5[5];
static int    handNo=1;
static uint32_t animT=0, phaseT=0;

// wager mode
static bool   wagerMode=false;
static int    wagerItem=-1;                 // the item I staked
static int    wagerSel=0;                   // inventory browser cursor
static bool   wagerEscrowed=false;
static uint8_t stakedBy[PB_MAX_PLAYERS];    // item id each seat staked (+1, 0=none)

// ---- card helpers -----------------------------------------------------------
static inline int rankOf(int c){ return c/4; }        // 0..12  (0='2', 12='A')
static inline int suitOf(int c){ return c%4; }
static const char* RANKS="23456789TJQKA";
static const char* SUITS="SHDC";

// ---- 7-card hand evaluator: returns a comparable score ----------------------
// category*1e6 + tiebreak ranks packed high-to-low. Higher is better.
static uint32_t evalHand(const int* cards,int n){
    int rc[13]={0}, sc[4]={0};
    for(int i=0;i<n;i++){ if(cards[i]<0) continue; rc[rankOf(cards[i])]++; sc[suitOf(cards[i])]++; }
    int flushSuit=-1;
    for(int s=0;s<4;s++) if(sc[s]>=5) flushSuit=s;
    // straight detection on a rank bitmask (ace also low)
    uint16_t mask=0;
    for(int r=0;r<13;r++) if(rc[r]) mask|=(uint16_t)(1u<<r);
    int straightHigh=-1;
    for(int hi=12;hi>=4;hi--){
        uint16_t need=(uint16_t)(0x1Fu<<(hi-4));
        if((mask&need)==need){ straightHigh=hi; break; }
    }
    if(straightHigh<0){ // A-2-3-4-5 (ace low)
        uint16_t wheel=(1u<<12)|(1u<<0)|(1u<<1)|(1u<<2)|(1u<<3);
        if((mask&wheel)==wheel) straightHigh=3;
    }
    // straight flush: check straights using only the flush suit's ranks
    int sfHigh=-1;
    if(flushSuit>=0){
        uint16_t fm=0;
        for(int i=0;i<n;i++) if(cards[i]>=0&&suitOf(cards[i])==flushSuit) fm|=(uint16_t)(1u<<rankOf(cards[i]));
        for(int hi=12;hi>=4;hi--){ uint16_t need=(uint16_t)(0x1Fu<<(hi-4)); if((fm&need)==need){ sfHigh=hi; break; } }
        if(sfHigh<0){ uint16_t wheel=(1u<<12)|(1u<<0)|(1u<<1)|(1u<<2)|(1u<<3); if((fm&wheel)==wheel) sfHigh=3; }
    }
    // count shapes
    int quad=-1,trip=-1,pair1=-1,pair2=-1;
    for(int r=12;r>=0;r--){
        if(rc[r]==4&&quad<0) quad=r;
        else if(rc[r]==3&&trip<0) trip=r;
        else if(rc[r]==2){ if(pair1<0) pair1=r; else if(pair2<0) pair2=r; }
    }
    auto kick=[&](int excl1,int excl2,int cnt)->uint32_t{
        uint32_t k=0; int taken=0;
        for(int r=12;r>=0&&taken<cnt;r--){
            if(r==excl1||r==excl2||!rc[r]) continue;
            k=(k<<4)|(uint32_t)r; taken++;
        }
        return k;
    };
    if(sfHigh>=0)                 return 8000000u + (uint32_t)sfHigh;
    if(quad>=0)                   return 7000000u + (uint32_t)quad*16u + (kick(quad,-1,1)&0xF);
    if(trip>=0&&pair1>=0)         return 6000000u + (uint32_t)trip*16u + (uint32_t)pair1;
    if(flushSuit>=0){
        // top five of the flush suit
        uint32_t k=0; int taken=0;
        for(int r=12;r>=0&&taken<5;r--){
            for(int i=0;i<n;i++) if(cards[i]>=0&&suitOf(cards[i])==flushSuit&&rankOf(cards[i])==r){ k=(k<<4)|(uint32_t)r; taken++; break; }
        }
        return 5000000u + k;
    }
    if(straightHigh>=0)           return 4000000u + (uint32_t)straightHigh;
    if(trip>=0)                   return 3000000u + (uint32_t)trip*256u + kick(trip,-1,2);
    if(pair1>=0&&pair2>=0)        return 2000000u + (uint32_t)pair1*256u + (uint32_t)pair2*16u + (kick(pair1,pair2,1)&0xF);
    if(pair1>=0)                  return 1000000u + (uint32_t)pair1*4096u + kick(pair1,-1,3);
    return kick(-1,-1,5);
}

// ---- lifecycle --------------------------------------------------------------
static void dealHand(){
    pbdeck::shuffle();
    for(int s=0;s<PB_MAX_PLAYERS;s++){ hole[s][0]=hole[s][1]=-1; folded[s]=false; acted[s]=false; }
    // Deal deterministically from the shared seed so every device agrees on who
    // holds what; each device only ever DISPLAYS its own two cards.
    for(int s=0;s<pbPlayerCount();s++){ hole[s][0]=pbdeck::draw(); hole[s][1]=pbdeck::draw(); }
    for(int i=0;i<5;i++) board5[i]=pbdeck::draw();
    pot=0;
    for(int s=0;s<pbPlayerCount();s++){
        int a = chips[s]<ANTE?chips[s]:ANTE;
        chips[s]-=a; pot+=a;
    }
    street=ST_PRE; phaseT=millis();
}

static void hdStart(uint32_t sd,int n,int seat){
    (void)sd;(void)n;(void)seat;
    for(int s=0;s<PB_MAX_PLAYERS;s++){ chips[s]=START_CHIPS; stakedBy[s]=0; }
    pbwager::reset(); pbwager::addChips(START_CHIPS);
    street=ST_SETUP; handNo=1; wagerMode=false; wagerItem=-1;
    wagerSel=0; wagerEscrowed=false; animT=0; phaseT=millis();
}

// inventory browser: the items I actually hold
static int itemAt(int idx){
    int seen=0;
    for(int id=0;id<QP_ITEM_COUNT;id++) if(qpItemCount((uint8_t)id)>0){ if(seen==idx) return id; seen++; }
    return -1;
}
static int itemsHeld(){
    int n=0; for(int id=0;id<QP_ITEM_COUNT;id++) if(qpItemCount((uint8_t)id)>0) n++;
    return n;
}

static void hdApply(const PbMove* m){
    if(m->len<1) return;
    switch(m->data[0]){
    case OP_SETUP:
        // host announces whether real items are in play
        if(m->len>=2) wagerMode=(m->data[1]!=0);
        dealHand();
        break;
    case OP_ACT: {
        if(m->len<2) break;
        int s=m->player; if(s<0||s>=PB_MAX_PLAYERS) break;
        uint8_t act=m->data[1];
        if(act==0){ folded[s]=true; }
        else {
            int amt = (act==2? BET*2 : BET);
            if(amt>chips[s]) amt=chips[s];
            chips[s]-=amt; pot+=amt;
        }
        acted[s]=true;
        // street advances once every live player has acted
        bool all=true; int live=0;
        for(int i=0;i<pbPlayerCount();i++){
            if(folded[i]) continue;
            live++;
            if(!acted[i]) all=false;
        }
        if(live<=1){ street=ST_SHOW; phaseT=millis(); }
        else if(all){
            for(int i=0;i<pbPlayerCount();i++) acted[i]=false;
            street=(Street)(street+1);
            if(street>ST_RIVER) street=ST_SHOW;
            phaseT=millis();
        }
        break; }
    case OP_HAND:
        // next hand
        for(int i=0;i<pbPlayerCount();i++) acted[i]=false;
        handNo++;
        dealHand();
        break;
    }
}

static int bestSeat(){
    uint32_t best=0; int who=-1;
    for(int s=0;s<pbPlayerCount();s++){
        if(folded[s]) continue;
        int seven[7]={hole[s][0],hole[s][1],board5[0],board5[1],board5[2],board5[3],board5[4]};
        uint32_t sc=evalHand(seven,7);
        if(who<0||sc>best){ best=sc; who=s; }
    }
    return who;
}

static void hdTick(){
    animT++;
    uint32_t now=millis();
    bool lE=btnL&&!btnL_prev, rE=btnR&&!btnR_prev;
    static bool rDown=false; static uint32_t rAt=0; static bool rLong=false;
    if(btnR&&!btnR_prev){ rDown=true; rAt=now; rLong=false; }
    if(btnR&&rDown&&!rLong&&now-rAt>380){ rLong=true; }
    bool rRaise = (!btnR&&btnR_prev&&rDown&&rLong); if(!btnR) rDown=false;

    switch(street){
    case ST_SETUP: {
        // wager opt-in: browse the items you hold, or play chips only
        int held=itemsHeld();
        if(lE && held>0) wagerSel=(wagerSel+1)%(held+1);   // last slot = chips only
        if(rE && now-phaseT>300){
            if(held>0 && wagerSel<held){ wagerMode=true; wagerItem=itemAt(wagerSel); }
            else { wagerMode=false; wagerItem=-1; }
            if(wagerMode && wagerItem>=0){
                // ESCROW: the item really leaves my inventory now
                if(qpUseItem((uint8_t)wagerItem)){ wagerEscrowed=true; stakedBy[pbMySeat()]=(uint8_t)(wagerItem+1); }
                else { wagerMode=false; wagerItem=-1; }
            }
            if(pbIsHost()||pbPlayerCount()==1){
                PbMove mv; memset(&mv,0,sizeof(mv));
                mv.player=(uint8_t)pbMySeat(); mv.len=2;
                mv.data[0]=OP_SETUP; mv.data[1]=wagerMode?1:0;
                pbSubmitInfo(&mv);
            } else { dealHand(); }
        }
        break; }
    case ST_PRE: case ST_FLOP: case ST_TURN: case ST_RIVER: {
        if(!pbMyTurn()) return;
        int me=pbMySeat();
        if(folded[me]||acted[me]) { pbAdvanceTurn(); return; }
        if(lE){                     // FOLD
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)me; mv.len=2; mv.data[0]=OP_ACT; mv.data[1]=0;
            pbSubmitMove(&mv);
        } else if(rRaise){          // RAISE (long press)
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)me; mv.len=2; mv.data[0]=OP_ACT; mv.data[1]=2;
            pbSubmitMove(&mv);
        } else if(rE && !rLong){    // STAY / call
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)me; mv.len=2; mv.data[0]=OP_ACT; mv.data[1]=1;
            pbSubmitMove(&mv);
        }
        break; }
    case ST_SHOW: {
        if(now-phaseT>2200){
            int w=bestSeat();
            if(w>=0) chips[w]+=pot;
            pot=0;
            street=ST_PAYOUT; phaseT=now;
        }
        break; }
    case ST_PAYOUT: {
        if((lE||rE) && now-phaseT>500){
            // knocked-out players stay out; otherwise deal again
            if(pbIsHost()||pbPlayerCount()==1){
                PbMove mv; memset(&mv,0,sizeof(mv));
                mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_HAND;
                pbSubmitInfo(&mv);
            }
        }
        break; }
    }
}

static bool hdOver(int* winner){
    if(street==ST_SETUP) return false;
    // someone reached the target stack, or only one player still has chips
    int alive=0,last=-1;
    for(int s=0;s<pbPlayerCount();s++){
        if(chips[s]>0){ alive++; last=s; }
        if(chips[s]>=WIN_CHIPS){ *winner=s; return true; }
    }
    if(alive<=1){ *winner=last; return true; }
    return false;
}
static void hdEnd(int w){
    // REAL WAGER PAYOUT: every staked item goes to the winner. My device only
    // grants items if I am the winner - the loser's item already left their
    // inventory at escrow, so nothing is duplicated.
    if(w==pbMySeat()){
        for(int s=0;s<pbPlayerCount();s++)
            if(stakedBy[s]) qpAddItem((uint8_t)(stakedBy[s]-1),1);
    }
    memset(stakedBy,0,sizeof(stakedBy));
    wagerEscrowed=false;
}

// ---- draw -------------------------------------------------------------------
static void drawCard(int x,int y,int c,bool faceUp){
    if(c<0){ return; }
    fb.fillRoundRect(x,y,20,28,3,faceUp?LIME_PALE:TEAL_DARK);
    fb.drawRoundRect(x,y,20,28,3,TEAL_LIGHT);
    if(!faceUp){
        for(int i=2;i<26;i+=4) fb.drawFastHLine(x+3,y+i,14,TEAL);
        return;
    }
    int r=rankOf(c), s=suitOf(c);
    uint16_t col = (s==1||s==2)?PURPLE_BRIGHT:TEAL_DEEP;   // hearts/diamonds red-ish
    char rb[2]={RANKS[r],0}, sb[2]={SUITS[s],0};
    fb.setTextDatum(TL_DATUM); fb.setTextColor(col);
    fb.drawString(rb,x+3,y+2,2);
    fb.setTextDatum(BR_DATUM); fb.drawString(sb,x+18,y+26,1);
}

static void hdDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS HOLD'EM",W/2,6,2);

    if(street==ST_SETUP){
        int held=itemsHeld();
        fb.setTextColor(TEAL_LIGHT); fb.drawString("stake something?",W/2,32,1);
        for(int i=0;i<held&&i<5;i++){
            int id=itemAt(i); bool sel=(wagerSel==i);
            fb.fillRoundRect(14,56+i*26,W-28,22,4,sel?PURPLE_DARK:TEAL_DARK);
            if(sel) fb.drawRoundRect(14,56+i*26,W-28,22,4,LIME);
            fb.setTextDatum(ML_DATUM); fb.setTextColor(sel?LIME_PALE:TEAL_LIGHT);
            char b[26]; snprintf(b,sizeof(b),"%s x%u",qpItemDef((uint8_t)id).name,
                                 (unsigned)qpItemCount((uint8_t)id));
            fb.drawString(b,22,67+i*26,1);
        }
        bool chipsOnly=(wagerSel>=held);
        int cy=56+(held<5?held:5)*26;
        fb.fillRoundRect(14,cy,W-28,22,4,chipsOnly?PURPLE_DARK:TEAL_DARK);
        if(chipsOnly) fb.drawRoundRect(14,cy,W-28,22,4,LIME);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(chipsOnly?LIME_PALE:TEAL_LIGHT);
        fb.drawString("chips only (no stakes)",22,cy+11,1);
        fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString(held>0?"L: choose   R: confirm":"R: play for chips",W/2,H-6,1);
        if(held>0){
            fb.setTextColor(PURPLE_LIGHT);
            fb.drawString("a staked item really transfers",W/2,H-20,1);
        }
        return;
    }

    // pot + chips
    char b[30];
    fb.setTextColor(LIME); snprintf(b,sizeof(b),"POT %d",pot); fb.drawString(b,W/2,26,2);
    for(int s=0;s<pbPlayerCount()&&s<4;s++){
        int x=6+s*40;
        bool me=(s==pbMySeat());
        uint16_t bg = folded[s]?TEAL_DEEP:(s==pbWhoseTurn()?PURPLE_DARK:TEAL_DARK);
        fb.fillRoundRect(x,46,36,26,3,bg);
        if(me) fb.drawRoundRect(x,46,36,26,3,LIME);
        fb.setTextDatum(TC_DATUM); fb.setTextColor(folded[s]?TEAL:LIME_PALE);
        snprintf(b,sizeof(b),"%d",chips[s]); fb.drawString(b,x+18,49,1);
        fb.setTextColor(stakedBy[s]?PURPLE_LIGHT:TEAL);
        fb.drawString(folded[s]?"out":(stakedBy[s]?"item":"in"),x+18,61,1);
    }
    // community cards, revealed by street
    int shown = street<=ST_PRE?0 : street==ST_FLOP?3 : street==ST_TURN?4 : 5;
    for(int i=0;i<5;i++){
        int x=8+i*32, y=84;
        if(i<shown) drawCard(x,y,board5[i],true);
        else { fb.drawRoundRect(x,y,20,28,3,TEAL_DARK); }
    }
    // my hole cards (only mine, ever)
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString("your hand",W/2,124,1);
    drawCard(W/2-24,138,hole[pbMySeat()][0],true);
    drawCard(W/2+4,138,hole[pbMySeat()][1],true);

    if(street==ST_SHOW||street==ST_PAYOUT){
        int w=bestSeat();
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(w==pbMySeat()?LIME:PURPLE_LIGHT);
        snprintf(b,sizeof(b),"%s takes it",w==pbMySeat()?"YOU":pbPlayerName(w));
        fb.drawString(b,W/2,180,2);
        // show everyone's cards at the showdown
        for(int s=0;s<pbPlayerCount()&&s<4;s++){
            if(folded[s]) continue;
            int x=6+s*40;
            drawCard(x,204,hole[s][0],true);
            drawCard(x+18,204,hole[s][1],true);
        }
        fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("press for the next hand",W/2,H-6,1);
        return;
    }

    const char* st = street==ST_PRE?"PRE-FLOP":street==ST_FLOP?"FLOP":street==ST_TURN?"TURN":"RIVER";
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL); fb.drawString(st,W/2,176,1);
    fb.setTextDatum(BC_DATUM);
    if(pbMyTurn()&&!folded[pbMySeat()]){
        fb.setTextColor(LIME_PALE);
        fb.drawString("L: fold   R: stay   hold R: raise",W/2,H-6,1);
    } else {
        fb.setTextColor(TEAL_DARK);
        snprintf(b,sizeof(b),"%s is thinking",pbPlayerName(pbWhoseTurn()));
        fb.drawString(folded[pbMySeat()]?"you folded":b,W/2,H-6,1);
    }
}

BoardGame HOLDEM_GAME = {
    "MANTIS HOLD'EM", 7, 2, 4,
    hdStart, hdTick, hdApply, hdDraw, hdOver, hdEnd
};
