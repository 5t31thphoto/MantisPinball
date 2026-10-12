// party_net.cpp - host-authoritative party framework. See party_net.h.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include "party_net.h"
#include "../theme_colors.h"
#include "charades.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170, H = 320;
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// registered games (only charades for now)
extern PartyGame CHARADES_GAME;
static PartyGame* GAME = &CHARADES_GAME;

// ---- silly nicknames -------------------------------------------------------
static const char* NICKS[] = {
    "Waffles","Pickle","Noodle","Bandit","Gizmo","Sprocket","Tater","Biscuit",
    "Wombat","Pretzel","Snacky","Bloop","Zonk","Ziggy","Mochi","Gonzo",
};
static const int NICK_COUNT = sizeof(NICKS)/sizeof(NICKS[0]);

// ---- message protocol ------------------------------------------------------
// Two directions only:
//   host -> all : PM_STATE  (full authoritative snapshot, every tick)
//   any  -> host: PM_JOIN (id+nick), PM_GUESS, PM_START, PM_BYE
enum PMType : uint8_t { PM_STATE=1, PM_JOIN=2, PM_GUESS=3, PM_START=4, PM_BYE=5, PM_BEACON=6,
                        PM_RIGHT=7, PM_WRONG=8 };   // UP-player judge verdicts

// The authoritative snapshot. Fixed size; fits ESP-NOW's 250-byte limit easily.
struct __attribute__((packed)) PartySnapshot {
    uint8_t  type;          // PM_STATE
    uint8_t  phase;         // PartyPhase
    uint8_t  count;         // player count
    uint8_t  whoUp;         // index of UP player
    uint8_t  guesser;       // index of who buzzed (255 = none)
    uint8_t  gameId;        // which game
    uint16_t target;        // score target
    uint16_t msLeft;        // timer remaining (ms/10 -> see pack; here plain ms capped)
    uint16_t msTotal;       // timer total
    uint32_t seed;          // round seed (prompt selection)
    uint16_t ids[PARTY_MAX_PLAYERS];
    uint16_t scores[PARTY_MAX_PLAYERS];
    char     names[PARTY_MAX_PLAYERS][PARTY_NAME_LEN];
};

// Small client->host control message.
struct __attribute__((packed)) PartyCtrl {
    uint8_t  type;          // PM_JOIN / PM_GUESS / PM_START / PM_BYE / PM_BEACON
    uint16_t id;
    char     name[PARTY_NAME_LEN];
};

// ---- local state -----------------------------------------------------------
static bool      pEsp = false;
static bool      pHost = false;
static uint16_t  pMyId = 0;
static char      pMyName[PARTY_NAME_LEN];

// HOST truth (only meaningful on the host):
static PartyPlayer hPlayers[PARTY_MAX_PLAYERS];
static int         hCount = 0;
static PartyPhase  hPhase = PH_LOBBY;
static int         hWhoUp = 0;
static uint8_t     hGuesser = 255;
static uint16_t    hTarget = 10;
static uint16_t    hRoundSecs = 60;
static uint32_t    hSeed = 0;
static uint32_t    hTimerStart = 0;      // millis when LIVE began
static bool        hTimerRunning = false;
static uint32_t    hPhaseUntil = 0;      // for timed phases (reveal/scores)

// setup selectors (host lobby)
static int hCfgTarget = 1;   // 5/10/15/20
static int hCfgSecs   = 1;   // 30/60/90/120

// JOINER mirror (the last snapshot we render from):
static PartySnapshot jSnap;
static bool          jHaveSnap = false;

// discovery / join-prompt
enum PScreen { SC_SEARCH, SC_JOINPROMPT, SC_PLAY };
static PScreen pScreen = SC_SEARCH;
static uint32_t pEnterAt = 0;
static uint16_t pFoundHost = 0;          // host id we discovered
static uint32_t pLastBeacon = 0;
static uint32_t pLastSnap = 0;

