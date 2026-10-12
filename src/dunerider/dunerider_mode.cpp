// FILE: src/dunerider/dunerider_mode.cpp
// DUNE RIDER - float-and-fly dune jumping (the Line Rider / Tiny Wings school).
//
// The draft died of a coordinate-system split (segments shifted into screen
// space while lookups stayed in world space, terrain double-scrolled off the
// planet, and the spawn loop grew without bound). This rewrite makes that
// class of bug IMPOSSIBLE: the terrain is a CLOSED-FORM function of world x -
// a seeded sum of sines whose amplitude swells with distance. No segment
// buffer, no spawn loop, no drift. Height and slope are exact everywhere.
//
// THE FEEL: gravity does the driving; you manage SPEED and ROTATION.
//   * On the sand, R = tuck (press into the downslopes to build speed),
//     L = brake.
//   * In the air, R = dive / rotate forward, L = flare / rotate back.
//   * THE LANDING IS THE SKILL: touch down aligned with the slope and you
//     keep everything; land crooked and you scrub speed; land past 50 deg
//     off (or on your lid) and you're tumbling. Full rotations in the air
//     pay style and a burst of boost.
//   * Overspeed a crest and the dune throws you - that's the fun AND the
//     danger. Fly too far off line and physics collects the debt.
//
// LEVELS: one seed = one level. 3 crashes ends it - the next ride is a NEW
// random level, forever, and the old one is gone... UNLESS it hosted the
// all-time best distance: that seed is saved, and the menu offers REPLAY
// BEST LEVEL any time (per spec). Beat the record elsewhere and the crown
// moves.
//
// VERSUS: 2.5-minute same-seed distance race over ESP-NOW (house convention:
// HELLO/CONFIRM/roles by id/copy-only callback/seq-dedup/BYE teardown/15 s
// timeout). Radio never comes up in solo.
//
// ECONOMY: distance pays no XP per meter (pinball rule). Eight one-time DUNE
// MEDALS pay 15-80 XP; versus wins pay +30 XP / +8 pet. NVS "dunerider"
// writes at run end only + shutdown flush.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "dunerider_mode.h"
#include "dunerider_sprites.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;
static constexpr int   BUG_SX   = 46;        // buggy's fixed screen column
static constexpr float DR_GRAV   = 0.115f;   // air gravity
// Gravity's pull ALONG the slope. This was *18/*22 in the draft, which on a
// 59-degree face applied -2.17/frame against a 0.085 throttle - gravity simply
// swamped the gas pedal and the buggy crawled. Simulated: at 3.0 a full minute
// covers ~1500-2000m with gas vs ~200-1000m coasting, so the pedal matters.
static constexpr float GRAV_ALONG = 3.0f;
static constexpr float THROTTLE  = 0.100f;   // L held = gas
static constexpr float ROLL_DRAG = 0.0016f;  // v^2 drag; sets the natural top end
static constexpr float SPD_MAX   = 9.0f;     // with boost you can really move
static constexpr float JUMP_V    = 4.4f;     // R on the ground = hop
static constexpr float ROT_RATE  = 0.058f;   // rad/frame of air rotation
static constexpr int   BOOST_FRAMES = 105;   // how long a boost burns
static constexpr float BOOST_ACC = 0.16f;    // extra accel while boosting
static constexpr uint32_t TAP_MS = 200;      // L tap (vs held gas) = fire boost
static constexpr int   MAX_LIVES= 3;
static constexpr uint32_t DR_LONG_MS=420, DR_ENTER_GUARD=250;
static constexpr uint32_t DR_HELLO_MS=300, DR_SEARCH_MS=15000;
static constexpr uint32_t DR_STATUS_MS=1000, DR_PEER_LOST=8000;
static constexpr int   VS_FRAMES = 150*62;   // 2.5 min race

// ---- persistence ------------------------------------------------------------
static const char* NVS_DR="dunerider";
struct __attribute__((packed)) DrSave {
    uint16_t magic;      // 0x5244 'DR'
    uint8_t  version;
    uint8_t  ach;
    uint32_t bestDist;   // meters
    uint32_t bestSeed;   // the level that hosted it (replayable)
    uint16_t runs;
    uint16_t vsWins;
};
static DrSave drSv;
static bool drDirty=false;
static void drDefaults(){ memset(&drSv,0,sizeof(drSv)); drSv.magic=0x5244; drSv.version=1; }
static void drLoad(){
    drDefaults();
    Preferences p; if(!p.begin(NVS_DR,true)) return;
    DrSave t; size_t n=p.getBytes("blob",&t,sizeof(t)); p.end();
    if(n==sizeof(t)&&t.magic==0x5244&&t.version==1) drSv=t;
}
static void drFlush(){
    if(!drDirty) return;
    Preferences p; if(!p.begin(NVS_DR,false)) return;
    p.putBytes("blob",&drSv,sizeof(drSv)); p.end();
    drDirty=false;
}

// ---- medals -----------------------------------------------------------------
enum DrAch:uint8_t{ ACH_FIRSTAIR=0, ACH_SUPERMAN, ACH_BACKFLIP, ACH_DOUBLE,
                    ACH_SMOOTH, ACH_MASTER, ACH_HORIZON, ACH_DUSTED };
