// FILE: src/mantisfish/mantisfish_mode.cpp
// MANTIS FISHING - the pond off the end of the dock.
//
// The draft's two dead mechanics are replaced with real games:
//  * CAST is a sweeping power meter (R locks it). Distance picks the depth
//    zone, and the far water holds the better fish. Overshooting the meter
//    snags the cast short.
//  * THE BITE IS A TIMING GAME, not a per-frame dice roll: the bobber drifts,
//    nibbles twitch it (fakes!), and a real strike plunges it with a "!" -
//    hook inside the strike window or the fish is gone. Striking a nibble
//    spooks the water.
//  * THE FIGHT is a tension tug-of-war: hold R to reel (tension climbs fast
//    while the fish runs, slow while it rests), tap L to give line (dumps
//    tension, loses progress). Max tension snaps the line. Fish stamina
//    drains as you play it right; surges weaken until it comes to the net.
//
// LIVE CATCH: landing a trout- or salmon-family fish rolls a rare chance the
// fish comes in ALIVE - it rides the quest-profile pending bus to the
// AQUARIUM, where Trout and Salmon are new hidden species that never appear
// in the shop until unlocked this way.
//
// VERSUS: 3-minute FISH-OFF derby over ESP-NOW (house convention throughout:
// HELLO 300ms / CONFIRM / roles by id / copy-only recv callback / seq-dedup /
// BYE + teardown / 15 s search timeout). Live rival weight beats. Radio never
// comes up in solo.
//
// ECONOMY: first catch of each species pays that species' XP (a discrete
// event, like a Sightings check-off). Eight one-time TACKLE MEDALS pay 10-80
// XP. Derby wins pay +30 XP / +8 pet. NVS "mfish" writes on catch (10 s
// coalesced) + shutdown flush.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "mantisfish_mode.h"
#include "../power_guard.h"
#include "mantisfish_sprites.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;
static constexpr int WATER_Y = 120;          // dock sits above, water below
static constexpr uint32_t FI_LONG_MS = 420, FI_ENTER_GUARD = 250;
static constexpr uint32_t FI_HELLO_MS = 300, FI_SEARCH_MS = 15000;
static constexpr uint32_t FI_STATUS_MS = 1000, FI_PEER_LOST = 8000;
static constexpr int      DERBY_FRAMES = 180 * 62;
static constexpr int      LIVE_CHANCE_PCT = 5;

// ---- fish table -------------------------------------------------------------
struct FishDef {
    const char* name;
    const uint8_t* sprite;
    uint8_t rarity;          // 1..4
    uint8_t minZone;         // 0 near, 1 mid, 2 far - deepest zone it prefers
    uint16_t wLo, wHi;       // weight range in deci-pounds
    uint8_t xp;              // first-catch XP
    uint8_t family;          // 0 pan, 1 trout, 2 salmon
    uint8_t strength;        // fight difficulty 1..5
};
static const FishDef FISH[8] = {
    { "BLUEGILL",        FI_BLUEGILL, 1, 0,  2,  8,   5, 0, 1 },
    { "YELLOW PERCH",    FI_PERCH,    1, 0,  3, 12,   5, 0, 1 },
    { "RAINBOW TROUT",   FI_RAINBOW,  2, 1,  8, 35,  10, 1, 2 },
    { "BROWN TROUT",     FI_BROWN,    2, 1, 10, 45,  10, 1, 3 },
    { "SMALLMOUTH BASS", FI_SMBASS,   2, 1, 10, 30,  10, 0, 3 },
    { "CUTTHROAT TROUT", FI_CUTT,     3, 2, 10, 40,  15, 1, 3 },
    { "KOKANEE SALMON",  FI_KOKANEE,  3, 2, 15, 40,  15, 2, 4 },
    { "GOLDEN TROUT",    FI_GOLDEN,   4, 2,  5, 20,  30, 1, 5 },
};

// ---- persistence ------------------------------------------------------------
static const char* NVS_FI = "mfish";
struct __attribute__((packed)) FiSave {
    uint16_t magic;          // 0x4946 'FI'
    uint8_t  version;
    uint8_t  caught;         // species bitmask
    uint16_t best[8];        // best weight per species (deci-lb)
    uint16_t total;          // lifetime catches
    uint8_t  ach;
    uint8_t  liveDone;       // bit0 trout sent, bit1 salmon sent
    uint16_t vsWins;
};
static FiSave fiSv;
static bool fiDirty = false;

static void fiDefaults(){ memset(&fiSv,0,sizeof(fiSv)); fiSv.magic=0x4946; fiSv.version=1; }
static void fiLoad(){
    fiDefaults();
    Preferences p;
    if(!p.begin(NVS_FI,true)) return;
    FiSave t; size_t n=p.getBytes("blob",&t,sizeof(t)); p.end();
    if(n==sizeof(t)&&t.magic==0x4946&&t.version==1) fiSv=t;
}
static void fiFlush(){
    if(!fiDirty) return;
    Preferences p;
    if(!p.begin(NVS_FI,false)) return;
    p.putBytes("blob",&fiSv,sizeof(fiSv)); p.end();
    fiDirty=false;
}