// incoming (set in recv cb, consumed in tick)
// Each message type stashes its OWN sender id (payload written before the
// flag), so a JOIN or BEACON landing between ticks can never clobber the id a
// pending GUESS/START/verdict refers to.
static volatile bool rxSnap=false, rxJoin=false, rxGuess=false, rxStart=false,
                     rxBye=false, rxBeacon=false, rxRight=false, rxWrong=false;
static PartySnapshot rxSnapBuf;
static volatile uint16_t rxJoinId=0, rxGuessId=0, rxStartId=0, rxByeId=0,
                         rxBeaconId=0, rxRightId=0, rxWrongId=0;
static char rxJoinName[PARTY_NAME_LEN];
static uint32_t jSeedSeen=0; static bool jSeedInit=false;
static void joinerAdoptSnap();

// game-facing "current prompt" derives from seed inside the game module.

// ---- helpers ---------------------------------------------------------------
static void nickFor(uint16_t id, char* out) {
    snprintf(out, PARTY_NAME_LEN, "%s", NICKS[id % NICK_COUNT]);
    // disambiguate with last hex digit so two same-nick players differ
    size_t l = strlen(out);
    if (l < PARTY_NAME_LEN-3) snprintf(out+l, PARTY_NAME_LEN-l, "%X", id & 0xF);
}

static int hIndexOf(uint16_t id) {
    for (int i=0;i<hCount;i++) if (hPlayers[i].id==id) return i;
    return -1;
}

static void ensurePeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t pi; memset(&pi,0,sizeof(pi));
    memcpy(pi.peer_addr, mac, 6); pi.channel=0; pi.encrypt=false;
    esp_now_add_peer(&pi);
}

static void sendCtrl(uint8_t type) {
    PartyCtrl c; memset(&c,0,sizeof(c));
    c.type=type; c.id=pMyId; memcpy(c.name, pMyName, PARTY_NAME_LEN);
    esp_now_send(BCAST, (const uint8_t*)&c, sizeof(c));
}
// One-shot controls ride an unacked broadcast, so send them twice; the host
// side is idempotent and sweeps stale flags each tick.
static void sendCtrl2(uint8_t type) { sendCtrl(type); delay(3); sendCtrl(type); }

// Adopt a snapshot; if the round seed changed, derive the new prompt. Joiners
// never called roundStart before, so an UP joiner performed a stale prompt.
static void joinerAdoptSnap() {
    memcpy(&jSnap,&rxSnapBuf,sizeof(jSnap)); jHaveSnap=true;
    if (!jSeedInit || jSnap.seed!=jSeedSeen) {
        jSeedSeen=jSnap.seed; jSeedInit=true;
        if (GAME && GAME->roundStart) GAME->roundStart(jSnap.seed);
    }
}

// HOST: build + broadcast the authoritative snapshot.
static void hostBroadcastState() {
    PartySnapshot s; memset(&s,0,sizeof(s));
    s.type=PM_STATE; s.phase=(uint8_t)hPhase; s.count=(uint8_t)hCount;
    s.whoUp=(uint8_t)hWhoUp; s.guesser=hGuesser; s.gameId=0;
    s.target=hTarget; s.msTotal=(uint16_t)(hRoundSecs*1000);
    uint32_t left = 0;
    if (hTimerRunning) {
        uint32_t el = millis()-hTimerStart;
        uint32_t tot = hRoundSecs*1000;
        left = (el>=tot)?0:tot-el;
    } else {
        left = hRoundSecs*1000;
    }
    s.msLeft=(uint16_t)(left>60000?60000:left);
    s.seed=hSeed;
    for (int i=0;i<hCount;i++){
        s.ids[i]=hPlayers[i].id; s.scores[i]=hPlayers[i].score;
        memcpy(s.names[i], hPlayers[i].name, PARTY_NAME_LEN);
    }
    esp_now_send(BCAST, (const uint8_t*)&s, sizeof(s));
}