struct DrAchDef{ const char* name; const char* how; uint8_t xp; };
static const DrAchDef ACHS[8]={
    {"FIRST FLIGHT","Hold 1.5s of air",         15},
    {"SUPERMANTIS", "Hold 4s of air",           30},
    {"BACKFLIP",    "Land a full backflip",     20},
    {"DOUBLE TROUBLE","Land a double flip",     40},
    {"SMOOTH OPERATOR","10 clean landings, 1 run",30},
    {"DUNE MASTER", "Ride 2000m in one run",    50},
    {"THE HORIZON", "Ride 5000m in one run",    80},
    {"DUSTED",      "Win a versus race",        50},
};
static uint8_t achQueue[4]; static int achQueueN=0;
static void drAward(uint8_t a){
    if((drSv.ach>>a)&1) return;
    drSv.ach|=(uint8_t)(1<<a);
    qpAddXp(ACHS[a].xp);
    drDirty=true;
    if(achQueueN<4) achQueue[achQueueN++]=a;
}

// ---- terrain: closed-form, seeded, amplitude swells with distance -----------
static uint32_t seed=1;
static float tA[4], tF[4], tP[4];
static void terrainInit(uint32_t s){
    seed=s;
    uint32_t r=s|1;
    for(int i=0;i<4;i++){
        r=r*1664525u+1013904223u; tP[i]=(r>>8&0xFF)*0.0246f;
        r=r*1664525u+1013904223u; tF[i]=0.006f+(i*0.006f)+((r>>8&0x1F)*0.0002f);
        tA[i]= (i==0?26.f: i==1?14.f: i==2?7.f:3.f);
    }
}
// Terrain is still closed-form (exact height AND slope everywhere, no buffers,
// no drift) but it is no longer a gentle ripple: the big roller carries a
// sharpened crest term, so every few hundred metres there is a proper lip that
// throws you instead of easing you over.
static inline float dunePhase(float x){ return tF[0]*x+tP[0]; }
static inline float groundY(float x){
    float amp = 1.0f + fminf(x/5000.0f, 2.4f);          // grows faster, gets bigger
    float y=230;
    for(int i=0;i<4;i++) y -= tA[i]*amp*sinf(tF[i]*x+tP[i]);
    // sharpened crest: adds a steep launch face on the back of the big roller
    float c = sinf(dunePhase(x));
    y -= 26.0f*amp*c*fabsf(c);
    return y;
}
static inline float groundSlope(float x){               // dy/dx, exact derivative
    float amp = 1.0f + fminf(x/5000.0f, 2.4f);
    float d=0;
    for(int i=0;i<4;i++) d -= tA[i]*amp*tF[i]*cosf(tF[i]*x+tP[i]);
    float c = sinf(dunePhase(x));
    d -= 26.0f*amp*2.0f*fabsf(c)*cosf(dunePhase(x))*tF[0];
    return d;
}

// ---- protocol ---------------------------------------------------------------
enum DrMsgType:uint8_t{ DM_HELLO=1, DM_CONFIRM, DM_START, DM_STATUS, DM_BYE };
struct __attribute__((packed)) DrMsg {
    uint16_t magic; uint8_t type,seq;
    uint32_t id, seed;
    uint32_t dist;                                       // meters
    uint8_t crashes, finished;
};
static constexpr uint16_t DR_MAGIC=0x5244;
static const uint8_t DR_BCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static volatile bool rxHello,rxConfirm,rxStart,rxStatus,rxBye;
static uint8_t rxMac[6]; static uint32_t rxId,rxSeed,rxDist;
static uint8_t rxCrashes,rxFinished;
static uint16_t rxStartSeqSeen=0x1FF;
static uint32_t drMyId=0; static uint8_t drPeer[6];
static bool drPeerKnown=false,drLinked=false,drEspReady=false,drConfL=false,drConfP=false;
static uint32_t drLastHello=0,drLastStatus=0,drLastPeerSeen=0,drSearchT0=0;
static uint8_t drSeq=0; static bool drIsHost=false;

static bool vsMode=false,vsDone=false,vsWon=false,rvDone=false,rvGone=false;
static int vsFrames=0; static uint32_t rvDist=0;
static uint32_t distM();