// Hand a live catch to the Aquarium via its own tiny NVS inbox ("aqlive").
static void fiSendLiveFish(bool salmon) {
    Preferences p;
    if (!p.begin("aqlive", false)) return;
    p.putUChar(salmon ? "salmon" : "trout", 1);
    p.end();
}

// ---- medals -----------------------------------------------------------------
enum FiAch:uint8_t{ ACH_FIRSTCATCH=0, ACH_CREEL, ACH_TROPHY, ACH_GOLDEN,
                    ACH_LIVEONE, ACH_DERBY, ACH_CENTURY, ACH_NOESCAPE };
struct FiAchDef{ const char* name; const char* how; uint8_t xp; };
static const FiAchDef ACHS[8]={
    {"FIRST CATCH",  "Land your first fish",       10},
    {"FULL CREEL",   "Catch all 8 species",        80},
    {"TROPHY WALL",  "Land a near-max weight fish",40},
    {"GOLDEN HOUR",  "Land the Golden Trout",      50},
    {"THE LIVE ONE", "Send a live fish home",      30},
    {"DERBY CHAMP",  "Win a Fish-Off",             50},
    {"CENTURY CLUB", "100 lifetime catches",       40},
    {"NO ESCAPE",    "Land 5 in a row, no losses", 30},
};
static uint8_t achQueue[4]; static int achQueueN=0;
static void fiAward(uint8_t a){
    if((fiSv.ach>>a)&1) return;
    fiSv.ach|=(uint8_t)(1<<a);
    qpAddXp(ACHS[a].xp);
    fiDirty=true;
    if(achQueueN<4) achQueue[achQueueN++]=a;
}

// ---- protocol ---------------------------------------------------------------
enum FiMsgType:uint8_t{ FM_HELLO=1, FM_CONFIRM, FM_START, FM_STATUS, FM_BYE };
struct __attribute__((packed)) FiMsg {
    uint16_t magic;          // 0x4946
    uint8_t type, seq;
    uint32_t id, seed;
    uint16_t weight;         // STATUS: total deci-lb this derby
    uint8_t  count, finished;
};
static constexpr uint16_t FI_MAGIC=0x4946;
static const uint8_t FI_BCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static volatile bool rxHello,rxConfirm,rxStart,rxStatus,rxBye;
static uint8_t rxMac[6]; static uint32_t rxId,rxSeed;
static uint16_t rxWeight; static uint8_t rxCount,rxFinished;
static uint16_t rxStartSeqSeen=0x1FF;
static uint32_t fiMyId=0; static uint8_t fiPeer[6];
static bool fiPeerKnown=false,fiLinked=false,fiEspReady=false;
static bool fiConfL=false,fiConfP=false;
static uint32_t fiLastHello=0,fiLastStatus=0,fiLastPeerSeen=0,fiSearchT0=0;
static uint8_t fiSeq=0; static bool fiIsHost=false;
static uint32_t fiStartSeed=1;

static bool derby=false, derbyDone=false, derbyWon=false;
static int  derbyFrames=0;
static uint16_t myDerbyW=0; static uint8_t myDerbyN=0;
static uint16_t rvW=0; static uint8_t rvN=0;
static bool rvDone=false, rvGone=false;

