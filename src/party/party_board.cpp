// FILE: src/party/party_board.cpp
// The game-agnostic board-game engine. See party_board.h for the design.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ============================================================================
//  GAME REGISTRY  - the board-stack menu lists these; charades is one of them.
// ============================================================================
// Each game is a BoardGame descriptor provided by its own .cpp. We declare the
// externs here; the ones not yet built are guarded so the file always links.
extern BoardGame CONNECT4_GAME;
extern BoardGame TICTACTOE_GAME;
extern BoardGame RPS_GAME;
extern BoardGame BATTLESHIP_GAME;
extern BoardGame HANGMAN_GAME;
extern BoardGame TRIVIA_GAME;
extern BoardGame HOLDEM_GAME;
extern BoardGame RALLY_GAME;

static BoardGame* GAMES[] = {
    &CONNECT4_GAME,
    &TICTACTOE_GAME,
    &RPS_GAME,
    &BATTLESHIP_GAME,
    &HANGMAN_GAME,
    &TRIVIA_GAME,
    &HOLDEM_GAME,
    &RALLY_GAME,
};
static const int GAME_COUNT = sizeof(GAMES)/sizeof(GAMES[0]);
static BoardGame* G = nullptr;      // the running game

// ============================================================================
//  TRANSPORT  (Versus/Pet house pattern - flat msg, fast callback, seq dedup)
// ============================================================================
static const uint8_t PB_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
enum PbMsgType : uint8_t { BM_HELLO=1, BM_CONFIRM, BM_START, BM_MOVE, BM_END, BM_BYE, BM_PRIV, BM_INFO };

struct __attribute__((packed)) PbPkt {
    uint16_t magic; uint8_t type; uint8_t seq;
    uint32_t id; uint32_t seed;
    uint8_t  gameId; uint8_t seat; uint8_t nPlayers; uint8_t winner;
    char     name[PB_NAME_LEN];
    PbMove   move;
};
static constexpr uint16_t PB_MAGIC = 0x5042;   // 'PB'

static volatile bool rxHello,rxConfirm,rxStart,rxMove,rxEnd,rxBye;
static uint8_t  rxMac[6];
static uint32_t rxId, rxSeed;
static uint8_t  rxGameId, rxSeat, rxNPlayers, rxWinner;
static char     rxName[PB_NAME_LEN];
static PbMove   rxMoveBuf;
static uint16_t rxMoveSeq = 0x1FF;   // dedup sentinel for BM_MOVE
static volatile bool rxInfo=false;   // referee result for everyone (no turn change)
static PbMove   rxInfoBuf;
static uint16_t rxInfoSeq = 0x1FF;
static volatile bool rxPriv=false;   // a referee message addressed to me
static PbMove   rxPrivBuf;
static uint16_t rxPrivSeq = 0x1FF;
static uint8_t  rxPrivSeat=0;

static uint32_t myId=0; static uint8_t mySeq=0;
static uint8_t  peerMac[PB_MAX_PLAYERS][6]; static int peerCount=0;
static bool     espUp=false;

static void pbOnRecv(const uint8_t* mac,const uint8_t* d,int len){
    if((size_t)len<sizeof(PbPkt)) return;
    const PbPkt* m=(const PbPkt*)d;
    if(m->magic!=PB_MAGIC || m->id==myId) return;
    memcpy((void*)rxMac,mac,6); rxId=m->id;
    switch(m->type){
        case BM_HELLO:   rxHello=true; break;
        case BM_CONFIRM: rxConfirm=true; memcpy(rxName,m->name,PB_NAME_LEN); break;
        case BM_START:   rxSeed=m->seed; rxGameId=m->gameId; rxSeat=m->seat; rxNPlayers=m->nPlayers; rxStart=true; break;
        case BM_MOVE:    if((uint16_t)m->seq!=rxMoveSeq){ rxMoveSeq=m->seq; rxMoveBuf=m->move; rxMove=true; } break;
        case BM_END:     rxWinner=m->winner; rxEnd=true; break;
        case BM_INFO:    if((uint16_t)m->seq!=rxInfoSeq){ rxInfoSeq=m->seq; rxInfoBuf=m->move; rxInfo=true; } break;
        case BM_PRIV:    if((uint16_t)m->seq!=rxPrivSeq){ rxPrivSeq=m->seq; rxPrivBuf=m->move; rxPrivSeat=m->seat; rxPriv=true; } break;
        case BM_BYE:     rxBye=true; break;
    }
}
static void pbClearRx(){ rxHello=rxConfirm=rxStart=rxMove=rxEnd=rxBye=false; rxPriv=false; rxInfo=false; }