static void drOnRecv(const uint8_t* mac,const uint8_t* data,int len){
    if((size_t)len<sizeof(DrMsg)) return;
    const DrMsg* m=(const DrMsg*)data;
    if(m->magic!=DR_MAGIC||m->id==drMyId) return;
    memcpy((void*)rxMac,mac,6); rxId=m->id;
    switch(m->type){
        case DM_HELLO: rxHello=true; break;
        case DM_CONFIRM: rxConfirm=true; break;
        case DM_START:
            if((uint16_t)m->seq!=rxStartSeqSeen){ rxStartSeqSeen=m->seq; rxSeed=m->seed; rxStart=true; }
            break;
        case DM_STATUS: rxDist=m->dist; rxCrashes=m->crashes; rxFinished=m->finished; rxStatus=true; break;
        case DM_BYE: rxBye=true; break;
    }
}
static void drClearRx(){ rxHello=rxConfirm=rxStart=rxStatus=rxBye=false; }
static void drSendMsg(DrMsgType t,const uint8_t* to,uint32_t sd=0){
    DrMsg m; memset(&m,0,sizeof(m));
    m.magic=DR_MAGIC; m.type=t; m.id=drMyId; m.seq=++drSeq;
    if(t==DM_START) m.seed=sd;
    if(t==DM_STATUS){ m.dist=distM(); m.finished=vsDone?1:0; }
    esp_now_send(to,(uint8_t*)&m,sizeof(m));
    if(t==DM_START){ delay(4); esp_now_send(to,(uint8_t*)&m,sizeof(m)); }
}
static bool drAddPeer(const uint8_t* mac){
    esp_now_peer_info_t p; memset(&p,0,sizeof(p));
    memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
    if(esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p)==ESP_OK;
}
static bool drRadioUp(){
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    drMyId=((uint32_t)mac[3]<<16)|((uint32_t)mac[4]<<8)|mac[5];
    if(esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(drOnRecv));
    drAddPeer(DR_BCAST);
    return true;
}
static void drRadioDown(bool bye){
    if(drEspReady){
        if(bye&&drPeerKnown) drSendMsg(DM_BYE,drPeer);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        drEspReady=false;
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    drPeerKnown=drLinked=false; drConfL=drConfP=false;
}

// ---- run state --------------------------------------------------------------
enum DrScreen:uint8_t{ SC_MENU=0, SC_LOBBY, SC_RIDE, SC_RESULTS, SC_ACH, SC_LINKLOST };
static DrScreen scr=SC_MENU;
static int menuSel=0;
static uint32_t enterAt=0, frameNo=0;

static float bx, by;                          // buggy world pos (wheel contact)
static float vx, vy;
static float ang;                             // body angle rad (0 = level, + = nose down)
static float rotAccum;                        // net rotation since takeoff (flips)
static bool  onGround;
static int   airT;
static int   lives, crashT;
static float boost;            // 0..100 meter: fills with air time, flips, landings
static int   boostT;           // frames of boost remaining
static uint32_t lDownAtDR;     // for tap-vs-hold on L
static bool  lWasDown;
static int   cleanLandings;
static bool  newBest;
static uint32_t runBestNote=0;
static int   resT=0, countdownT=0;
static bool  replayingBest=false;

struct DrPart{ float x,y,vx,vy; uint8_t life; };
static DrPart parts[20];
static int shakeT=0;
static void fx(float x,float y,float vx_,float vy_,uint8_t l){
    for(int i=0;i<20;i++) if(!parts[i].life){ parts[i]={x,y,vx_,vy_,l}; return; }
}
static void fxTick(){
    for(int i=0;i<20;i++){
        if(!parts[i].life) continue;
        parts[i].vy+=0.05f; parts[i].x+=parts[i].vx; parts[i].y+=parts[i].vy; parts[i].life--;
    }
    if(shakeT) shakeT--;
}
struct DrPop{ char txt[14]; int x,y; uint8_t t; };
static DrPop pops[3];
static void popup(const char* s,int x,int y){
    int slot=0; uint8_t lo=255;
    for(int i=0;i<3;i++) if(pops[i].t<lo){lo=pops[i].t;slot=i;}
    snprintf(pops[slot].txt,sizeof(pops[slot].txt),"%s",s);
    pops[slot].x=x; pops[slot].y=y; pops[slot].t=26;
}

static uint32_t distM(){ return (uint32_t)(bx/10); }

static bool lTap,rTap,lHoldFire,rHoldFire;
static bool lDown,rDown,lLongDone,rLongDone;
static uint32_t lDownAt,rDownAt;
static void drInput(){
    lTap=rTap=lHoldFire=rHoldFire=false;
    uint32_t now=millis();
    if(btnL&&!btnL_prev){lDown=true;lDownAt=now;lLongDone=false;}
    if(btnL&&lDown&&!lLongDone&&now-lDownAt>DR_LONG_MS){lLongDone=true;lHoldFire=true;}
    if(!btnL&&btnL_prev){if(lDown&&!lLongDone)lTap=true;lDown=false;}
    if(btnR&&!btnR_prev){rDown=true;rDownAt=now;rLongDone=false;}
    if(btnR&&rDown&&!rLongDone&&now-rDownAt>DR_LONG_MS){rLongDone=true;rHoldFire=true;}
    if(!btnR&&btnR_prev){if(rDown&&!rLongDone)rTap=true;rDown=false;}
    if(now-enterAt<DR_ENTER_GUARD){lTap=rTap=lHoldFire=rHoldFire=false;}
}

static void drNewRun(uint32_t s, bool versus, bool replay){
    terrainInit(s);
    replayingBest=replay;
    bx=40; by=groundY(bx); vx=1.8f; vy=0;
    ang=atanf(groundSlope(bx)); rotAccum=0;
    onGround=true; airT=0;
    lives=MAX_LIVES; crashT=0;
    boost=0; boostT=0; lWasDown=false; cleanLandings=0;
    newBest=false; runBestNote=0;
    achQueueN=0; frameNo=0;
    for(int i=0;i<20;i++) parts[i].life=0;
    for(int i=0;i<3;i++) pops[i].t=0;
    shakeT=0;
    vsMode=versus; vsDone=false; vsWon=false; rvDone=false; rvGone=false; rvDist=0;
    vsFrames=VS_FRAMES;
    countdownT=versus?186:26;
    scr=SC_RIDE;
}

static void drFinishRun(){
    drSv.runs++;
    uint32_t d=distM();
    if(d>drSv.bestDist){
        drSv.bestDist=d;
        drSv.bestSeed=seed;                    // this level hosts the crown now
        newBest=true;
    }
    if(d>=2000) drAward(ACH_MASTER);
    if(d>=5000) drAward(ACH_HORIZON);
    drDirty=true;
    if(vsMode){
        vsDone=true;
        if(drLinked){ drSendMsg(DM_STATUS,drPeer); drLastStatus=millis(); }
    } else {
        drFlush();
        resT=0; scr=SC_RESULTS;
    }
}
static void drSettleVersus(){
    if(!rvGone){
        vsWon = distM()>rvDist;
        if(vsWon){ drSv.vsWins++; qpAddXp(30); qpGrantPetXp(8); drAward(ACH_DUSTED); drDirty=true; }
    } else vsWon=false;
    drFlush();
    if(drEspReady) drRadioDown(true);
    resT=0; scr=SC_RESULTS;
}

static void drCrash(){
    lives--;
    crashT=52;
    shakeT=10;
    boostT=0;
    for(int i=0;i<10;i++)
        fx(bx,by-6,(i%2?1.1f:-0.9f)*(1+(i%3)*0.4f),-1.4f-(i%4)*0.4f,22+(i%8));
    popup("CRASH!",BUG_SX,120);
}
static void drRespawn(){
    by=groundY(bx); vy=0; vx=1.4f;
    ang=atanf(groundSlope(bx));
    onGround=true; rotAccum=0; airT=0;
}

static void drTickNet(){
    if(!vsMode||!drEspReady) return;
    uint32_t now=millis();
    if(rxBye){ rxBye=false; drRadioDown(false); rvGone=true; }
    if(rxStatus){ rxStatus=false; drLastPeerSeen=now; rvDist=rxDist; rvDone=rxFinished!=0; }
    if(drLinked&&now-drLastStatus>DR_STATUS_MS){ drSendMsg(DM_STATUS,drPeer); drLastStatus=now; }
    if(drLinked&&now-drLastPeerSeen>DR_PEER_LOST){ drRadioDown(false); rvGone=true; }
}

static void drRideTick(){
    frameNo++;
    fxTick();
    for(int i=0;i<3;i++) if(pops[i].t) pops[i].t--;
    drTickNet();
    if(countdownT>0){ countdownT--; return; }

    if(vsMode&&!vsDone){ if(--vsFrames<=0) drFinishRun(); }
    if(vsMode&&vsDone){
        if(rvDone||rvGone||!drLinked) drSettleVersus();
        return;
    }

    if(crashT>0){
        crashT--;
        if(crashT==0){
            if(lives<=0){ drFinishRun(); return; }
            drRespawn();
        }
        return;
    }

    float slope=groundSlope(bx);
    float gAng=atanf(slope);

    // ---- L is the gas pedal; a TAP of L fires a full boost -----------------
    bool lTapFire=false;
    if(btnL && !lWasDown){ lWasDown=true; lDownAtDR=millis(); }
    if(!btnL && lWasDown){
        lWasDown=false;
        if(millis()-lDownAtDR < TAP_MS) lTapFire=true;
    }
    if(lTapFire && boost>=100.0f && boostT==0){
        boostT=BOOST_FRAMES; boost=0;
        popup("BOOST!",BUG_SX,104);
        for(int i=0;i<6;i++) fx(bx-10,by-4,-2.2f-(i%3),0.4f-(i%3)*0.4f,16);
    }
    if(boostT>0) boostT--;

    if(onGround){
        // Gravity down the face + throttle. No brake: this is a dune buggy,
        // the only pedal is the loud one.
        float along = DR_GRAV*GRAV_ALONG*sinf(gAng);     // downhill pulls
        if(btnL) along += THROTTLE;                      // gas
        if(boostT>0) along += BOOST_ACC;
        vx += along;
        vx -= ROLL_DRAG*vx*vx;                           // v^2 drag = natural top end
        if(vx<0.5f) vx=0.5f;
        if(vx>SPD_MAX) vx=SPD_MAX;
        bx += vx;
        float gy=groundY(bx);
        float newSlope=groundSlope(bx);
        float ballistic = by + vx*slope;                 // where momentum carries us
        // R = JUMP. Pop off whatever face you're on; on a crest lip that plus
        // the terrain launch angle is what sends you into orbit.
        if(rTap && vx>0.4f){
            onGround=false;
            vy = vx*slope - JUMP_V;
            airT=0; rotAccum=0;
            fx(bx-4,by-1,-1.2f,-0.6f,14); fx(bx+4,by-1,1.0f,-0.7f,14);
        }
        // or leave the ground because the dune fell away faster than we drop
        else if(ballistic < gy-1.2f && vx>1.4f){
            onGround=false;
            vy = vx*slope;
            airT=0; rotAccum=0;
            by = ballistic;
        } else if(onGround){
            by = gy;
            ang += (atanf(newSlope)-ang)*0.35f;          // settle to the surface
        }
        if(vx>3.4f && (frameNo&3)==0) fx(bx-8,by-1,-1.0f,-0.5f,10);
        if(boostT>0 && (frameNo&1)==0) fx(bx-9,by-4,-2.6f,-0.2f,10);
    } else {
        // airborne: dive or flare; rotation is yours to manage
        vy += DR_GRAV;
        if(boostT>0) vx += BOOST_ACC*0.5f;               // boost still pushes in flight
        // In the air BOTH buttons are rotation: R spins the nose down/forward,
        // L brings it back up. Landing angle is everything.
        if(btnR){ ang += ROT_RATE; rotAccum += ROT_RATE; }
        if(btnL){ ang -= ROT_RATE; rotAccum -= ROT_RATE; }
        if(airT>0 && (airT%20)==0 && boost<100.0f) boost += 4.0f;   // hang time pays
        bx += vx;
        by += vy;
        airT++;
        if(airT==94)  drAward(ACH_FIRSTAIR);             // 1.5 s
        if(airT==248) drAward(ACH_SUPERMAN);             // 4 s
        float gy=groundY(bx);
        if(by>=gy){
            // touchdown: alignment vs the slope decides everything
            float la=atanf(groundSlope(bx));
            float diff=ang-la;
            while(diff> 3.14159f) diff-=6.28318f;
            while(diff<-3.14159f) diff+=6.28318f;
            float ad=fabsf(diff);
            int flips=(int)(fabsf(rotAccum)/6.0f);       // ~full rotations
            by=gy; onGround=true;
            if(ad<0.44f){                                // clean
                cleanLandings++;
                if(cleanLandings==10) drAward(ACH_SMOOTH);
                float smooth=1.0f-ad*0.5f;
                vx = fmaxf(vx, (vx+fabsf(vy)*0.55f)*smooth);
                if(vx>SPD_MAX) vx=SPD_MAX;
                ang=la;
                boost += 18.0f;                          // clean landing pays
                if(boost>100.0f) boost=100.0f;
                if(flips>=1){
                    boost += 30.0f*flips;
                    if(boost>100.0f) boost=100.0f;
                    popup(flips>=2?"DOUBLE FLIP!":(rotAccum<0?"BACKFLIP!":"FRONTFLIP!"),BUG_SX,116);
                    if(rotAccum<0) drAward(ACH_BACKFLIP);
                    if(flips>=2) drAward(ACH_DOUBLE);
                } else if(airT>50) popup("CLEAN!",BUG_SX,120);
                fx(bx-4,by-1,-0.8f,-0.7f,12); fx(bx+4,by-1,0.6f,-0.8f,12);
            } else if(ad<0.9f){                          // hard landing
                vx*=0.55f; vy=0; ang=la;
                popup("ROUGH",BUG_SX,120);
                shakeT=5;
                cleanLandings=0;
            } else {                                     // lid-first
                drCrash();
                return;
            }
            vy=0; rotAccum=0; airT=0;
        }
    }
    uint32_t d=distM();
    if(!vsMode && d>drSv.bestDist && runBestNote!=1){ runBestNote=1; popup("NEW BEST!",BUG_SX,104); }
}

// ---- draw -------------------------------------------------------------------
static void drBlitRot(const uint8_t* sp,int sw,int sh,int cx,int cy,float a){
    // nearest-neighbor rotated blit around the sprite center (tiny sprites,
    // trivial at 62 fps)
    float ca=cosf(a), sa=sinf(a);
    int rowB=(sw+1)/2;
    for(int py=0;py<sh;py++) for(int px=0;px<sw;px++){
        uint8_t b=sp[py*rowB+px/2];
        uint8_t v=(px&1)?(b>>4):(b&0x0F);
        if(!v||v>DU_PAL_N) continue;
        float dx=px-sw*0.5f, dy=py-sh*0.5f;
        int x=cx+(int)lroundf(dx*ca-dy*sa);
        int y=cy+(int)lroundf(dx*sa+dy*ca);
        // paint 2 px so nearest-neighbor rotation leaves no pinholes
        fb.drawPixel(x,y,DU_PAL[v-1]);
        fb.drawPixel(x+1,y,DU_PAL[v-1]);
    }
}
static void drBlit(const uint8_t* sp,int sw,int sh,int x,int y,int scale=1){
    int rowB=(sw+1)/2;
    for(int py=0;py<sh;py++) for(int px=0;px<sw;px++){
        uint8_t b=sp[py*rowB+px/2];
        uint8_t v=(px&1)?(b>>4):(b&0x0F);
        if(!v||v>DU_PAL_N) continue;
        if(scale==1) fb.drawPixel(x+px,y+py,DU_PAL[v-1]);
        else fb.fillRect(x+px*scale,y+py*scale,scale,scale,DU_PAL[v-1]);
    }
}
static void drFooter(const char* l,const char* r){
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString(l,4,H-7,1);
    fb.setTextDatum(MR_DATUM); fb.drawString(r,W-4,H-7,1);
}
static void drawRide(){
    int ox=0,oy=0;
    if(shakeT){ ox=(int)((frameNo*2654435761u>>9)&3)-1; oy=(int)((frameNo*2654435761u>>11)&3)-1; }
    fb.fillSprite(TEAL_DEEP);
    // sun + heat bands
    fb.fillCircle(30,34,9,LIME_PALE);
    fb.drawCircle(30,34,12,LIME_DARK);
    // camera: buggy at BUG_SX; vertical follows softly
    static float camY=140;
    camY += ((by-150)-camY)*0.08f;
    float camX=bx-BUG_SX;
    // far dune silhouette (parallax, same function sampled slower)
    for(int sx=0;sx<W;sx+=2){
        float wx=camX*0.4f+sx*2.2f;
        int y=(int)(groundY(wx)*0.4f+56);
        fb.drawPixel(sx,y+oy,TEAL_DARK);
    }
    // main terrain: filled columns
    for(int sx=0;sx<W;sx++){
        float wx=camX+sx;
        int gy=(int)(groundY(wx)-camY);
        if(gy<0) gy=0;
        if(gy<H-14){
            fb.fillRect(sx+ox,gy+oy,1,H-14-gy,TEAL_DARK);
            fb.drawPixel(sx+ox,gy+oy,PURPLE_LIGHT);      // sunlit crest line
            fb.drawPixel(sx+ox,gy+1+oy,TEAL);
        }
    }
    // decor: cacti/rocks at deterministic world spots
    for(int k=-1;k<3;k++){
        uint32_t cell=(uint32_t)((camX)/160)+k;
        uint32_t h=cell*2654435761u^seed;
        int lx=(int)(cell*160+(h>>8&127));
        int sx=(int)(lx-camX);
        if(sx<-12||sx>W+4) continue;
        int gy=(int)(groundY((float)lx)-camY);
        if((h>>4&3)==0) drBlit(DU_CACTUS,DU_CACTUS_W,DU_CACTUS_H,sx+ox,gy-13+oy);
        else if((h>>4&3)==1) drBlit(DU_ROCK,DU_ROCK_W,DU_ROCK_H,sx+ox,gy-6+oy);
    }
    // buggy
    int bsy=(int)(by-camY);
    if(crashT>0){
        drBlit((crashT/6)&1?DU_CRASH2:DU_CRASH1,DU_CRASH1_W,DU_CRASH1_H,BUG_SX-12+ox,bsy-14+oy);
    } else {
        drBlitRot(DU_BUGGY,DU_BUGGY_W,DU_BUGGY_H,BUG_SX+ox,bsy-7+oy,ang);
        if(!onGround&&btnR){ fb.drawLine(BUG_SX-10+ox,bsy-16+oy,BUG_SX-4+ox,bsy-10+oy,TEAL_LIGHT); }
    }
    // particles
    for(int i=0;i<20;i++)
        if(parts[i].life) fb.drawPixel((int)(parts[i].x-camX)+ox,(int)(parts[i].y-camY)+oy,
                                       parts[i].life>10?LIME_PALE:PURPLE_LIGHT);
    // popups
    for(int i=0;i<3;i++){
        if(!pops[i].t) continue;
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        fb.drawString(pops[i].txt,pops[i].x,pops[i].y-(26-pops[i].t)/2,2);
    }
    // HUD
    char b[24];
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"%um",(unsigned)distM());
    fb.drawString(b,3,3,2);
    fb.setTextColor(TEAL_DARK);
    snprintf(b,sizeof(b),"BEST %um",(unsigned)drSv.bestDist);
    fb.drawString(b,3,20,1);
    // speed bar
    int fw=(int)((vx/SPD_MAX)*44);
    if(fw<0)fw=0;
    fb.drawRect(62,4,46,5,TEAL);
    if(fw>0) fb.fillRect(63,5,fw,3,boostT>0?PURPLE_LIGHT:(vx>6.0f?LIME:TEAL_BRIGHT));
    fb.setTextColor(TEAL_DARK); fb.drawString("SPEED",62,11,1);
    // boost meter: full + lime = tap L to fire it
    int bw=(int)((boost/100.0f)*44);
    fb.drawRect(62,18,46,5,boost>=100.0f?LIME:TEAL);
    if(bw>0) fb.fillRect(63,19,bw,3,boost>=100.0f?LIME:PURPLE_LIGHT);
    fb.setTextColor(boost>=100.0f?LIME:TEAL_DARK);
    fb.drawString(boostT>0?"BOOSTING":(boost>=100.0f?"TAP L!":"BOOST"),62,25,1);
    // lives as wheels
    for(int i=0;i<MAX_LIVES;i++){
        uint16_t c=i<lives?LIME:TEAL_DARK;
        fb.drawCircle(W-10-i*10,8,3,c);
        fb.drawPixel(W-10-i*10,8,c);
    }
    if(replayingBest){ fb.setTextDatum(TR_DATUM); fb.setTextColor(PURPLE_LIGHT); fb.drawString("BEST LEVEL",W-3,16,1); }
    if(vsMode){
        fb.fillRect(0,26,W,12,PURPLE_DEEP);
        int secs=vsFrames/62;
        fb.setTextDatum(ML_DATUM); fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"RIVAL %um",(unsigned)rvDist);
        fb.drawString(b,3,32,1);
        fb.setTextDatum(MR_DATUM);
        if(rvGone) fb.drawString("RIVAL LOST",W-3,32,1);
        else { snprintf(b,sizeof(b),"%d:%02d",secs/60,secs%60); fb.drawString(b,W-3,32,1); }
    }
    if(countdownT>0){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
        if(vsMode){ char c[4]; snprintf(c,sizeof(c),"%d",countdownT/62+1); fb.drawString(c,W/2,140,6); }
        else if(countdownT>18) fb.drawString("RIDE!",W/2,140,4);
    }
    if(vsMode&&vsDone){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("WAITING FOR RIVAL...",W/2,150,1);
    }
    drFooter(onGround?"L: GAS (tap=boost)":"L: rotate back",
             onGround?"R: JUMP":"R: rotate forward");
}
static void drawMenu(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillCircle(30,40,9,LIME_PALE);
    for(int sx=0;sx<W;sx++){
        int y=104-(int)(sinf(sx*0.05f)*8+sinf(sx*0.11f+2)*4);
        fb.fillRect(sx,y,1,10,TEAL_DARK);
        fb.drawPixel(sx,y,PURPLE_LIGHT);
    }
    drBlit(DU_BUGGY,DU_BUGGY_W,DU_BUGGY_H,W/2-24,60,2);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("DUNE",W/2,22,4);
    fb.drawString("RIDER",W/2,44,4);
    static const char* ITEMS[4]={"RIDE NEW DUNES","REPLAY BEST LEVEL","VERSUS RACE","DUNE MEDALS"};
    char sub[4][28];
    snprintf(sub[0],28,"a level you'll never see twice");
    if(drSv.bestDist) snprintf(sub[1],28,"%um lives here",(unsigned)drSv.bestDist);
    else snprintf(sub[1],28,"set a record first");
    snprintf(sub[2],28,"2.5 min race over ESP-NOW");
    { int got=0; for(int i=0;i<8;i++) if((drSv.ach>>i)&1) got++;
      snprintf(sub[3],28,"%d/8 - %u wins - %u runs",got,(unsigned)drSv.vsWins,(unsigned)drSv.runs); }
    for(int i=0;i<4;i++){
        int y=126+i*46; bool sel=(i==menuSel);
        bool dim=(i==1&&drSv.bestDist==0);
        fb.fillRoundRect(10,y,W-20,40,5,sel?TEAL_DARK:TEAL_DEEP);
        fb.drawRoundRect(10,y,W-20,40,5,sel?LIME:(dim?TEAL_DARK:TEAL_DARK));
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(dim?TEAL_DARK:(sel?LIME_PALE:TEAL_BRIGHT));
        fb.drawString(ITEMS[i],W/2,y+13,2);
        fb.setTextColor(sel?TEAL_LIGHT:TEAL_DARK);
        fb.drawString(sub[i],W/2,y+29,1);
    }
    drFooter("L: next","R: select");
}
static void drawLobby(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("FINDING A RIVAL",W/2,60,2);
    float a=(millis()%2000)*0.00314f;
    fb.drawCircle(W/2,150,38,TEAL_DARK); fb.drawCircle(W/2,150,24,TEAL_DARK);
    fb.drawLine(W/2,150,W/2+(int)(cosf(a)*38),150+(int)(sinf(a)*38),LIME);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(drPeerKnown?"RIVAL FOUND - LINKING":"hold devices close",W/2,212,1);
    char t[12];
    snprintf(t,sizeof(t),"%us",(unsigned)((DR_SEARCH_MS-(millis()-drSearchT0))/1000));
    fb.drawString(t,W/2,230,1);
    drFooter("L: cancel","");
}
static void drawResults(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRoundRect(10,60,W-20,196,8,TEAL_DEEP);
    fb.drawRoundRect(10,60,W-20,196,8,(vsMode?vsWon:newBest)?LIME:LIME_DARK);
    fb.setTextDatum(MC_DATUM);
    char b[28];
    if(vsMode){
        fb.setTextColor(vsWon?LIME:PURPLE_LIGHT);
        fb.drawString(vsWon?"DUSTED 'EM!":(rvGone?"RIVAL LOST":"OUTRUN"),W/2,80,4);
        fb.setTextColor(LIME_PALE);
        snprintf(b,sizeof(b),"YOU   %um",(unsigned)distM());
        fb.drawString(b,W/2,108,2);
        fb.setTextColor(TEAL_BRIGHT);
        snprintf(b,sizeof(b),"RIVAL %um",(unsigned)rvDist);
        fb.drawString(b,W/2,126,2);
        if(vsWon){ fb.setTextColor(TEAL_LIGHT); fb.drawString("+30 XP",W/2,144,1); }
    } else {
        fb.setTextColor(LIME_PALE);
        snprintf(b,sizeof(b),"%um",(unsigned)distM());
        fb.drawString(b,W/2,92,6);
        if(newBest){
            fb.setTextColor(LIME);
            fb.drawString("NEW BEST LEVEL!",W/2,124,2);
            fb.setTextColor(TEAL_LIGHT);
            fb.drawString("this level is saved to replay",W/2,140,1);
        } else {
            fb.setTextColor(TEAL_LIGHT);
            snprintf(b,sizeof(b),"best %um",(unsigned)drSv.bestDist);
            fb.drawString(b,W/2,128,1);
            fb.setTextColor(TEAL_DARK);
            fb.drawString("that level is gone forever",W/2,142,1);
        }
    }
    int y=vsMode?166:162;
    for(int i=0;i<achQueueN&&y<244;i++,y+=13){
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"MEDAL: %s +%dXP",ACHS[achQueue[i]].name,ACHS[achQueue[i]].xp);
        fb.drawString(b,W/2,y,1);
    }
    drFooter("L: menu",vsMode?"R: menu":"R: new dunes");
}
static void drawAch(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("DUNE MEDALS",4,9,1);
    char b[12];
    int got=0; for(int i=0;i<8;i++) if((drSv.ach>>i)&1) got++;
    fb.setTextDatum(MR_DATUM);
    snprintf(b,sizeof(b),"%d/8",got);
    fb.drawString(b,W-4,9,1);
    for(int i=0;i<8;i++){
        int y=24+i*35;
        bool have=(drSv.ach>>i)&1;
        fb.fillRoundRect(4,y,W-8,32,4,TEAL_DEEP);
        fb.drawRoundRect(4,y,W-8,32,4,have?LIME_DARK:TEAL_DARK);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(have?LIME:TEAL_LIGHT);
        fb.drawString(ACHS[i].name,10,y+9,1);
        fb.setTextColor(have?LIME_PALE:TEAL_DARK);
        fb.drawString(ACHS[i].how,10,y+21,1);
        if(have){
            fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME);
            snprintf(b,sizeof(b),"+%d",ACHS[i].xp);
            fb.drawString(b,W-10,y+15,1);
        }
    }
    drFooter("","L/R: back");
}