static void fiOnRecv(const uint8_t* mac,const uint8_t* data,int len){
    if((size_t)len<sizeof(FiMsg)) return;
    const FiMsg* m=(const FiMsg*)data;
    if(m->magic!=FI_MAGIC||m->id==fiMyId) return;
    memcpy((void*)rxMac,mac,6); rxId=m->id;
    switch(m->type){
        case FM_HELLO: rxHello=true; break;
        case FM_CONFIRM: rxConfirm=true; break;
        case FM_START:
            if((uint16_t)m->seq!=rxStartSeqSeen){ rxStartSeqSeen=m->seq; rxSeed=m->seed; rxStart=true; }
            break;
        case FM_STATUS: rxWeight=m->weight; rxCount=m->count; rxFinished=m->finished; rxStatus=true; break;
        case FM_BYE: rxBye=true; break;
    }
}
static void fiClearRx(){ rxHello=rxConfirm=rxStart=rxStatus=rxBye=false; }
static void fiSend(FiMsgType t,const uint8_t* to){
    FiMsg m; memset(&m,0,sizeof(m));
    m.magic=FI_MAGIC; m.type=t; m.id=fiMyId; m.seq=++fiSeq;
    if(t==FM_START) m.seed=fiStartSeed;
    if(t==FM_STATUS){ m.weight=myDerbyW; m.count=myDerbyN; m.finished=derbyDone?1:0; }
    esp_now_send(to,(uint8_t*)&m,sizeof(m));
    if(t==FM_START){ delay(4); esp_now_send(to,(uint8_t*)&m,sizeof(m)); }
}
static bool fiAddPeer(const uint8_t* mac){
    esp_now_peer_info_t p; memset(&p,0,sizeof(p));
    memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
    if(esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p)==ESP_OK;
}
static bool fiRadioUp(){
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    fiMyId=((uint32_t)mac[3]<<16)|((uint32_t)mac[4]<<8)|mac[5];
    if(esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(fiOnRecv));
    fiAddPeer(FI_BCAST);
    return true;
}
static void fiRadioDown(bool bye){
    if(fiEspReady){
        if(bye&&fiPeerKnown) fiSend(FM_BYE,fiPeer);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        fiEspReady=false;
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    fiPeerKnown=fiLinked=false; fiConfL=fiConfP=false;
}

// ---- state machine ----------------------------------------------------------
enum FiScreen:uint8_t{ SC_MENU=0, SC_LOBBY, SC_FISH, SC_CARD, SC_TACKLE, SC_LINKLOST, SC_DERBYEND };
enum FiPhase:uint8_t{ PH_READY=0, PH_CASTING, PH_WAIT, PH_FIGHT };
static FiScreen scr=SC_MENU;
static FiPhase  ph=PH_READY;
static int menuSel=0, tackleSel=0;
static uint32_t enterAt=0, frameNo=0;
static uint32_t rng=1;
static uint32_t frand(){ rng=rng*1664525u+1013904223u; return rng>>8; }

// cast/wait/fight state
static float castMeter=0; static int castDir=1;
static int  zone=0;                              // 0 near 1 mid 2 far
static float bobX,bobY,bobBase;
static int  biteTimer=0;                         // frames to next event
static int  nibbleT=0;                           // twitch animation
static int  strikeT=0;                           // strike window remaining
static int  spookT=0;
static int  hookedFish=-1;
static uint16_t hookedW=0;
static float lineOut, tension, stamina;
static int  surgeT=0; static bool surging=false;
static int  streak=0;
static int  cardFish=-1; static uint16_t cardW=0;
static bool cardNew=false, cardRecord=false, cardLive=false, cardTrophy=false;
static uint8_t cardXp=0;
static int  cardT=0;
static int  splashT=0; static float splashX;

// ---- input ------------------------------------------------------------------
static bool lTap,rTap,lHoldFire,rHoldFire;
static bool lDown,rDown,lLongDone,rLongDone;
static uint32_t lDownAt,rDownAt;
static void fiInput(){
    lTap=rTap=lHoldFire=rHoldFire=false;
    uint32_t now=millis();
    if(btnL&&!btnL_prev){lDown=true;lDownAt=now;lLongDone=false;}
    if(btnL&&lDown&&!lLongDone&&now-lDownAt>FI_LONG_MS){lLongDone=true;lHoldFire=true;}
    if(!btnL&&btnL_prev){if(lDown&&!lLongDone)lTap=true;lDown=false;}
    if(btnR&&!btnR_prev){rDown=true;rDownAt=now;rLongDone=false;}
    if(btnR&&rDown&&!rLongDone&&now-rDownAt>FI_LONG_MS){rLongDone=true;rHoldFire=true;}
    if(!btnR&&btnR_prev){if(rDown&&!rLongDone)rTap=true;rDown=false;}
    if(now-enterAt<FI_ENTER_GUARD){lTap=rTap=lHoldFire=rHoldFire=false;}
}

// ---- fishing logic ----------------------------------------------------------
static void fiToReady(){
    ph=PH_READY; hookedFish=-1; strikeT=0; nibbleT=0; spookT=0;
}
static void fiScheduleBite(){
    biteTimer = 120 + (int)(frand()% (spookT>0 ? 420 : 300));
    if(spookT>0) spookT=0;
}
static void fiCast(){
    // meter value -> zone; overshoot snags short
    float m=castMeter;
    if(m>0.95f){ zone=0; splashT=14; }
    else if(m>0.62f) zone=2;
    else if(m>0.32f) zone=1;
    else zone=0;
    bobX = 46 + zone*42 + (int)(frand()%18);
    bobBase = WATER_Y + 10 + zone*3;
    bobY = bobBase;
    splashX=bobX; splashT=12;
    ph=PH_WAIT;
    fiScheduleBite();
}
static int fiPickFish(){
    // weight species by rarity and zone fit; deeper zones open the good table
    int w[8], tot=0;
    for(int i=0;i<8;i++){
        int base = FISH[i].rarity==1?40:FISH[i].rarity==2?22:FISH[i].rarity==3?10:3;
        if(zone<FISH[i].minZone) base=(FISH[i].minZone-zone>1)?0:base/4;
        w[i]=base; tot+=base;
    }
    int r=(int)(frand()%(uint32_t)(tot>0?tot:1));
    for(int i=0;i<8;i++){ r-=w[i]; if(r<0) return i; }
    return 0;
}
static void fiStrike(){
    hookedFish=fiPickFish();
    const FishDef& f=FISH[hookedFish];
    hookedW = f.wLo + (uint16_t)(frand()%(uint32_t)(f.wHi-f.wLo+1));
    strikeT = 28;                                // ~0.45 s window
    bobY = bobBase+6;
}
static void fiHook(){
    ph=PH_FIGHT;
    const FishDef& f=FISH[hookedFish];
    lineOut = 0.55f + zone*0.15f;
    tension = 30;
    stamina = 60 + f.strength*28 + hookedW*0.6f;
    surging=true; surgeT=40+(int)(frand()%30);
    splashX=bobX; splashT=14;
}
static void fiLose(const char* why){
    (void)why;
    streak=0;
    splashX=bobX; splashT=16;
    fiToReady();
}
static void fiLand(){
    const FishDef& f=FISH[hookedFish];
    cardFish=hookedFish; cardW=hookedW;
    cardNew = !((fiSv.caught>>hookedFish)&1);
    cardXp = cardNew ? f.xp : 0;
    if(cardNew){ fiSv.caught|=(uint8_t)(1<<hookedFish); qpAddXp(f.xp); }
    cardRecord = hookedW > fiSv.best[hookedFish];
    if(cardRecord) fiSv.best[hookedFish]=hookedW;
    cardTrophy = hookedW >= f.wLo + (uint16_t)((f.wHi-f.wLo)*9/10);
    if(cardTrophy) fiAward(ACH_TROPHY);
    fiSv.total++;
    streak++;
    fiAward(ACH_FIRSTCATCH);
    if(fiSv.caught==0xFF) fiAward(ACH_CREEL);
    if(hookedFish==7) fiAward(ACH_GOLDEN);
    if(fiSv.total>=100) fiAward(ACH_CENTURY);
    if(streak>=5) fiAward(ACH_NOESCAPE);
    // the live one: trout/salmon families only, rare, once per family
    cardLive=false;
    if(f.family>0 && (int)(frand()%100)<LIVE_CHANCE_PCT){
        uint8_t bit = (f.family==2)?2:1;
        if(!(fiSv.liveDone&bit)){
            fiSv.liveDone|=bit;
            fiSendLiveFish(f.family==2);
            cardLive=true;
            fiAward(ACH_LIVEONE);
        } else qpGrantPetXp(10);
    }
    if(f.rarity>=4) qpGrantPetXp(10);
    if(derby){ myDerbyW+=hookedW; myDerbyN++; }
    fiDirty=true;
    fiFlush();          // a landed fish is a discrete event: write it now
    cardT=0;
    scr=SC_CARD;
    fiToReady();
}

static void fiNewSession(bool derbyMode, uint32_t seed){
    rng=seed|1;
    derby=derbyMode; derbyDone=false; derbyWon=false;
    derbyFrames=DERBY_FRAMES;
    myDerbyW=0; myDerbyN=0; rvW=0; rvN=0; rvDone=false; rvGone=false;
    streak=0; achQueueN=0;
    fiToReady();
    scr=SC_FISH;
}

static void fiTickNet(){
    if(!derby||!fiEspReady) return;
    uint32_t now=millis();
    if(rxBye){ rxBye=false; fiRadioDown(false); rvGone=true; }
    if(rxStatus){
        rxStatus=false; fiLastPeerSeen=now;
        rvW=rxWeight; rvN=rxCount; rvDone=rxFinished!=0;
    }
    if(fiLinked && now-fiLastStatus>FI_STATUS_MS){ fiSend(FM_STATUS,fiPeer); fiLastStatus=now; }
    if(fiLinked && now-fiLastPeerSeen>FI_PEER_LOST){ fiRadioDown(false); rvGone=true; }
}

static void fiSettleDerby(){
    if(!rvGone){
        derbyWon = (myDerbyW>rvW)||(myDerbyW==rvW&&myDerbyN>=rvN);
        if(derbyWon){ fiSv.vsWins++; qpAddXp(30); qpGrantPetXp(8); fiAward(ACH_DERBY); fiDirty=true; }
    } else derbyWon=false;
    fiFlush();
    if(fiEspReady) fiRadioDown(true);
    scr=SC_DERBYEND;
}

static void fiTickFishing(){
    frameNo++;
    fiTickNet();
    if(derby){
        if(!derbyDone && --derbyFrames<=0){
            derbyDone=true;
            if(fiLinked){ fiSend(FM_STATUS,fiPeer); fiLastStatus=millis(); }
        }
        if(derbyDone){
            if(rvDone||rvGone||!fiLinked){ fiSettleDerby(); return; }
        }
    }
    if(splashT>0) splashT--;

    switch(ph){
    case PH_READY:
        if(rTap){ ph=PH_CASTING; castMeter=0; castDir=1; }
        if(lHoldFire && !derby){ fiFlush(); scr=SC_MENU; }
        break;
    case PH_CASTING:
        castMeter += 0.028f*castDir;
        if(castMeter>1){castMeter=1;castDir=-1;}
        if(castMeter<0){castMeter=0;castDir=1;}
        if(rTap) fiCast();
        if(lTap) fiToReady();
        break;
    case PH_WAIT:
        bobY = bobBase + sinf(frameNo*0.06f)*1.2f + (nibbleT>0?((frameNo&2)?2:-1):0) + (strikeT>0?6:0);
        if(nibbleT>0) nibbleT--;
        if(strikeT>0){
            if(--strikeT==0){ hookedFish=-1; fiScheduleBite(); }   // missed it
            if(rTap){ fiHook(); break; }
        } else {
            if(rTap){ // struck at nothing (or a nibble): spook the water
                if(nibbleT>0){ nibbleT=0; spookT=1; splashX=bobX; splashT=10; }
                fiScheduleBite();
            }
            if(--biteTimer<=0){
                if((int)(frand()%100)<38){ nibbleT=26; fiScheduleBite(); }  // fake
                else fiStrike();
            }
        }
        if(lTap) fiToReady();                    // reel the bobber in
        if(lHoldFire && !derby){ fiFlush(); scr=SC_MENU; }
        break;
    case PH_FIGHT: {
        const FishDef& f=FISH[hookedFish];
        if(surgeT>0) surgeT--;
        else { surging=!surging; surgeT= surging ? 28+(int)(frand()%(uint32_t)(20+f.strength*8))
                                                : 30+(int)(frand()%40); }
        float pull = surging ? (0.5f+f.strength*0.28f)*(stamina>40?1.0f:0.6f) : 0.12f;
        if(btnR){                                 // reeling
            tension += pull*1.6f + 0.5f;
            lineOut -= 0.004f + (surging?0.0f:0.004f);
        } else {
            tension -= 1.4f;
            lineOut += surging?0.0035f:0.0008f;
        }
        if(lTap){ tension-=26; lineOut+=0.05f; }  // give line
        if(tension<0) tension=0;
        stamina -= surging?0.16f:0.05f;
        if(stamina<0) stamina=0;
        if(stamina<=0 && surging){ surging=false; surgeT=40; }
        bobY = bobBase+4+sinf(frameNo*0.3f)*2;
        bobX += surging ? ((frameNo&16)?0.3f:-0.3f) : 0;
        if(tension>=100){ fiLose("SNAP"); break; }
        if(lineOut>1.35f){ fiLose("RAN"); break; }
        if(lineOut<=0.02f){ fiLand(); break; }
        break; }
    }
}

// ---- draw -------------------------------------------------------------------
static void fiBlit(const uint8_t* sp,int sw,int sh,int x,int y,int scale=1,bool hflip=false){
    int rowB=(sw+1)/2;
    for(int py=0;py<sh;py++) for(int px=0;px<sw;px++){
        uint8_t b=sp[py*rowB+px/2];
        uint8_t v=(px&1)?(b>>4):(b&0x0F);
        if(!v||v>FI_PAL_N) continue;
        int dx=hflip?(sw-1-px):px;
        if(scale==1) fb.drawPixel(x+dx,y+py,FI_PAL[v-1]);
        else fb.fillRect(x+dx*scale,y+py*scale,scale,scale,FI_PAL[v-1]);
    }
}
static void fiFooter(const char* l,const char* r){
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString(l,4,H-7,1);
    fb.setTextDatum(MR_DATUM); fb.drawString(r,W-4,H-7,1);
}
static void drawPond(){
    fb.fillSprite(TEAL_DEEP);
    // dusk sky bands + moon
    fb.fillRect(0,0,W,26,PURPLE_DEEP);
    fb.fillCircle(W-26,30,7,LIME_PALE); fb.fillCircle(W-29,28,6,TEAL_DEEP);
    // far shore
    for(int x=0;x<W;x+=2){ int y=WATER_Y-8-(int)(sinf(x*0.07f)*4); fb.drawPixel(x,y,TEAL_DARK); }
    // water
    fb.fillRect(0,WATER_Y,W,H-14-WATER_Y,TEAL_DARK);
    for(int y=WATER_Y;y<H-14;y+=14){
        int off=(int)((frameNo/3+y)%28);
        for(int x=-off;x<W;x+=28) fb.fillRect(x,y,9,1,TEAL);
    }
    // zone hints
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL);
    fb.drawString("NEAR",56,WATER_Y+2,1);
    fb.drawString("MID",98,WATER_Y+2,1);
    fb.drawString("FAR",140,WATER_Y+2,1);
    // dock + angler (dock extends left edge)
    fb.fillRect(0,96,34,6,PURPLE_DARK);
    fb.fillRect(0,96,34,1,PURPLE);
    fiBlit(ph==PH_FIGHT?FA_HOOK:FA_IDLE,FA_IDLE_W,FA_IDLE_H,4,78);
    // line + bobber
    if(ph==PH_WAIT||ph==PH_FIGHT){
        int tipx = ph==PH_FIGHT?25:27, tipy = ph==PH_FIGHT?78:80;
        fb.drawLine(tipx,tipy,(int)bobX,(int)bobY-3,TEAL_LIGHT);
        fiBlit(FA_BOBBER,FA_BOBBER_W,FA_BOBBER_H,(int)bobX-4,(int)bobY-4);
        if(strikeT>0){
            fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
            fb.drawString("!",(int)bobX,(int)bobY-16,4);
        }
    }
    if(splashT>0){
        for(int i=0;i<4;i++){
            fb.drawPixel((int)splashX-5+i*3,(int)bobBase-2-(12-splashT)/3-i%2,TEAL_LIGHT);
        }
    }
    // HUD
    char b[24];
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"CREEL %d/8",__builtin_popcount(fiSv.caught));
    fb.drawString(b,3,3,1);
    fb.setTextDatum(TR_DATUM); fb.setTextColor(TEAL_LIGHT);
    snprintf(b,sizeof(b),"%u fish",(unsigned)fiSv.total);
    fb.drawString(b,W-3,3,1);
    if(derby){
        fb.fillRect(0,12,W,12,PURPLE_DEEP);
        int secs=derbyFrames/62;
        fb.setTextDatum(ML_DATUM); fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"YOU %u.%u lb (%d)",myDerbyW/10,myDerbyW%10,(int)myDerbyN);
        fb.drawString(b,3,18,1);
        fb.setTextDatum(MR_DATUM);
        fb.setTextColor(rvGone?TEAL_DARK:TEAL_LIGHT);
        if(rvGone) fb.drawString("RIVAL LOST",W-3,18,1);
        else { snprintf(b,sizeof(b),"RV %u.%u  %d:%02d",rvW/10,rvW%10,secs/60,secs%60); fb.drawString(b,W-3,18,1); }
    }
    // phase UI
    if(ph==PH_CASTING){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("CAST",W/2,H-52,2);
        fb.drawRect(14,H-40,W-28,8,TEAL);
        int fw=(int)(castMeter*(W-30));
        if(fw>0) fb.fillRect(15,H-39,fw,6,castMeter>0.95f?PURPLE_LIGHT:(castMeter>0.62f?LIME:TEAL_BRIGHT));
        fiFooter("L: cancel","R: cast!");
    } else if(ph==PH_FIGHT){
        const FishDef& f=FISH[hookedFish];
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(surging?PURPLE_LIGHT:TEAL_LIGHT);
        fb.drawString(surging?"IT'S RUNNING!":"IT'S TIRING...",W/2,H-64,1);
        // tension
        fb.drawRect(14,H-54,W-28,7,TEAL);
        int tw=(int)(tension*(W-30)/100);
        if(tw>0) fb.fillRect(15,H-53,tw,5,tension>75?PURPLE_LIGHT:LIME);
        fb.setTextColor(TEAL_DARK); fb.drawString("TENSION",W/2,H-44,1);
        // line progress (fills as the fish comes in)
        fb.drawRect(14,H-36,W-28,5,TEAL);
        int lw=(int)((1.35f-lineOut)/1.35f*(W-30));
        if(lw>0) fb.fillRect(15,H-35,lw,3,TEAL_BRIGHT);
        (void)f;
        fiFooter("L: give line","R: reel");
    } else if(ph==PH_WAIT){
        fiFooter(derby?"L: reel in":"L: reel  hold: menu","R: hook on strike");
    } else {
        fiFooter(derby?"":"hold L: menu","R: cast");
    }
}
static void drawCard(){
    drawPond();
    const FishDef& f=FISH[cardFish];
    fb.fillRoundRect(12,60,W-24,168,8,TEAL_DEEP);
    fb.drawRoundRect(12,60,W-24,168,8,LIME);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(cardNew?"NEW SPECIES!":"LANDED!",W/2,76,2);
    fiBlit(f.sprite,24,24,W/2-24,88,2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(f.name,W/2,142,1);
    char b[24];
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b,sizeof(b),"%u.%u lb",cardW/10,cardW%10);
    fb.drawString(b,W/2,158,2);
    int y=174;
    if(cardRecord){ fb.setTextColor(LIME); fb.drawString("PERSONAL BEST!",W/2,y,1); y+=12; }
    if(cardXp){ fb.setTextColor(LIME); snprintf(b,sizeof(b),"+%d XP",cardXp); fb.drawString(b,W/2,y,1); y+=12; }
    if(cardLive){
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("IT'S ALIVE!",W/2,y,1); y+=11;
        fb.drawString(f.family==2?"SALMON -> AQUARIUM":"TROUT -> AQUARIUM",W/2,y,1); y+=12;
    }
    for(int i=0;i<achQueueN&&y<222;i++,y+=11){
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"MEDAL: %s",ACHS[achQueue[i]].name);
        fb.drawString(b,W/2,y,1);
    }
    fiFooter("","L/R: keep fishing");
}
static void drawMenu(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,110,W,H-124,TEAL_DARK);
    for(int x=0;x<W;x+=24) fb.fillRect(x,130,9,1,TEAL);
    fiBlit(FA_IDLE,FA_IDLE_W,FA_IDLE_H,W/2-24,60,2);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS",W/2,24,4);
    fb.drawString("FISHING",W/2,46,4);
    static const char* ITEMS[3]={"FISH THE POND","FISH-OFF DERBY","TACKLE BOX"};
    char sub[3][26];
    snprintf(sub[0],26,"%d/8 species  %u fish",__builtin_popcount(fiSv.caught),(unsigned)fiSv.total);
    snprintf(sub[1],26,"3 min duel over ESP-NOW");
    { int got=0; for(int i=0;i<8;i++) if((fiSv.ach>>i)&1) got++;
      snprintf(sub[2],26,"%d/8 medals - %u derby wins",got,(unsigned)fiSv.vsWins); }
    for(int i=0;i<3;i++){
        int y=150+i*52; bool sel=(i==menuSel);
        fb.fillRoundRect(10,y,W-20,44,5,sel?TEAL_DARK:TEAL_DEEP);
        fb.drawRoundRect(10,y,W-20,44,5,sel?LIME:TEAL_DARK);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(sel?LIME_PALE:TEAL_BRIGHT);
        fb.drawString(ITEMS[i],W/2,y+15,2);
        fb.setTextColor(sel?TEAL_LIGHT:TEAL_DARK);
        fb.drawString(sub[i],W/2,y+32,1);
    }
    fiFooter("L: next","R: select");
}
static void drawLobby(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("FINDING A RIVAL",W/2,60,2);
    float a=(millis()%2000)*0.00314f;
    fb.drawCircle(W/2,150,38,TEAL_DARK); fb.drawCircle(W/2,150,24,TEAL_DARK);
    fb.drawLine(W/2,150,W/2+(int)(cosf(a)*38),150+(int)(sinf(a)*38),LIME);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(fiPeerKnown?"RIVAL FOUND - LINKING":"hold devices close",W/2,212,1);
    char t[12];
    snprintf(t,sizeof(t),"%us",(unsigned)((FI_SEARCH_MS-(millis()-fiSearchT0))/1000));
    fb.drawString(t,W/2,230,1);
    fiFooter("L: cancel","");
}
static void drawTackle(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(tackleSel==0?"CREEL RECORDS":"TACKLE MEDALS",4,9,1);
    char b[26];
    if(tackleSel==0){
        for(int i=0;i<8;i++){
            int y=22+i*34;
            bool have=(fiSv.caught>>i)&1;
            fb.fillRoundRect(4,y,W-8,31,4,TEAL_DEEP);
            fb.drawRoundRect(4,y,W-8,31,4,have?LIME_DARK:TEAL_DARK);
            fiBlit(FISH[i].sprite,24,24,7,y+3);
            fb.setTextDatum(ML_DATUM);
            fb.setTextColor(have?LIME_PALE:TEAL_DARK);
            fb.drawString(have?FISH[i].name:"? ? ?",36,y+10,1);
            if(have){
                fb.setTextColor(TEAL_LIGHT);
                snprintf(b,sizeof(b),"best %u.%u lb",fiSv.best[i]/10,fiSv.best[i]%10);
                fb.drawString(b,36,y+22,1);
            }
        }
    } else {
        for(int i=0;i<8;i++){
            int y=22+i*34;
            bool have=(fiSv.ach>>i)&1;
            fb.fillRoundRect(4,y,W-8,31,4,TEAL_DEEP);
            fb.drawRoundRect(4,y,W-8,31,4,have?LIME_DARK:TEAL_DARK);
            fb.setTextDatum(ML_DATUM);
            fb.setTextColor(have?LIME:TEAL_LIGHT);
            fb.drawString(ACHS[i].name,10,y+9,1);
            fb.setTextColor(have?LIME_PALE:TEAL_DARK);
            fb.drawString(ACHS[i].how,10,y+21,1);
        }
    }
    fiFooter("L: page","R: back");
}
static void drawDerbyEnd(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRoundRect(10,70,W-20,150,8,TEAL_DEEP);
    fb.drawRoundRect(10,70,W-20,150,8,derbyWon?LIME:PURPLE_LIGHT);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(derbyWon?LIME:PURPLE_LIGHT);
    fb.drawString(derbyWon?"DERBY CHAMP!":(rvGone?"RIVAL LOST":"RIVAL WINS"),W/2,90,2);
    char b[26];
    fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"YOU   %u.%u lb (%d)",myDerbyW/10,myDerbyW%10,(int)myDerbyN);
    fb.drawString(b,W/2,118,2);
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b,sizeof(b),"RIVAL %u.%u lb (%d)",rvW/10,rvW%10,(int)rvN);
    fb.drawString(b,W/2,138,2);
    if(derbyWon){ fb.setTextColor(TEAL_LIGHT); fb.drawString("+30 XP",W/2,158,1); }
    int y=174;
    for(int i=0;i<achQueueN&&y<212;i++,y+=12){
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"MEDAL: %s",ACHS[achQueue[i]].name);
        fb.drawString(b,W/2,y,1);
    }
    fiFooter("L: menu","R: menu");
}