static char myName[PB_NAME_LEN];
static const char* NICKS[] = {"Mantis","Raptor","Nymph","Striker","Hopper","Blade",
                              "Verde","Snappy","Ambush","Prowler","Twitch","Gaze"};
static void makeName(uint32_t id,char*out){ snprintf(out,PB_NAME_LEN,"%s%X",NICKS[id%12],(unsigned)(id&0xF)); }

static bool pbAddPeer(const uint8_t* mac){
    if(esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p; memset(&p,0,sizeof(p));
    memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
    return esp_now_add_peer(&p)==ESP_OK;
}
static bool pbRadioUp(){
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    myId=((uint32_t)mac[3]<<16)|((uint32_t)mac[4]<<8)|mac[5];
    makeName(myId,myName);
    if(esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(pbOnRecv));
    pbAddPeer(PB_BCAST); espUp=true; pbClearRx();
    return true;
}
static void pbSend(PbMsgType t,const uint8_t* to,const PbMove* mv=nullptr,uint8_t winner=0,int seatArg=-1);
static void pbRadioDown(bool bye){
    if(espUp){
        if(bye) pbSend(BM_BYE,PB_BCAST);
        delay(20);
        esp_now_unregister_recv_cb(); esp_now_deinit(); espUp=false;
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    peerCount=0; pbClearRx();
}

// ============================================================================
//  MATCH STATE
// ============================================================================
static bool     solo=true;          // single device, pass-and-play or vs nobody
static bool     host=false;
static int      nPlayers=1, mySeat=0, turnSeat=0;
static uint32_t seed=1;
static int      turnCounter=0;      // increments each move (dice/deck sequencing)
static bool     keepTurn=false;     // set by a game inside applyMove
static bool     matchOver=false; static int winnerSeat=-1;
static char     seatName[PB_MAX_PLAYERS][PB_NAME_LEN];
static uint8_t  seatMac[PB_MAX_PLAYERS][6];   // host-only: who sits where
static uint8_t  hostMac[6];                   // joiner-only: who to send secrets to

static void pbSend(PbMsgType t,const uint8_t* to,const PbMove* mv,uint8_t winner,int seatArg){
    if(!espUp) return;
    PbPkt m; memset(&m,0,sizeof(m));
    m.magic=PB_MAGIC; m.type=t; m.seq=++mySeq; m.id=myId;
    m.seed=seed; m.gameId=G?G->gameId:0;
    m.seat=(uint8_t)(seatArg>=0?seatArg:mySeat);
    m.nPlayers=(uint8_t)nPlayers; m.winner=winner;
    memcpy(m.name,myName,PB_NAME_LEN);
    if(mv) m.move=*mv;
    esp_now_send(to,(uint8_t*)&m,sizeof(m));
    if(t==BM_START||t==BM_MOVE||t==BM_END||t==BM_PRIV||t==BM_INFO){ delay(4); esp_now_send(to,(uint8_t*)&m,sizeof(m)); }
}

// ---- framework API ----------------------------------------------------------
int   pbPlayerCount(){ return nPlayers; }
int   pbMySeat(){ return mySeat; }
int   pbWhoseTurn(){ return turnSeat; }
bool  pbMyTurn(){ return turnSeat==mySeat; }
bool  pbIsHost(){ return host; }
uint32_t pbSeed(){ return seed; }
const char* pbPlayerName(int seat){ return (seat>=0&&seat<nPlayers)?seatName[seat]:""; }
void  pbKeepTurn(){ keepTurn=true; }
void  pbAdvanceTurn(){ turnSeat=(turnSeat+1)%nPlayers; }
void  pbEndMatch(int w){ matchOver=true; winnerSeat=w; }

void pbSubmitMove(const PbMove* m){
    // apply locally, broadcast, then advance turn unless the game held it
    keepTurn=false;
    if(G&&G->applyMove) G->applyMove(m);
    turnCounter++;
    if(!solo) pbSend(BM_MOVE,PB_BCAST,m);
    if(!keepTurn) pbAdvanceTurn();
}

// ============================================================================
//  DICE / DECK / WAGER MODULES  (deterministic from seed+turnCounter)
// ============================================================================
namespace pbdice {
    static uint32_t s=1;
    void reset(){ s = seed ^ 0x9E3779B9u; }
    int roll(int sides){
        // mix in the turn counter so both devices roll identically in sequence
        s = s*1664525u + 1013904223u + (uint32_t)turnCounter*2654435761u;
        return sides<=0?1:1+(int)((s>>8)%(uint32_t)sides);
    }
}
namespace pbdeck {
    static uint8_t cards[52]; static int top=0;
    void shuffle(){
        for(int i=0;i<52;i++) cards[i]=(uint8_t)i;
        uint32_t s=seed ^ 0xC2B2AE35u;
        for(int i=51;i>0;i--){ s=s*1664525u+1013904223u; int j=(int)((s>>8)%(uint32_t)(i+1)); uint8_t t=cards[i];cards[i]=cards[j];cards[j]=t; }
        top=0;
    }
    int draw(){ return top<52?cards[top++]:-1; }
    int remaining(){ return 52-top; }
}
namespace pbwager {
    static uint32_t sessChips=0;
    void reset(){ sessChips=0; }
    uint32_t chips(){ return sessChips; }
    void addChips(int d){ long v=(long)sessChips+d; sessChips=(uint32_t)(v<0?0:v); }
    // real-item wagering deferred to the games that use it (Hold'em) - the
    // qpUseItem/qpAddItem calls live there so this module stays dependency-free
    // until that game is built. Declared here, defined in holdem.
    bool escrowItem(uint8_t,uint16_t);   // provided by holdem when built
    void awardItem(uint8_t,uint16_t);
}

// ============================================================================
//  GRID CURSOR MODULE  (two-button placement idiom)
// ============================================================================
namespace pbgrid {
    static int cols=1,rows=1,curx=0,cury=0;
    static bool fMoved,fSel,fRot;
    static bool lDown; static uint32_t lAt; static bool lLong;
    static constexpr uint32_t LONG_MS=380;
    void begin(int c,int r){ cols=c;rows=r;curx=0;cury=0;lDown=false;lLong=false; }
    void setCursor(int x,int y){ curx=x;cury=y; }
    void tick(){
        fMoved=fSel=fRot=false; uint32_t now=millis();
        // L: tap = advance cursor (row-major wrap); long = rotate/secondary
        if(btnL&&!btnL_prev){ lDown=true; lAt=now; lLong=false; }
        if(btnL&&lDown&&!lLong&&now-lAt>LONG_MS){ lLong=true; fRot=true; }
        if(!btnL&&btnL_prev){ if(lDown&&!lLong){ curx++; if(curx>=cols){curx=0;cury=(cury+1)%rows;} fMoved=true; } lDown=false; }
        // R: select
        if(btnR&&!btnR_prev) fSel=true;
    }
    int cx(){return curx;} int cy(){return cury;}
    bool moved(){return fMoved;} bool selected(){return fSel;} bool rotated(){return fRot;}
}

// ============================================================================
//  KEYBOARD MODULE  (A-Z, backspace, enter; L navigate, R select)
// ============================================================================
namespace pbkbd {
    static const char* ROW[4]={"ABCDEFG","HIJKLMN","OPQRSTU","VWXYZ<#"};  // < = bksp, # = enter
    static int sel=0; static char buf[20]; static int len=0, maxLen=12;
    static bool fEnter;
    void begin(int ml){ maxLen=ml>19?19:ml; sel=0; len=0; buf[0]='\0'; }
    void clear(){ len=0; buf[0]='\0'; }
    static int totalKeys(){ int n=0; for(int r=0;r<4;r++) n+=(int)strlen(ROW[r]); return n; }
    static char keyAt(int idx,int&rr,int&cc){ int n=0; for(int r=0;r<4;r++){int L=(int)strlen(ROW[r]); if(idx<n+L){rr=r;cc=idx-n;return ROW[r][idx-n];} n+=L;} rr=cc=0; return 'A'; }
    void tick(){
        fEnter=false;
        if(btnL&&!btnL_prev) sel=(sel+1)%totalKeys();
        if(btnR&&!btnR_prev){
            int rr,cc; char k=keyAt(sel,rr,cc);
            if(k=='<'){ if(len>0){ len--; buf[len]='\0'; } }
            else if(k=='#'){ if(len>0) fEnter=true; }
            else if(len<maxLen){ buf[len++]=k; buf[len]='\0'; }
        }
    }
    const char* text(){ return buf; }
    bool entered(){ return fEnter; }
    // exposed for the game's draw:
    int  cursorKey(){ return sel; }
    const char* rowStr(int r){ return (r>=0&&r<4)?ROW[r]:""; }
}

// ============================================================================
//  LOBBY + DISPATCH
// ============================================================================
enum PbScreen : uint8_t { PS_MODE=0, PS_LOBBY, PS_PLAY, PS_OVER };
static PbScreen scr = PS_MODE;
static int  modeSel = 0;
static bool wantExit=false;            // 0 = pass&play (solo device), 1 = link up
static int  selGame = 0;            // which GAMES[] entry
static uint32_t enterAt=0, lastHello=0;
static constexpr uint32_t PB_ENTER_GUARD=250, PB_HELLO_MS=300;

int  pbGameCount(){ return GAME_COUNT; }
const char* pbGameName(int i){ return (i>=0&&i<GAME_COUNT)?GAMES[i]->name:""; }
void pbSelectGame(int i){ if(i>=0&&i<GAME_COUNT) selGame=i; }
bool pbWantsExit(){ return wantExit; }

// local edge helpers (framework screens only; games use the modules)
static bool lEdge(){ return btnL&&!btnL_prev; }
static bool rEdge(){ return btnR&&!btnR_prev; }

static void seatNamesDefault(){
    for(int i=0;i<PB_MAX_PLAYERS;i++) snprintf(seatName[i],PB_NAME_LEN,"P%d",i+1);
}

static void startMatch(uint32_t sd,int n,int seat){
    seed=sd; nPlayers=n; mySeat=seat; turnSeat=0; turnCounter=0;
    matchOver=false; winnerSeat=-1; keepTurn=false;
    pbdice::reset(); pbdeck::shuffle(); pbwager::reset();
    G=GAMES[selGame];
    if(G&&G->start) G->start(seed,nPlayers,mySeat);
    scr=PS_PLAY;
}

void partyBoardEnter(){
    fb.setRotation(0);
    scr=PS_MODE; modeSel=0; enterAt=millis(); wantExit=false;
    solo=true; host=false; nPlayers=1; mySeat=0; G=nullptr;
    matchOver=false; winnerSeat=-1;
    seatNamesDefault();
    espUp=false; peerCount=0; pbClearRx();
}

void partyBoardShutdown(){
    if(G&&G->onEnd&&matchOver) { /* already ended */ }
    pbRadioDown(true);
    G=nullptr;
}

void partyBoardTick(){
    uint32_t now=millis();
    bool guard = (now-enterAt)<PB_ENTER_GUARD;
    switch(scr){
    case PS_MODE: {
        if(guard) break;
        if(lEdge()) modeSel^=1;
        if(btnR&&btnL) { /* ignore chords */ }
        if(rEdge()){
            if(modeSel==0){
                // pass-and-play on one device: seats rotate on the same screen
                solo=true; host=true;
                int n=GAMES[selGame]->minPlayers; if(n<2)n=2;
                snprintf(seatName[0],PB_NAME_LEN,"P1"); snprintf(seatName[1],PB_NAME_LEN,"P2");
                startMatch((uint32_t)now|1u,n,0);
            } else {
                if(pbRadioUp()){ solo=false; scr=PS_LOBBY; lastHello=0; }
            }
        }
        break; }

    case PS_LOBBY: {
        // HELLO beacon; lowest id hosts. Host presses R to start.
        if(now-lastHello>PB_HELLO_MS){ lastHello=now; pbSend(BM_HELLO,PB_BCAST); }
        if(rxHello||rxConfirm){
            rxHello=rxConfirm=false;
            if(pbAddPeer(rxMac) && peerCount<PB_MAX_PLAYERS){
                bool known=false;
                for(int i=0;i<peerCount;i++) if(memcmp(peerMac[i],rxMac,6)==0) known=true;
                if(!known){ memcpy(peerMac[peerCount],rxMac,6); peerCount++; }
            }
            pbSend(BM_CONFIRM,PB_BCAST);
            // lowest id hosts
            host = (myId <= rxId);
        }
        if(rxStart){
            rxStart=false;
            // The host addresses each joiner individually, so m->seat IS my seat.
            // (Broadcasting one START gave every joiner seat 1 - broken for 3+.)
            solo=false; host=false;
            memcpy(hostMac,rxMac,6);      // secrets (fleet layouts) go here only
            startMatch(rxSeed,rxNPlayers,(int)rxSeat);
            break;
        }
        if(!guard && rEdge() && host && peerCount>=1){
            nPlayers=peerCount+1; mySeat=0;
            uint32_t sd=(uint32_t)now|1u; seed=sd;
            for(int s=0;s<nPlayers;s++) snprintf(seatName[s],PB_NAME_LEN,"P%d",s+1);
            // host is seat 0; each peer gets its own seat in a targeted START,
            // and we remember which MAC sits where so a referee-style game can
            // message one player privately later.
            memset(seatMac,0,sizeof(seatMac));
            for(int i=0;i<peerCount && i+1<PB_MAX_PLAYERS;i++){
                memcpy(seatMac[i+1],peerMac[i],6);
                pbSend(BM_START,peerMac[i],nullptr,0,i+1);
            }
            startMatch(sd,nPlayers,0);
        }
        if(!guard && lEdge()){ pbRadioDown(true); wantExit=true; }
        break; }

    case PS_PLAY: {
        // peers' moves arrive here
        if(rxMove){ rxMove=false; if(G&&G->applyMove){ keepTurn=false; G->applyMove(&rxMoveBuf); turnCounter++; if(!keepTurn) pbAdvanceTurn(); } }
        // a referee message addressed to me: apply it, never advance the turn
        if(rxInfo){ rxInfo=false; if(G&&G->applyMove) G->applyMove(&rxInfoBuf); }
        if(rxPriv){ rxPriv=false; if(rxPrivSeat==(uint8_t)mySeat && G&&G->applyMove) G->applyMove(&rxPrivBuf); }
        if(rxEnd){ rxEnd=false; matchOver=true; winnerSeat=rxWinner; }
        if(G&&G->tickLocal) G->tickLocal();
        if(!matchOver && G&&G->isOver){ int w=-1; if(G->isOver(&w)){ matchOver=true; winnerSeat=w; if(!solo) pbSend(BM_END,PB_BCAST,nullptr,(uint8_t)(w<0?255:w)); } }
        if(matchOver){ if(G&&G->onEnd) G->onEnd(winnerSeat); scr=PS_OVER; enterAt=now; }
        break; }

    case PS_OVER: {
        if((now-enterAt)>600 && (lEdge()||rEdge())){
            // rematch same game, or back to the stack menu on L
            if(lEdge()){ pbRadioDown(true); wantExit=true; }
            else { startMatch((uint32_t)now|1u,nPlayers,mySeat); }
        }
        break; }
    }
}

// ---- framework screens ------------------------------------------------------
static void drawModeScreen(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(GAMES[selGame]->name,W/2,14,2);
    fb.setTextColor(TEAL_LIGHT); fb.drawString("how do you want to play?",W/2,40,1);
    const char* opt[2]={"PASS & PLAY","LINK DEVICES"};
    for(int i=0;i<2;i++){
        int y=90+i*54; bool s=(i==modeSel);
        fb.fillRoundRect(16,y,W-32,42,5,s?PURPLE_DARK:TEAL_DARK);
        fb.drawRoundRect(16,y,W-32,42,5,s?LIME:TEAL);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(s?LIME_PALE:TEAL_LIGHT);
        fb.drawString(opt[i],W/2,y+21,2);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("L: switch   R: go",W/2,H-6,1);
}
static void drawLobby(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("LOBBY",W/2,12,4);
    fb.setTextColor(TEAL_LIGHT);
    char b[28]; snprintf(b,sizeof(b),"%d device%s found",peerCount,peerCount==1?"":"s");
    fb.drawString(b,W/2,44,1);
    fb.setTextColor(host?LIME:TEAL_DARK);
    fb.drawString(host?"you are the host":"waiting for host",W/2,60,1);
    for(int i=0;i<=peerCount&&i<PB_MAX_PLAYERS;i++){
        int y=84+i*22;
        fb.fillRoundRect(20,y,W-40,18,3,TEAL_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(i==0?myName:"player",26,y+9,1);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString(host?"R: start   L: back":"L: back",W/2,H-6,1);
}
static void drawOver(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(PURPLE_LIGHT); fb.drawString("MATCH OVER",W/2,90,2);
    char b[28];
    if(winnerSeat<0) snprintf(b,sizeof(b),"DRAW");
    else if(winnerSeat==mySeat) snprintf(b,sizeof(b),"YOU WIN!");
    else snprintf(b,sizeof(b),"%s WINS",pbPlayerName(winnerSeat));
    fb.setTextColor(LIME_PALE); fb.drawString(b,W/2,130,4);
    fb.setTextColor(TEAL_DARK); fb.drawString("R: rematch   L: game list",W/2,H-30,1);
}

void partyBoardDraw(){
    switch(scr){
    case PS_MODE:  drawModeScreen(); break;
    case PS_LOBBY: drawLobby();      break;
    case PS_PLAY:  if(G&&G->draw) G->draw(); else fb.fillSprite(TEAL_DEEP); break;
    case PS_OVER:  drawOver();       break;
    }
    fb.pushSprite(0,0);
}

// ---- referee support (host holds hidden state; see party_board.h) -----------
void pbSendToSeat(int seat,const PbMove* m){
    if(solo||!host||seat<=0||seat>=nPlayers) return;
    pbSend(BM_PRIV,seatMac[seat],m,0,seat);
}
void pbSubmitInfo(const PbMove* m){
    if(G&&G->applyMove) G->applyMove(m);   // apply here
    if(!solo) pbSend(BM_INFO,PB_BCAST,m);  // and everywhere else; no turn change
}
void pbSendToHost(const PbMove* m){
    if(solo||host) return;              // host applies its own directly
    pbSend(BM_PRIV,hostMac,m,0,0);      // seat 0 = the host
}
void pbBroadcastInfo(const PbMove* m){
    if(solo) return;
    pbSend(BM_INFO,PB_BCAST,m);   // applied everywhere, turn untouched
}