void duneriderDraw(){
    switch(scr){
    case SC_MENU: drawMenu(); break;
    case SC_LOBBY: drawLobby(); break;
    case SC_RIDE: drawRide(); break;
    case SC_RESULTS: drawResults(); break;
    case SC_ACH: drawAch(); break;
    case SC_LINKLOST:
        fb.fillSprite(TEAL_DEEP);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("LINK LOST",W/2,130,4);
        drFooter("","L/R: menu");
        break;
    }
    fb.pushSprite(0,0);
}

// ---- lobby + top-level ------------------------------------------------------
static void drTickLobby(){
    uint32_t now=millis();
    if(now-drLastHello>DR_HELLO_MS){ drSendMsg(DM_HELLO,DR_BCAST); drLastHello=now; }
    if(rxHello&&!drPeerKnown){
        rxHello=false;
        memcpy(drPeer,(const void*)rxMac,6);
        drAddPeer(drPeer);
        drPeerKnown=true;
        drIsHost=(drMyId<rxId);
        drSendMsg(DM_CONFIRM,drPeer);
        drConfL=true;
    }
    if(rxConfirm&&drPeerKnown){
        rxConfirm=false; drConfP=true;
        if(!drConfL){ drSendMsg(DM_CONFIRM,drPeer); drConfL=true; }
    }
    if(drConfL&&drConfP&&!drLinked){
        drLinked=true; drLastPeerSeen=now; drLastStatus=now;
        if(drIsHost){
            uint32_t sd=(uint32_t)esp_random()|1;
            drSendMsg(DM_START,drPeer,sd);
            drClearRx();
            drNewRun(sd,true,false);
        }
    }
    if(drLinked&&!drIsHost&&rxStart){
        rxStart=false; drClearRx();
        drNewRun(rxSeed,true,false);
    }
    if(!drLinked&&now-drSearchT0>DR_SEARCH_MS){ drRadioDown(false); scr=SC_MENU; }
    if(lTap||lHoldFire){ drRadioDown(true); scr=SC_MENU; }
}