void mantisfishDraw(){
    switch(scr){
    case SC_MENU: drawMenu(); break;
    case SC_LOBBY: drawLobby(); break;
    case SC_FISH: drawPond(); break;
    case SC_CARD: drawCard(); break;
    case SC_TACKLE: drawTackle(); break;
    case SC_DERBYEND: drawDerbyEnd(); break;
    case SC_LINKLOST:
        fb.fillSprite(TEAL_DEEP);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("LINK LOST",W/2,130,4);
        fiFooter("","L/R: menu");
        break;
    }
    fb.pushSprite(0,0);
}

// ---- lobby + top-level ------------------------------------------------------
static void fiTickLobby(){
    uint32_t now=millis();
    if(now-fiLastHello>FI_HELLO_MS){ fiSend(FM_HELLO,FI_BCAST); fiLastHello=now; }
    if(rxHello&&!fiPeerKnown){
        rxHello=false;
        memcpy(fiPeer,(const void*)rxMac,6);
        fiAddPeer(fiPeer);
        fiPeerKnown=true;
        fiIsHost=(fiMyId<rxId);
        fiSend(FM_CONFIRM,fiPeer);
        fiConfL=true;
    }
    if(rxConfirm&&fiPeerKnown){
        rxConfirm=false; fiConfP=true;
        if(!fiConfL){ fiSend(FM_CONFIRM,fiPeer); fiConfL=true; }
    }
    if(fiConfL&&fiConfP&&!fiLinked){
        fiLinked=true; fiLastPeerSeen=now; fiLastStatus=now;
        if(fiIsHost){
            fiStartSeed=(uint32_t)esp_random()|1;
            fiSend(FM_START,fiPeer);
            fiClearRx();
            fiNewSession(true,fiStartSeed);
        }
    }
    if(fiLinked&&!fiIsHost&&rxStart){
        rxStart=false; fiClearRx();
        fiNewSession(true,rxSeed);
    }
    if(!fiLinked&&now-fiSearchT0>FI_SEARCH_MS){ fiRadioDown(false); scr=SC_MENU; }
    if(lTap||lHoldFire){ fiRadioDown(true); scr=SC_MENU; }
}