// ---- recv callback (fast, just stash) --------------------------------------
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) return;
    uint8_t t = data[0];
    if (t==PM_STATE && (size_t)len>=sizeof(PartySnapshot)) {
        memcpy(&rxSnapBuf, data, sizeof(PartySnapshot)); rxSnap=true;
    } else if ((size_t)len>=sizeof(PartyCtrl)) {
        const PartyCtrl* c=(const PartyCtrl*)data;
        switch (t) {                    // payload first, flag last
            case PM_JOIN:   rxJoinId=c->id;
                            memcpy(rxJoinName,c->name,PARTY_NAME_LEN);
                            rxJoin=true;   break;
            case PM_GUESS:  rxGuessId=c->id;  rxGuess=true;  break;
            case PM_START:  rxStartId=c->id;  rxStart=true;  break;
            case PM_RIGHT:  rxRightId=c->id;  rxRight=true;  break;
            case PM_WRONG:  rxWrongId=c->id;  rxWrong=true;  break;
            case PM_BYE:    rxByeId=c->id;    rxBye=true;    break;
            case PM_BEACON: rxBeaconId=c->id; rxBeacon=true; break;
        }
    }
    ensurePeer(mac);
}

static bool espInit() {
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    pMyId=(mac[4]<<8)|mac[5];
    nickFor(pMyId, pMyName);
    if (esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(onRecv));
    ensurePeer(BCAST);
    return true;
}

static void espStop() {
    if (pEsp) { sendCtrl(PM_BYE); delay(5);
        esp_now_unregister_recv_cb(); esp_now_deinit(); pEsp=false; }
    WiFi.mode(WIFI_OFF);
}

// ---- framework API (game-facing) -------------------------------------------
// These read from HOST truth when hosting, else from the joiner snapshot.
int partyPlayerCount() { return pHost ? hCount : jSnap.count; }
PartyPlayer* partyPlayer(int i) {
    static PartyPlayer tmp;
    if (pHost) return (i>=0&&i<hCount)?&hPlayers[i]:nullptr;
    if (i<0||i>=jSnap.count) return nullptr;
    tmp.id=jSnap.ids[i]; tmp.score=jSnap.scores[i]; tmp.active=true;
    memcpy(tmp.name, jSnap.names[i], PARTY_NAME_LEN);
    return &tmp;
}
int partyMyIndex() {
    if (pHost) return hIndexOf(pMyId);
    for (int i=0;i<jSnap.count;i++) if (jSnap.ids[i]==pMyId) return i;
    return -1;
}
int partyWhoUp() { return pHost ? hWhoUp : jSnap.whoUp; }
bool partyIsHost() { return pHost; }
bool partyIAmUp() { return partyMyIndex()==partyWhoUp() && partyMyIndex()>=0; }
PartyPhase partyPhase() { return pHost ? hPhase : (PartyPhase)jSnap.phase; }
uint32_t partyRoundSeed() { return pHost ? hSeed : jSnap.seed; }
uint16_t partyScoreTarget() { return pHost ? hTarget : jSnap.target; }
uint8_t partyGuesser() { return pHost ? hGuesser : jSnap.guesser; }
uint32_t partyMsTotal() { return pHost ? (uint32_t)hRoundSecs*1000 : jSnap.msTotal; }
uint32_t partyMsLeft() {
    if (pHost) {
        if (!hTimerRunning) return (uint32_t)hRoundSecs*1000;
        uint32_t el=millis()-hTimerStart, tot=hRoundSecs*1000;
        return (el>=tot)?0:tot-el;
    }
    return jSnap.msLeft;
}

void partyHostAward(int idx, int pts) {
    if (!pHost || idx<0 || idx>=hCount) return;
    int ns=(int)hPlayers[idx].score+pts; if (ns<0) ns=0;
    hPlayers[idx].score=(uint16_t)ns;
    if (hPlayers[idx].score>=hTarget) { hPhase=PH_OVER; hWhoUp=idx; }
}
void partyHostSetPhase(PartyPhase ph) { if (pHost) hPhase=ph; }
void partyHostNextTurn() {
    if (!pHost) return;
    int nx=hWhoUp;
    for (int k=0;k<hCount;k++){ nx=(nx+1)%hCount; if (hPlayers[nx].active) break; }
    hWhoUp=nx; hGuesser=255; hSeed=esp_random();
    hTimerRunning=false;
    hPhase=PH_ARMED;
    if (GAME && GAME->roundStart) GAME->roundStart(hSeed);
}