void duneriderTick(){
    drInput();
    switch(scr){
    case SC_MENU:
        if(lTap) menuSel=(menuSel+1)%4;
        if(rTap){
            if(menuSel==0) drNewRun((uint32_t)esp_random()|1,false,false);
            else if(menuSel==1){ if(drSv.bestDist) drNewRun(drSv.bestSeed,false,true); }
            else if(menuSel==2){
                drClearRx(); rxStartSeqSeen=0x1FF;
                drEspReady=drRadioUp();
                if(drEspReady){ drSearchT0=millis(); drLastHello=0; scr=SC_LOBBY; }
            } else scr=SC_ACH;
        }
        break;
    case SC_LOBBY: drTickLobby(); break;
    case SC_RIDE: drRideTick(); break;
    case SC_RESULTS:
        resT++;
        if(resT>30&&rTap&&!vsMode) drNewRun((uint32_t)esp_random()|1,false,false);
        if(resT>30&&(lTap||lHoldFire||(rTap&&vsMode))) scr=SC_MENU;
        break;
    case SC_ACH:
        if(lTap||rTap) scr=SC_MENU;
        break;
    case SC_LINKLOST:
        if(lTap||rTap) scr=SC_MENU;
        break;
    }
}

void duneriderEnter(){
    fb.setRotation(0);
    drLoad();
    scr=SC_MENU; menuSel=0;
    enterAt=millis(); frameNo=0;
    lDown=rDown=false; lLongDone=rLongDone=false;
    drEspReady=false; drPeerKnown=drLinked=false; drConfL=drConfP=false;
    drClearRx(); rxStartSeqSeen=0x1FF; drSeq=0;
    vsMode=false; achQueueN=0;
    terrainInit(1);
    for(int i=0;i<20;i++) parts[i].life=0;
    for(int i=0;i<3;i++) pops[i].t=0;
}
void duneriderShutdown(){
    drRadioDown(true);
    drFlush();
}