void mantisfishTick(){
    fiInput();
    switch(scr){
    case SC_MENU:
        if(lTap) menuSel=(menuSel+1)%3;
        if(rTap){
            if(menuSel==0) fiNewSession(false,(uint32_t)esp_random()|1);
            else if(menuSel==1){
                fiClearRx(); rxStartSeqSeen=0x1FF;
                fiEspReady=fiRadioUp();
                if(fiEspReady){ fiSearchT0=millis(); fiLastHello=0; scr=SC_LOBBY; }
            } else { tackleSel=0; scr=SC_TACKLE; }
        }
        break;
    case SC_LOBBY: fiTickLobby(); break;
    case SC_FISH: fiTickFishing(); break;
    case SC_CARD:
        cardT++;
        if(cardT>24&&(lTap||rTap)) scr=SC_FISH;
        break;
    case SC_TACKLE:
        if(lTap) tackleSel^=1;
        if(rTap||lHoldFire) scr=SC_MENU;
        break;
    case SC_DERBYEND:
        if(lTap||rTap) scr=SC_MENU;
        break;
    case SC_LINKLOST:
        if(lTap||rTap) scr=SC_MENU;
        break;
    }
}

void mantisfishEnter(){
    fb.setRotation(0);
    fiLoad();
    scr=SC_MENU; menuSel=0; ph=PH_READY;
    enterAt=millis(); frameNo=0;
    rng=millis()|1;
    lDown=rDown=false; lLongDone=rLongDone=false;
    fiEspReady=false; fiPeerKnown=fiLinked=false; fiConfL=fiConfP=false;
    fiClearRx(); rxStartSeqSeen=0x1FF; fiSeq=0;
    derby=false; achQueueN=0; splashT=0;
    fiDirty=false;
    pgRegisterFlush(fiFlush);   // flushed by pgFlushAll() on sleep (manual hold or low-voltage cutoff)
}
void mantisfishShutdown(){
    fiRadioDown(true);
    fiFlush();
}