// ============================================================================
//  DRAW helpers shared across screens
// ============================================================================
// The glowing breathing border for non-UP players while the round is LIVE.
// Solid bright when a guess is in progress (color code: cyan for the guesser,
// magenta for everyone else).
static void drawBorder(uint16_t col, bool solid) {
    uint8_t b;
    if (solid) b = 255;
    else {
        float ph = (millis()%1600)/1600.0f;         // ~0.6 Hz breathing
        b = (uint8_t)(70 + 120*(0.5f*(1+sinf(ph*2*PI))));
    }
    // scale the color by b/255
    uint8_t r=((col>>11)&0x1F), g=((col>>5)&0x3F), bl=(col&0x1F);
    r=(r*b)/255; g=(g*b)/255; bl=(bl*b)/255;
    uint16_t c=(r<<11)|(g<<5)|bl;
    for (int t=0;t<4;t++) fb.drawRect(t,t,W-2*t,H-2*t,c);
}

// ============================================================================
//  LOBBY / SETUP / SEARCH / JOIN-PROMPT / SCOREBOARD
// ============================================================================
static void drawSearch() {
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("PARTY GAME", W/2, 40, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("looking for a game...", W/2, 150, 1);
    // animated dots
    int n=(millis()/400)%4;
    char d[5]={0}; for(int i=0;i<n;i++) d[i]='.';
    fb.drawString(d, W/2, 166, 2);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("host starts automatically", W/2, H-30, 1);
    fb.drawString("if none is found", W/2, H-18, 1);
    fb.pushSprite(0,0);
}

static void drawJoinPrompt() {
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("GAME FOUND!", W/2, 60, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("Join this party?", W/2, 120, 2);
    fb.fillRoundRect(10,170,70,50,6,TFT_RED);
    fb.fillRoundRect(90,170,70,50,6,TFT_GREEN);
    fb.setTextColor(TFT_WHITE); fb.drawString("L", 45, 188, 4);
    fb.setTextColor(TFT_WHITE); fb.drawString("No", 45, 210, 1);
    fb.setTextColor(TFT_BLACK); fb.drawString("R", 125, 188, 4);
    fb.setTextColor(TFT_BLACK); fb.drawString("Yes", 125, 210, 1);
    fb.pushSprite(0,0);
}

static void drawLobby() {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,0,W,20,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("PARTY LOBBY", W/2, 10, 2);

    int cnt = partyPlayerCount();
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("Players:", 8, 26, 1);
    int y=42;
    for (int i=0;i<cnt;i++){
        PartyPlayer* p=partyPlayer(i);
        bool me=(p && p->id==pMyId);
        fb.setTextColor(me?LIME:TEAL_LIGHT);
        char row[24]; snprintf(row,sizeof(row),"%s%s", p?p->name:"?", i==0?" (host)":"");
        fb.drawString(row, 14, y, 1); y+=14;
    }

    if (pHost) {
        static const int T[4]={5,10,15,20}, S[4]={30,60,90,120};
        fb.setTextColor(LIME_PALE);
        char b[24];
        snprintf(b,sizeof(b),"Play to %d pts", T[hCfgTarget]);
        fb.drawString(b, 8, H-70, 1);
        snprintf(b,sizeof(b),"Turn %ds", S[hCfgSecs]);
        fb.drawString(b, 8, H-56, 1);
        fb.fillRect(0,H-30,W,30,PURPLE_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString("L: change  R: START", W/2, H-20, 1);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("(hold L to switch setting)", W/2, H-8, 1);
    } else {
        fb.fillRect(0,H-24,W,24,PURPLE_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("waiting for host to start...", W/2, H-12, 1);
    }
    fb.pushSprite(0,0);
}

static void drawScores(bool over) {
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,0,W,20,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(over?LIME:LIME_PALE);
    fb.drawString(over?"WINNER!":"ROUND SCORES", W/2, 10, 2);
    int cnt=partyPlayerCount();
    fb.setTextDatum(TL_DATUM); int y=34;
    // find leader for the win highlight
    int lead=0; for(int i=1;i<cnt;i++){PartyPlayer*p=partyPlayer(i);PartyPlayer*l=partyPlayer(lead); if(p&&l&&p->score>l->score) lead=i;}
    for (int i=0;i<cnt;i++){
        PartyPlayer* p=partyPlayer(i);
        bool hl = over && (i==lead);
        fb.setTextColor(hl?LIME:TEAL_LIGHT);
        char row[24]; snprintf(row,sizeof(row),"%-10s %d", p?p->name:"?", p?p->score:0);
        fb.drawString(row, 14, y, 2); y+=22;
    }
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    if (over) fb.drawString("dbl-tap both = menu", W/2, H-7, 1);
    else if (partyIsHost()) fb.drawString("R: next turn", W/2, H-7, 1);
    else fb.drawString("next turn soon...", W/2, H-7, 1);
    fb.pushSprite(0,0);
}

// ============================================================================
//  ENTER / TICK / DRAW / SHUTDOWN
// ============================================================================
void partyEnter() {
    fb.setRotation(0);
    pEsp = espInit();
    pHost=false; hCount=0; jHaveSnap=false;
    hPhase=PH_LOBBY; hWhoUp=0; hGuesser=255;
    pScreen=SC_SEARCH; pEnterAt=millis(); pFoundHost=0;
    pLastBeacon=0; pLastSnap=0;
    hCfgTarget=1; hCfgSecs=1;
    jSeedSeen=0; jSeedInit=false;
    rxSnap=rxJoin=rxGuess=rxStart=rxBye=rxBeacon=rxRight=rxWrong=false;
}

// Become the host: seed myself as player 0.
static void becomeHost() {
    pHost=true; hCount=0;
    PartyPlayer& me=hPlayers[hCount++];
    me.id=pMyId; me.score=0; me.active=true; memcpy(me.name,pMyName,PARTY_NAME_LEN);
    hPhase=PH_LOBBY; hWhoUp=0; hGuesser=255;
    pScreen=SC_PLAY;
}

static void hostConsume() {
    if (rxJoin) { rxJoin=false;
        uint16_t jid=rxJoinId;
        if (hIndexOf(jid)<0 && hCount<PARTY_MAX_PLAYERS) {
            PartyPlayer& p=hPlayers[hCount++];
            p.id=jid; p.score=0; p.active=true;
            // use their sent nick, else derive
            if (rxJoinName[0]) { memcpy(p.name,rxJoinName,PARTY_NAME_LEN); p.name[PARTY_NAME_LEN-1]=0; }
            else nickFor(p.id,p.name);
        }
    }
    if (rxGuess) { rxGuess=false;
        // only accept the first guess during LIVE
        if (hPhase==PH_LIVE) {
            int gi=hIndexOf(rxGuessId);
            if (gi>=0 && gi!=hWhoUp) { hGuesser=(uint8_t)gi; hPhase=PH_GUESS; }
        }
    }
    if (rxBye) { rxBye=false;
        int i=hIndexOf(rxByeId); if (i>=0) hPlayers[i].active=false;
    }
    rxBeacon=false;
}

void partyTick() {
    if (!pEsp) return;
    bool lEdge=btnL&&!btnL_prev, rEdge=btnR&&!btnR_prev;
    uint32_t now=millis();

    // -------- discovery / join-prompt (joiner side, pre-game) --------
    if (!pHost && pScreen!=SC_PLAY) {
        // listen for a host snapshot; if we hear one, offer to join.
        if (rxSnap) { rxSnap=false; joinerAdoptSnap();
            pFoundHost=1;
            if (pScreen==SC_SEARCH) pScreen=SC_JOINPROMPT;
        }
        if (pScreen==SC_SEARCH) {
            // periodic beacon so simultaneous openers can detect each other
            if (now-pLastBeacon>400){ pLastBeacon=now; sendCtrl(PM_BEACON); }
            // If another searcher with a LOWER id is beaconing, they will host;
            // extend our wait so we discover their snapshot and join instead of
            // both becoming hosts.
            if (rxBeacon) { rxBeacon=false;
                if (rxBeaconId < pMyId) pEnterAt = now;  // reset our host-timer; defer
            }
            // no host found within the window -> I become host
            if (now-pEnterAt>2500 && !pFoundHost) { becomeHost(); return; }
        } else if (pScreen==SC_JOINPROMPT) {
            if (lEdge) { // No -> leave to menu (signal main via a flag)
                // we simply shut down; main's global menu gesture returns; but we
                // want an immediate exit, so emulate by going back to search then
                // let the user double-tap. Simplest: send BYE and show search.
                sendCtrl(PM_BYE); espStop();
                // re-init so the app is still alive but idle in search
                pEsp=espInit(); pScreen=SC_SEARCH; pEnterAt=millis(); pFoundHost=0; jHaveSnap=false;
            }
            if (rEdge) { // Yes -> join
                sendCtrl2(PM_JOIN); pScreen=SC_PLAY;
            }
        }
        return;
    }

    // -------- HOST authoritative logic --------
    if (pHost) {
        hostConsume();
        // lobby: adjust config + start
        static bool editSecs=false; static uint32_t lHoldT=0; static bool lHolding=false;
        if (hPhase==PH_LOBBY) {
            if (btnL&&!lHolding){lHolding=true;lHoldT=now;}
            if (!btnL&&lHolding){ lHolding=false;
                if (now-lHoldT>=450) editSecs=!editSecs;
                else { if(editSecs) hCfgSecs=(hCfgSecs+1)%4; else hCfgTarget=(hCfgTarget+1)%4; }
            }
            if (rEdge) {
                static const int T[4]={5,10,15,20}, S[4]={30,60,90,120};
                hTarget=T[hCfgTarget]; hRoundSecs=S[hCfgSecs];
                hWhoUp=0; hSeed=esp_random(); hGuesser=255; hTimerRunning=false;
                hPhase=PH_ARMED;
                if (GAME&&GAME->roundStart) GAME->roundStart(hSeed);
            }
        }
        else if (hPhase==PH_ARMED) {
            // UP player presses to start the timer. If host is UP, R starts.
            if (partyIAmUp()) {
                if (rEdge) { hTimerRunning=true; hTimerStart=now; hPhase=PH_LIVE; }
            }
            // a remote UP player would send PM_START:
            if (rxStart) { rxStart=false;
                if (!partyIAmUp() && hIndexOf(rxStartId)==hWhoUp) {
                    hTimerRunning=true; hTimerStart=now; hPhase=PH_LIVE;
                }
            }
        }
        else if (hPhase==PH_LIVE) {
            // time up -> tally + scoreboard
            if (partyMsLeft()==0) { hTimerRunning=false; hPhase=PH_SCORES; hPhaseUntil=now+4000; }
            // THE HOST IS A GUESSER TOO on every round it isn't up. Remote
            // players buzz in through hostConsume(); the host has no packet to
            // send itself, so its own buttons must be read right here. Without
            // this the host simply cannot buzz from round 2 onward - which,
            // with two devices, means nobody can guess at all.
            else if (!partyIAmUp() && hGuesser==255) {
                if (lEdge || rEdge) {
                    int mi = partyMyIndex();
                    if (mi >= 0) { hGuesser=(uint8_t)mi; hPhase=PH_GUESS; }
                }
            }
        }
        else if (hPhase==PH_GUESS) {
            // The UP player judges. If host is UP, L=wrong R=right.
            if (partyIAmUp()) {
                if (lEdge) { hGuesser=255; hPhase=PH_LIVE; }      // wrong: resume
                else if (rEdge) {                                   // right!
                    partyHostAward(hWhoUp,2);
                    if (hGuesser<hCount) partyHostAward(hGuesser,3);
                    hPhase=PH_REVEAL; hPhaseUntil=now+2500;
                }
            } else {
                // remote UP player judges via PM_START-repurpose? No: we add a
                // dedicated path — the UP joiner sends PM_GUESS with id==self to
                // mean "right", or PM_BYE-like. Keep simple: UP joiner sends
                // PM_START for RIGHT and PM_BEACON for WRONG during PH_GUESS.
                if (rxRight){ rxRight=false;
                    if (hIndexOf(rxRightId)==hWhoUp) {
                        partyHostAward(hWhoUp,2); if(hGuesser<hCount) partyHostAward(hGuesser,3);
                        if (hPhase!=PH_OVER) { hPhase=PH_REVEAL; hPhaseUntil=now+2500; }
                    }
                }
                if (rxWrong){ rxWrong=false;
                    if (hIndexOf(rxWrongId)==hWhoUp) { hGuesser=255; hPhase=PH_LIVE; }
                }
            }
        }
        else if (hPhase==PH_REVEAL) {
            if (now>hPhaseUntil) { hPhase=PH_SCORES; hPhaseUntil=now+4000; }
        }
        else if (hPhase==PH_SCORES) {
            if (rEdge || now>hPhaseUntil) partyHostNextTurn();
        }
        // Stale-flag hygiene: a START or verdict that arrived outside its phase
        // must not latch and auto-fire when that phase is next entered.
        if (hPhase!=PH_ARMED) rxStart=false;
        if (hPhase!=PH_GUESS) { rxRight=false; rxWrong=false; }
        // host streams truth every ~80ms
        if (now-pLastSnap>80){ pLastSnap=now; hostBroadcastState(); }
        return;
    }

    // -------- JOINER: mirror snapshot + send my inputs --------
    if (rxSnap){ rxSnap=false; joinerAdoptSnap(); }
    PartyPhase ph=(PartyPhase)jSnap.phase;
    if (ph==PH_ARMED && partyIAmUp()) {
        if (rEdge) sendCtrl2(PM_START);     // I'm UP: start the timer
    }
    else if (ph==PH_LIVE && !partyIAmUp()) {
        if (lEdge||rEdge) sendCtrl2(PM_GUESS); // buzz in to guess
    }
    else if (ph==PH_GUESS && partyIAmUp()) {
        if (rEdge) sendCtrl2(PM_RIGHT);     // judge: correct
        else if (lEdge) sendCtrl2(PM_WRONG);// judge: wrong, resume
    }
}

void partyDraw() {
    if (!pHost && pScreen==SC_SEARCH)      { drawSearch(); return; }
    if (!pHost && pScreen==SC_JOINPROMPT)  { drawJoinPrompt(); return; }
    if (!pHost && !jHaveSnap)              { drawSearch(); return; }

    PartyPhase ph=partyPhase();
    switch (ph) {
        case PH_LOBBY:  drawLobby(); break;
        case PH_ARMED: {
            bool up=partyIAmUp();
            if (up) {
                GAME->drawPerform();          // prompt + hourglass (fills frame)
                fb.fillRect(0,H-28,W,28,PURPLE_DARK);
                fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
                fb.drawString("R: START TIMER", W/2, H-14, 2);
                fb.pushSprite(0,0);
            } else {
                fb.fillSprite(TEAL_DEEP);
                fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
                PartyPlayer* u=partyPlayer(partyWhoUp());
                char b[24]; snprintf(b,sizeof(b),"%s is up", u?u->name:"?");
                fb.drawString(b, W/2, H/2-10, 2);
                fb.setTextColor(TEAL_DARK);
                fb.drawString("get ready to guess...", W/2, H/2+14, 1);
                fb.pushSprite(0,0);
            }
            break;
        }
        case PH_LIVE: {
            if (partyIAmUp()) { GAME->drawPerform(); }
            else { GAME->drawWatch(); drawBorder(TEAL_LIGHT,false); }  // breathing
            fb.pushSprite(0,0);
            break;
        }
        case PH_GUESS: {
            if (partyIAmUp()) { GAME->drawJudge(); }
            else {
                GAME->drawWatch();
                bool iGuessed=(partyMyIndex()==partyGuesser());
                drawBorder(iGuessed?TFT_CYAN:TFT_MAGENTA, true);       // solid bright
            }
            fb.pushSprite(0,0);
            break;
        }
        case PH_REVEAL: GAME->drawReveal(); fb.pushSprite(0,0); break;
        case PH_SCORES: drawScores(false); break;
        case PH_OVER:   drawScores(true);  break;
    }
}

void partyShutdown() { espStop(); }
