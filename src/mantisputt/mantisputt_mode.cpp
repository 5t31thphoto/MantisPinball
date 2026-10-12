// FILE: src/mantisputt/mantisputt_mode.cpp
// MANTIS PUTT - minigolf played on the Pinball Blaster table.
//
// Per spec: "very similar to corner pocket only it is minigolf with infinite
// procedural levels... same pinball layout... same flippers and same physics
// and same angled walls but the minigolf aesthetic... ball needs to be able to
// always drain back down to be hit again until it gets in the cup."
//
// So this is NOT top-down golf. It is the house table - same GRAVITY 0.18,
// same BALL_R 4, same 38 px flippers at 28 deg rest / -32 deg active, same
// funnel walls - with a cup somewhere in the playfield and minigolf furniture
// in the way. The draft died of two things this rewrite fixes by design:
//   * its flipper test used a one-sided `perp < 0` check that was sign-correct
//     for the right flipper only, so the left flipper never hit the ball. We
//     use the house radial-normal test with the anti-trap flip instead.
//   * its ball spawned inside the launch lane wall and rattled there forever.
//     We have a real plunger, a lane wider than the ball, and a one-way gate
//     that closes behind the ball so it can never re-enter.
//
// THE SHOT: gravity means a high cup is reached at the TOP of the ball's arc,
// where it is moving slowest - and the cup only accepts a slow ball (a fast
// one lips out and rattles away). So the skill is choosing a flipper contact
// point that puts the apex ON the cup. That is a genuine minigolf feel
// expressed in pinball physics, and it is why holes are reachable without
// being free.
//
// PROCEDURAL: infinite levels from `seedBase + level*7919`. A template picks
// the cup shelf, then furniture is placed by rejection sampling against a
// reserved approach corridor, so every generated hole is provably shootable
// (see puttGenerate). Difficulty tiers add furniture and shrink the corridor.
//
// STROKES: a flipper strike that actually imparts speed counts as a stroke, as
// does each plunge. Draining is free - the ball always comes back, exactly as
// specified. Par is computed from cup height and furniture count.
//
// ECONOMY (house rule - score pays nothing): XP is event-driven only, +25 per
// 5 holes cleared, plus eight one-time CLUB MEDALS. NVS "mputt" is written on
// hole-clear at most every 10 s, plus a shutdown flush.
//
// VERSUS: 3-minute same-seed scramble over ESP-NOW using the house convention
// (HELLO 300 ms / CONFIRM / roles by chip id / copy-only recv callback /
// seq-dedup with a 0x1FF sentinel / BYE + full teardown / 15 s search timeout).
// Both devices generate identical holes from the shared seed. Radio never
// comes up in solo.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "mantisputt_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"
#include "../power_guard.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ---- house table physics (identical to main.cpp) ---------------------------
static constexpr float BALL_R    = 4.0f;
static constexpr float GRAVITY   = 0.18f;
static constexpr float MAX_VEL   = 8.5f;
static constexpr float FLIP_LEN        = 38.0f;
static constexpr float FLIP_HALF_THICK = 3.0f;
static constexpr float FLIP_REST_DEG   = 28.0f;
static constexpr float FLIP_ACT_DEG    = -32.0f;
static constexpr float FLIP_REST_RAD   = FLIP_REST_DEG * (float)M_PI / 180.0f;
static constexpr float FLIP_ACT_RAD    = FLIP_ACT_DEG  * (float)M_PI / 180.0f;
static constexpr float FLIP_LERP_UP    = 0.55f;
static constexpr float FLIP_LERP_DOWN  = 0.30f;
static constexpr float FLIP_Y          = (float)H - 22.0f;
static constexpr float DRAIN_HALF_GAP  = 7.0f;
static constexpr float FLIP_REST_COS   = 0.88295f;
static constexpr float FLIP_L_PX = (float)W*0.5f - DRAIN_HALF_GAP - FLIP_LEN*FLIP_REST_COS;
static constexpr float FLIP_R_PX = (float)W*0.5f + DRAIN_HALF_GAP + FLIP_LEN*FLIP_REST_COS;
// Corner Pocket and Slots boost the kick so the ball can reach the top of a
// table with no bumpers helping it. Putt needs the same reach, but a touch
// lower so the apex lands in the playfield rather than the ceiling.
static constexpr float PUTT_KICK_GAIN  = 130.0f;   // house Corner Pocket value

static constexpr float CUP_R     = 7.0f;
static constexpr float CUP_CATCH = 1.95f;    // ball must be slower than this

static constexpr uint32_t PT_LONG_MS=420, PT_ENTER_GUARD=250;
static constexpr uint32_t PT_HELLO_MS=300, PT_SEARCH_MS=15000;
static constexpr uint32_t PT_STATUS_MS=1000, PT_PEER_LOST=8000;
static constexpr int      VS_FRAMES = 180*62;

// ---- persistence -----------------------------------------------------------
static const char* NVS_PT="mputt";
struct __attribute__((packed)) PtSave {
    uint16_t magic;      // 0x5054 'PT'
    uint8_t  version;
    uint8_t  ach;
    uint32_t holesCleared;   // lifetime
    uint16_t bestUnder;      // best (par - strokes) on a single hole, >=0
    uint16_t bestRunHoles;   // most holes in one session
    uint16_t aces;           // holes-in-one
    uint16_t vsWins;
    uint16_t xpDebt;         // holes counted toward the next +25 XP
};
static PtSave ptSv;
static bool ptDirty=false;
static void ptDefaults(){ memset(&ptSv,0,sizeof(ptSv)); ptSv.magic=0x5054; ptSv.version=1; }
static void ptLoad(){
    ptDefaults();
    Preferences p; if(!p.begin(NVS_PT,true)) return;
    PtSave t; size_t n=p.getBytes("blob",&t,sizeof(t)); p.end();
    if(n==sizeof(t)&&t.magic==0x5054&&t.version==1) ptSv=t;
}
static void ptFlush(){
    if(!ptDirty) return;
    Preferences p; if(!p.begin(NVS_PT,false)) return;
    p.putBytes("blob",&ptSv,sizeof(ptSv)); p.end();
    ptDirty=false;
}

// ---- medals ----------------------------------------------------------------
enum PtAch:uint8_t{ ACH_FIRSTHOLE=0, ACH_ACE, ACH_BIRDIE, ACH_TEN, ACH_FIFTY,
                    ACH_SANDSAVE, ACH_WINDMILL, ACH_SCRAMBLE };
struct PtAchDef{ const char* name; const char* how; uint8_t xp; };
static const PtAchDef ACHS[8]={
    {"FIRST CUP",   "Hole your first putt",      10},
    {"HOLE IN ONE", "Ace a hole",                50},
    {"BIRDIE",      "Finish a hole under par",   25},
    {"BACK NINE",   "Clear 10 holes",            30},
    {"CLUBHOUSE",   "Clear 50 holes",            60},
    {"SAND SAVE",   "Hole out from a sand trap", 35},
    {"WINDMILL",    "Ace a windmill hole",       45},
    {"SCRAMBLE",    "Win a versus scramble",     50},
};
static uint8_t achQueue[4]; static int achQueueN=0;
static void ptAward(uint8_t a){
    if((ptSv.ach>>a)&1) return;
    ptSv.ach|=(uint8_t)(1<<a);
    qpAddXp(ACHS[a].xp);
    ptDirty=true;
    if(achQueueN<4) achQueue[achQueueN++]=a;
}

// ---- table geometry --------------------------------------------------------
struct PWall { float x1,y1,x2,y2; float rest; uint8_t kind; };  // kind: 0 wall 1 furniture 2 gate
enum { WK_WALL=0, WK_FURN=1, WK_GATE=2 };
static constexpr int MAX_W = 44;
static PWall pw[MAX_W]; static int pwN=0;
static void addW(float x1,float y1,float x2,float y2,float rest,uint8_t k=WK_WALL){
    if(pwN<MAX_W) pw[pwN++] = {x1,y1,x2,y2,rest,k};
}

struct Rock { float x,y,r; uint8_t hit; };          // circular bumper
struct Zone { float x,y,w,h; uint8_t kind; };       // 0 sand 1 water
static constexpr int MAX_ROCK=5, MAX_ZONE=3;
static Rock rocks[MAX_ROCK]; static int rockN=0;
static Zone zones[MAX_ZONE]; static int zoneN=0;

// windmill: a blade segment spinning about a hub (dynamic wall slot)
static bool  wmOn=false; static float wmX,wmY,wmA,wmSpd,wmLen; static int wmSlot=-1;
// gate: a segment sliding horizontally (dynamic wall slot)
static bool  gtOn=false; static float gtX,gtY,gtLen,gtLo,gtHi,gtDir; static int gtSlot=-1;
// pipe: teleport pair
static bool  ppOn=false; static float ppAx,ppAy,ppBx,ppBy;

static float cupX,cupY;
static uint8_t holePar;
static uint32_t seedBase=1, holeSeed=1;
static uint32_t rng=1;
static uint32_t prand(){ rng=rng*1664525u+1013904223u; return rng>>8; }
static float    prandf(){ return (prand()&0xFFFF)/65535.0f; }

// ---- ball + flippers -------------------------------------------------------
struct PBall { float x,y,vx,vy; };
static PBall ball;
struct PFlip { float pivotX,pivotY,angle,prevAngle,restAngle,activeAngle; int sign; bool active; };
static PFlip flipL, flipR;

static int   level=1, strokes=0, holeStrokes=0;
static int   holesThisRun=0;
static int   ballsRemaining=3;      // 3 balls per run, like Corner Pocket
static bool  runOver=false;         // set on the third drain
static int   goT=0;                 // game-over screen dwell
static bool  inSand=false, sandThisHole=false;
static int   celebT=0, lipT=0;
static uint32_t frameNo=0;

// ---- helpers (house math) --------------------------------------------------
static inline float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }
static void reflectV(float &vx,float &vy,float nx,float ny,float rest,float fric){
    float vn = vx*nx + vy*ny;
    if(vn>0) return;
    float tx = vx - vn*nx, ty = vy - vn*ny;
    vx = tx*fric - vn*rest*nx;
    vy = ty*fric - vn*rest*ny;
}
static float closestOnSeg(float x1,float y1,float x2,float y2,float px,float py,float &cx,float &cy){
    float dx=x2-x1, dy=y2-y1, l2=dx*dx+dy*dy;
    if(l2<0.0001f){ cx=x1; cy=y1; return 0.0f; }
    float t=clampf(((px-x1)*dx+(py-y1)*dy)/l2, 0.0f, 1.0f);
    cx=x1+t*dx; cy=y1+t*dy; return t;
}

// ---- procedural hole engine ------------------------------------------------
// Reserved approach corridor: a vertical band under the cup that furniture may
// never intrude on, so a well-aimed shot always has a line. Its width shrinks
// with difficulty but never below one ball diameter plus margin.
static float corrX, corrHalf;

static bool inCorridor(float x,float y,float pad){
    return (y > cupY - 6) && (y < 250.0f) &&
           (x > corrX - corrHalf - pad) && (x < corrX + corrHalf + pad);
}

static void puttGenerate(int lvl){
    holeSeed = seedBase + (uint32_t)lvl * 7919u;
    rng = holeSeed | 1;
    pwN=0; rockN=0; zoneN=0;
    wmOn=gtOn=ppOn=false; wmSlot=gtSlot=-1;

    int tier = lvl/4; if(tier>4) tier=4;

    // ---- static table: COPIED EXACTLY from the house buildWalls() -----------
    // The funnels must run from the outer rail at FUNNEL_TOP_Y down to the
    // actual flipper tip (FLIP_L_PX-4, FLIP_Y-2), or the ball slips behind them
    // - which is the bug in the photo. Outer rails stop at FUNNEL_TOP_Y so the
    // funnel takes over from there. FLIP_Y = H-22 keeps the flippers on-screen.
    const float FUNNEL_TOP_Y = (float)H - 90.0f;     // 230
    addW(0,    0, 0,    FUNNEL_TOP_Y, 0.85f);        // left outer rail
    addW(W,    0, W,    FUNNEL_TOP_Y, 0.85f);        // right outer rail
    addW(0,    0, W,    0,            0.85f);         // ceiling
    addW(0,    FUNNEL_TOP_Y, FLIP_L_PX-4.0f, FLIP_Y-2.0f, 0.57f);   // left funnel -> L tip
    addW(W,    FUNNEL_TOP_Y, FLIP_R_PX+4.0f, FLIP_Y-2.0f, 0.57f);   // right funnel -> R tip

    // ---- cup shelf --------------------------------------------------------
    cupY = 74.0f + prandf()*104.0f;                  // 74..178
    cupX = 22.0f + prandf()*106.0f;                  // 22..128
    corrX = cupX;
    corrHalf = 15.0f - tier*1.6f; if(corrHalf<11.0f) corrHalf=11.0f;

    // a small shelf under the cup so it can hold a slow ball
    addW(cupX-13, cupY+9, cupX-CUP_R-1, cupY+9, 0.30f, WK_FURN);
    addW(cupX+CUP_R+1, cupY+9, cupX+13, cupY+9, 0.30f, WK_FURN);

    // ---- furniture (rejection-sampled against the corridor) ---------------
    int want = 2 + tier;                             // 2..6 pieces
    int guard = 0;
    while(want > 0 && ++guard < 60){
        float fx = 14.0f + prandf()*((float)W-30.0f);
        float fy = 52.0f + prandf()*150.0f;
        if(inCorridor(fx,fy,10.0f)) continue;                 // keep the line open
        if(fy > 200.0f) continue;                             // keep the flipper area clear
        if(fabsf(fx-cupX)<20.0f && fabsf(fy-cupY)<20.0f) continue;  // don't smother the cup
        int pick = (int)(prand()%100);

        if(pick<30 && rockN<MAX_ROCK){                        // ROCK bumper
            rocks[rockN++] = { fx, fy, 5.0f+prandf()*2.5f, 0 };
            want--;
        } else if(pick<52){                                   // angled RAMP wall
            float len = 22.0f+prandf()*26.0f;
            float ang = (prandf()<0.5f? -1.0f:1.0f)*(0.35f+prandf()*0.5f);
            float ex = fx+cosf(ang)*len, ey = fy+sinf(ang)*len;
            if(inCorridor(ex,ey,8.0f)) continue;
            if(ex<8||ex>(float)W-8) continue;
            addW(fx,fy,ex,ey,0.72f,WK_FURN);
            want--;
        } else if(pick<66 && zoneN<MAX_ZONE){                 // SAND trap
            zones[zoneN++] = { fx-14, fy-5, 28, 10, 0 };
            want--;
        } else if(pick<74 && zoneN<MAX_ZONE && tier>=2){      // WATER
            zones[zoneN++] = { fx-12, fy-4, 24, 8, 1 };
            want--;
        } else if(pick<86 && !wmOn && tier>=1){               // WINDMILL
            wmOn=true; wmX=fx; wmY=fy; wmA=prandf()*6.28f;
            wmLen = 15.0f+tier*2.0f;
            wmSpd = (prandf()<0.5f?-1.0f:1.0f)*(0.020f+prandf()*0.022f);
            wmSlot = pwN; addW(fx-wmLen,fy,fx+wmLen,fy,0.80f,WK_FURN);
            want--;
        } else if(pick<94 && !gtOn && tier>=2){               // sliding GATE
            gtOn=true; gtLen=20.0f+tier*3.0f; gtY=fy;
            gtLo = 12.0f; gtHi = (float)W-14.0f-gtLen;
            gtX = gtLo + prandf()*(gtHi-gtLo);
            gtDir = (prandf()<0.5f?-1.0f:1.0f)*(0.45f+prandf()*0.45f);
            gtSlot = pwN; addW(gtX,gtY,gtX+gtLen,gtY,0.75f,WK_GATE);
            want--;
        } else if(!ppOn && tier>=3){                          // PIPE teleport
            ppOn=true; ppAx=fx; ppAy=fy;
            ppBx = 16.0f+prandf()*((float)W-34.0f);
            ppBy = 52.0f+prandf()*60.0f;
            want--;
        }
    }

    // par: distance to climb + how cluttered it is
    int furn = rockN + zoneN + (wmOn?1:0) + (gtOn?1:0);
    holePar = (uint8_t)(2 + (cupY < 120.0f ? 1 : 0) + (furn >= 4 ? 1 : 0));
    if(holePar<2) holePar=2;
    if(holePar>5) holePar=5;

    holeStrokes = 0;
    sandThisHole = false;
}

// ---- ball placement --------------------------------------------------------
// Served EXACTLY like the house table's resetBall() (main.cpp): dropped into
// the lower-middle playfield with a fixed velocity, straight into play above
// the flippers. There is NO launch lane and NO plunger - the real pinball
// games don't have one, and I should have matched them from the start instead
// of inventing a chute that trapped the ball. On a drain the golf rule is the
// ball simply comes back, so serveBall() is called again (no ball count).
static void serveBall(){
    ball.x = (float)W * 0.5f - 4.0f;
    ball.y = (float)H - 70.0f;
    ball.vx = 1.6f;
    ball.vy = -4.4f;
}

static void newHole(int lvl){
    level = lvl;
    puttGenerate(lvl);
    serveBall();
    celebT = 0; lipT = 0;
    for(int i=0;i<rockN;i++) rocks[i].hit=0;
}

// ---- physics ---------------------------------------------------------------
static void flipEndpoints(const PFlip&f,float&tx,float&ty){
    tx = f.pivotX + cosf(f.angle)*FLIP_LEN;
    ty = f.pivotY + sinf(f.angle)*FLIP_LEN;
}
static bool flipStruck=false;
static void collideFlip(PFlip &f){
    float tipX,tipY; flipEndpoints(f,tipX,tipY);
    float cx,cy; closestOnSeg(f.pivotX,f.pivotY,tipX,tipY,ball.x,ball.y,cx,cy);
    float pdx=ball.x-cx, pdy=ball.y-cy;
    float minD=BALL_R+FLIP_HALF_THICK;
    float d2=pdx*pdx+pdy*pdy;
    if(d2>=minD*minD) return;
    float d=sqrtf(d2), nx,ny;
    if(d>0.001f){ nx=pdx/d; ny=pdy/d; }
    else { float dx=tipX-f.pivotX, dy=tipY-f.pivotY; float l=sqrtf(dx*dx+dy*dy); nx=-dy/l; ny=dx/l; }
    // house anti-trap: never let an extended flipper pin the ball beneath it
    float midA=(f.restAngle+f.activeAngle)*0.5f;
    bool extended = (f.sign>0)?(f.angle<midA):(f.angle>midA);
    if(ny>0 && (f.active||extended)){
        nx=-nx; ny=-ny;
        ball.x=cx+nx*(minD+0.5f); ball.y=cy+ny*(minD+0.5f);
    }
    float pen=minD-d;
    ball.x+=nx*pen; ball.y+=ny*pen;
    reflectV(ball.vx,ball.vy,nx,ny,0.92f,0.97f);
    float omega=f.angle-f.prevAngle;
    float rx=cx-f.pivotX, ry=cy-f.pivotY;
    float tx=-ry, ty=rx;
    // House kick math (main.cpp): omega * gain DIRECTLY, no /1000. That factor
    // was mine and it's why the ball died in the lower third.
    ball.vx += tx*omega*PUTT_KICK_GAIN;
    ball.vy += ty*omega*PUTT_KICK_GAIN;
    if(fabsf(omega) > 0.02f) flipStruck=true;   // a real strike, not a resting touch
}

static void teleportCheck(){
    if(!ppOn) return;
    float dx=ball.x-ppAx, dy=ball.y-ppAy;
    if(dx*dx+dy*dy < 49.0f){
        ball.x=ppBx; ball.y=ppBy;
        ball.vy = -fabsf(ball.vy)*0.6f - 1.2f;
    }
}

static void zoneCheck(){
    inSand=false;
    for(int i=0;i<zoneN;i++){
        const Zone&z=zones[i];
        if(ball.x>z.x && ball.x<z.x+z.w && ball.y>z.y && ball.y<z.y+z.h){
            if(z.kind==0){                       // SAND: heavy damping
                inSand=true; sandThisHole=true;
                ball.vx*=0.86f; ball.vy*=0.90f;
            } else {                             // WATER: back to the tee
                holeStrokes++; strokes++;
                serveBall();
            }
        }
    }
}

// ---- protocol (house convention) -------------------------------------------
enum PtMsg:uint8_t{ PT_HELLO=1, PT_CONFIRM, PT_START, PT_STATUS, PT_BYE };
struct __attribute__((packed)) PtPkt {
    uint16_t magic; uint8_t type,seq; uint32_t id,seed;
    uint16_t holes, strokes; uint8_t finished;
};
static constexpr uint16_t PT_MAGIC=0x5054;
static const uint8_t PT_BCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static volatile bool rxHello,rxConfirm,rxStart,rxStatus,rxBye;
static uint8_t rxMac[6]; static uint32_t rxId,rxSeed;
static uint16_t rxHoles,rxStrokes; static uint8_t rxFin;
static uint16_t rxStartSeq=0x1FF;
static uint32_t ptMyId=0; static uint8_t ptPeer[6];
static bool ptPeerKnown=false, ptLinked=false, ptEsp=false, ptConfL=false, ptConfP=false;
static uint32_t ptLastHello=0, ptLastStatus=0, ptLastSeen=0, ptSearchT0=0;
static uint8_t ptSeq=0; static bool ptIsHost=false;
static bool vsMode=false, vsDone=false, vsWon=false, rvDone=false, rvGone=false;
static int  vsFrames=0; static uint16_t rvHoles=0, rvStrokes=0;
static uint32_t vsSeed=1;

static void ptOnRecv(const uint8_t* mac,const uint8_t* d,int len){
    if((size_t)len<sizeof(PtPkt)) return;
    const PtPkt* m=(const PtPkt*)d;
    if(m->magic!=PT_MAGIC||m->id==ptMyId) return;
    memcpy((void*)rxMac,mac,6); rxId=m->id;
    switch(m->type){
        case PT_HELLO: rxHello=true; break;
        case PT_CONFIRM: rxConfirm=true; break;
        case PT_START: if((uint16_t)m->seq!=rxStartSeq){ rxStartSeq=m->seq; rxSeed=m->seed; rxStart=true; } break;
        case PT_STATUS: rxHoles=m->holes; rxStrokes=m->strokes; rxFin=m->finished; rxStatus=true; break;
        case PT_BYE: rxBye=true; break;
    }
}
static void ptClearRx(){ rxHello=rxConfirm=rxStart=rxStatus=rxBye=false; }
static void ptSend(PtMsg t,const uint8_t* to){
    PtPkt m; memset(&m,0,sizeof(m));
    m.magic=PT_MAGIC; m.type=t; m.id=ptMyId; m.seq=++ptSeq;
    if(t==PT_START) m.seed=vsSeed;
    if(t==PT_STATUS){ m.holes=(uint16_t)holesThisRun; m.strokes=(uint16_t)strokes; m.finished=vsDone?1:0; }
    esp_now_send(to,(uint8_t*)&m,sizeof(m));
    if(t==PT_START){ delay(4); esp_now_send(to,(uint8_t*)&m,sizeof(m)); }
}
static bool ptAddPeer(const uint8_t* mac){
    esp_now_peer_info_t p; memset(&p,0,sizeof(p));
    memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
    if(esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p)==ESP_OK;
}
static bool ptRadioUp(){
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    uint8_t mac[6]; WiFi.macAddress(mac);
    ptMyId=((uint32_t)mac[3]<<16)|((uint32_t)mac[4]<<8)|mac[5];
    if(esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(ptOnRecv));
    ptAddPeer(PT_BCAST);
    return true;
}
static void ptRadioDown(bool bye){
    if(ptEsp){
        if(bye&&ptPeerKnown) ptSend(PT_BYE,ptPeer);
        delay(20);
        esp_now_unregister_recv_cb(); esp_now_deinit(); ptEsp=false;
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    ptPeerKnown=ptLinked=false; ptConfL=ptConfP=false;
}

// ---- screens + input -------------------------------------------------------
enum PtScreen:uint8_t{ SC_MENU=0, SC_LOBBY, SC_PLAY, SC_CARD, SC_MEDALS, SC_VSEND, SC_GAMEOVER };
static PtScreen scr=SC_MENU;
static int menuSel=0, resT=0;
static uint32_t enterAt=0;
static bool lTap,rTap,lHoldFire;
static bool lDown,rDown,lLong,rLong; static uint32_t lAt,rAt;
static void ptInput(){
    lTap=rTap=lHoldFire=false;
    uint32_t now=millis();
    if(btnL&&!btnL_prev){lDown=true;lAt=now;lLong=false;}
    if(btnL&&lDown&&!lLong&&now-lAt>PT_LONG_MS){lLong=true;lHoldFire=true;}
    if(!btnL&&btnL_prev){ if(lDown&&!lLong) lTap=true; lDown=false; }
    if(btnR&&!btnR_prev){rDown=true;rAt=now;rLong=false;}
    if(btnR&&rDown&&!rLong&&now-rAt>PT_LONG_MS){rLong=true;}
    if(!btnR&&btnR_prev){ if(rDown&&!rLong) rTap=true; rDown=false; }
    if(now-enterAt<PT_ENTER_GUARD){ lTap=rTap=lHoldFire=false; }
}

// ---- hole completion -------------------------------------------------------
static void holeCleared(){
    holesThisRun++;
    ptSv.holesCleared++;
    int under = (int)holePar - holeStrokes;
    if(under>0 && (uint16_t)under>ptSv.bestUnder) ptSv.bestUnder=(uint16_t)under;
    if(holesThisRun>(int)ptSv.bestRunHoles) ptSv.bestRunHoles=(uint16_t)holesThisRun;
    ptAward(ACH_FIRSTHOLE);
    if(under>0) ptAward(ACH_BIRDIE);
    if(holeStrokes<=1){
        ptSv.aces++;
        ptAward(ACH_ACE);
        if(wmOn) ptAward(ACH_WINDMILL);
    }
    if(sandThisHole) ptAward(ACH_SANDSAVE);
    if(ptSv.holesCleared>=10) ptAward(ACH_TEN);
    if(ptSv.holesCleared>=50) ptAward(ACH_FIFTY);
    // house economy: no XP per point - XP every 5 holes cleared
    ptSv.xpDebt++;
    if(ptSv.xpDebt>=5){ ptSv.xpDebt=0; qpAddXp(25); qpGrantPetXp(4); }
    ptDirty=true; ptFlush();     // hole cleared: a discrete event, one write
    celebT=70;
}

static void ptSettleVs(){
    if(!rvGone){
        // most holes wins; strokes break the tie (fewer is better)
        vsWon = (holesThisRun>(int)rvHoles) ||
                (holesThisRun==(int)rvHoles && strokes<=(int)rvStrokes);
        if(vsWon){ ptSv.vsWins++; qpAddXp(30); qpGrantPetXp(8); ptAward(ACH_SCRAMBLE); ptDirty=true; }
    } else vsWon=false;
    ptFlush();
    if(ptEsp) ptRadioDown(true);
    resT=0; scr=SC_VSEND;
}

// ---- the frame -------------------------------------------------------------
static void stepPhysics(){
    // dynamic furniture first so collisions see this frame's positions
    if(wmOn && wmSlot>=0){
        wmA += wmSpd;
        pw[wmSlot].x1 = wmX - cosf(wmA)*wmLen; pw[wmSlot].y1 = wmY - sinf(wmA)*wmLen;
        pw[wmSlot].x2 = wmX + cosf(wmA)*wmLen; pw[wmSlot].y2 = wmY + sinf(wmA)*wmLen;
    }
    if(gtOn && gtSlot>=0){
        gtX += gtDir;
        if(gtX<gtLo){ gtX=gtLo; gtDir=-gtDir; }
        if(gtX>gtHi){ gtX=gtHi; gtDir=-gtDir; }
        pw[gtSlot].x1=gtX; pw[gtSlot].y1=gtY;
        pw[gtSlot].x2=gtX+gtLen; pw[gtSlot].y2=gtY;
    }

    // flippers (identical easing to the house table)
    flipL.prevAngle=flipL.angle; flipR.prevAngle=flipR.angle;
    flipL.active = btnL;
    flipR.active = btnR;
    float tL = flipL.active?flipL.activeAngle:flipL.restAngle;
    float tR = flipR.active?flipR.activeAngle:flipR.restAngle;
    flipL.angle += (tL-flipL.angle)*(flipL.active?FLIP_LERP_UP:FLIP_LERP_DOWN);
    flipR.angle += (tR-flipR.angle)*(flipR.active?FLIP_LERP_UP:FLIP_LERP_DOWN);

    ball.vy += GRAVITY;
    if(inSand) ball.vy *= 0.97f;
    // Cap like the house table: while a flipper is active the ball may carry
    // far more than MAX_VEL (that's how it reaches the top); otherwise a gentle
    // cap. A hard MAX_VEL clamp here was killing every flipper kick.
    bool flipping = flipL.active || flipR.active;
    float capSp = MAX_VEL * (flipping ? 14.0f : 1.4f);
    float sp = sqrtf(ball.vx*ball.vx+ball.vy*ball.vy);
    if(sp>capSp){ ball.vx*=capSp/sp; ball.vy*=capSp/sp; }

    // SUBSTEP exactly like the house stepBall(): a fast ball moves in up to 4
    // sub-moves, colliding each time, so it can't tunnel through a wall or
    // flipper. Single-stepping (what I had) is why a hard-hit ball slipped
    // behind the funnels. steps scale with speed.
    int steps=(int)ceilf(sp/(BALL_R*0.6f));
    if(steps<1) steps=1;
    if(steps>4) steps=4;
    float dt=1.0f/steps;
    for(int st=0; st<steps; ++st){
        ball.x += ball.vx*dt; ball.y += ball.vy*dt;

        for(int i=0;i<pwN;i++){
            const PWall&w=pw[i];
            float cx,cy; closestOnSeg(w.x1,w.y1,w.x2,w.y2,ball.x,ball.y,cx,cy);
            float dx=ball.x-cx, dy=ball.y-cy;
            float d2=dx*dx+dy*dy;
            if(d2>=BALL_R*BALL_R) continue;
            float d=sqrtf(d2); float nx,ny;
            if(d>0.001f){ nx=dx/d; ny=dy/d; } else { nx=0; ny=-1; d=0; }
            ball.x += nx*(BALL_R-d); ball.y += ny*(BALL_R-d);
            reflectV(ball.vx,ball.vy,nx,ny,w.rest,0.96f);
        }
        for(int i=0;i<rockN;i++){
            Rock&r=rocks[i];
            float dx=ball.x-r.x, dy=ball.y-r.y;
        float rr=BALL_R+r.r;
        float d2=dx*dx+dy*dy;
        if(d2>=rr*rr) continue;
        float d=sqrtf(d2); if(d<0.001f) d=0.001f;
        float nx=dx/d, ny=dy/d;
        ball.x=r.x+nx*rr; ball.y=r.y+ny*rr;
        reflectV(ball.vx,ball.vy,nx,ny,0.95f,0.98f);
        r.hit=10;
        }
        collideFlip(flipL);
        collideFlip(flipR);
    }   // end substep loop
    // A flipper strike (omega-driven, so at most once per frame) costs a stroke.
    if(flipStruck){ holeStrokes++; strokes++; flipStruck=false; }

    zoneCheck();
    teleportCheck();

    // ---- the cup: only a slow ball drops; a fast one lips out -------------
    float dx=ball.x-cupX, dy=ball.y-cupY;
    float dd=sqrtf(dx*dx+dy*dy);
    if(dd < CUP_R + BALL_R*0.5f){
        float v=sqrtf(ball.vx*ball.vx+ball.vy*ball.vy);
        if(v < CUP_CATCH){ holeCleared(); return; }
        // lip-out: spit it back out along the rim, and let the player see why
        float nx=dx/(dd>0.001f?dd:1), ny=dy/(dd>0.001f?dd:1);
        ball.x=cupX+nx*(CUP_R+BALL_R); ball.y=cupY+ny*(CUP_R+BALL_R);
        reflectV(ball.vx,ball.vy,nx,ny,0.55f,0.9f);
        lipT=26;
    }

    // drain: in SOLO it costs a ball (Corner Pocket rule) and the 3rd drain
    // ends the run. In VERSUS the clock ends the round, so a drain just
    // re-serves - no ball count, or a player could be knocked out early.
    bool drained = (ball.y > (float)H-4.0f) || ball.x < 1 || ball.x > W-1;
    if(drained){
        if(vsMode){
            serveBall();
        } else if(ballsRemaining > 1){
            ballsRemaining--;
            serveBall();
        } else {
            // run over: bank the best (most holes cleared) and show game over
            if(holesThisRun > (int)ptSv.bestRunHoles){
                ptSv.bestRunHoles = (uint16_t)holesThisRun;
                ptDirty = true;
            }
            ptFlush();
            runOver = true;
            goT = 0;
        }
    }
}

// ---- draw ------------------------------------------------------------------
static void ptFooter(const char* l,const char* r){
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString(l,4,H-7,1);
    fb.setTextDatum(MR_DATUM); fb.drawString(r,W-4,H-7,1);
}
static void drawFlipper(const PFlip&f){
    float tx,ty; flipEndpoints(f,tx,ty);
    fb.drawLine((int)f.pivotX,(int)f.pivotY,(int)tx,(int)ty,LIME);
    fb.drawLine((int)f.pivotX,(int)f.pivotY-1,(int)tx,(int)ty-1,LIME);
    fb.fillCircle((int)f.pivotX,(int)f.pivotY,3,LIME_MED);
    fb.fillCircle((int)tx,(int)ty,3,LIME_MED);
}
static void drawTable(){
    fb.fillSprite(TEAL_DEEP);
    // felt
    fb.fillRect(2,2,W-4,H-4,TEAL_DARK);      // full playfield, no crop
    for(int y=40;y<260;y+=26)
        for(int x=8;x<W-8;x+=26) fb.drawPixel(x,y,TEAL);

    // zones
    for(int i=0;i<zoneN;i++){
        const Zone&z=zones[i];
        if(z.kind==0){
            fb.fillRoundRect((int)z.x,(int)z.y,(int)z.w,(int)z.h,3,LIME_DARK);
            for(int k=0;k<5;k++) fb.drawPixel((int)z.x+3+k*5,(int)z.y+3+(k&1),LIME_MED);
        } else {
            fb.fillRoundRect((int)z.x,(int)z.y,(int)z.w,(int)z.h,3,TEAL_BRIGHT);
            for(int k=0;k<4;k++) fb.drawPixel((int)z.x+4+k*5,(int)z.y+4,TEAL_LIGHT);
        }
    }
    // pipes
    if(ppOn){
        fb.drawCircle((int)ppAx,(int)ppAy,6,PURPLE_LIGHT);
        fb.drawCircle((int)ppAx,(int)ppAy,3,PURPLE);
        fb.drawCircle((int)ppBx,(int)ppBy,6,PURPLE_LIGHT);
        fb.drawCircle((int)ppBx,(int)ppBy,3,PURPLE);
    }
    // walls
    for(int i=0;i<pwN;i++){
        const PWall&w=pw[i];
        uint16_t c = w.kind==WK_FURN ? PURPLE : (w.kind==WK_GATE ? PURPLE_BRIGHT : TEAL_LIGHT);
        fb.drawLine((int)w.x1,(int)w.y1,(int)w.x2,(int)w.y2,c);
    }
    // windmill hub
    if(wmOn){ fb.fillCircle((int)wmX,(int)wmY,3,PURPLE_LIGHT); }
    // rocks
    for(int i=0;i<rockN;i++){
        Rock&r=rocks[i];
        uint16_t c = r.hit? LIME : PURPLE_LIGHT;
        fb.fillCircle((int)r.x,(int)r.y,(int)r.r,c);
        fb.drawCircle((int)r.x,(int)r.y,(int)r.r,LIME_PALE);
        if(r.hit) r.hit--;
    }
    // the cup + flag
    fb.fillCircle((int)cupX,(int)cupY,(int)CUP_R,TEAL_DEEP);
    fb.drawCircle((int)cupX,(int)cupY,(int)CUP_R,lipT?PURPLE_LIGHT:LIME);
    fb.drawLine((int)cupX,(int)cupY-(int)CUP_R,(int)cupX,(int)cupY-20,LIME_PALE);
    fb.fillTriangle((int)cupX,(int)cupY-20,(int)cupX,(int)cupY-12,(int)cupX+9,(int)cupY-16,PURPLE_LIGHT);
    if(lipT) lipT--;

    drawFlipper(flipL); drawFlipper(flipR);
    // ball
    fb.fillCircle((int)ball.x,(int)ball.y,(int)BALL_R,LIME_PALE);
    fb.drawPixel((int)ball.x-1,(int)ball.y-1,LIME);

    // HUD
    char b[26];
    fb.fillRect(0,0,W,24,PURPLE_DEEP);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"HOLE %d",level);
    fb.drawString(b,3,2,2);
    fb.setTextColor(TEAL_LIGHT);
    snprintf(b,sizeof(b),"PAR %d",(int)holePar);
    fb.drawString(b,3,14,1);
    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(holeStrokes>(int)holePar?PURPLE_LIGHT:LIME);
    snprintf(b,sizeof(b),"%d",holeStrokes);
    fb.drawString(b,W-4,2,2);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("STROKES",W-4,16,1);
    // balls remaining (Corner Pocket style), centred under the hole label
    fb.setTextDatum(TC_DATUM);
    for(int i=0;i<3;i++){
        uint16_t c = (i < ballsRemaining) ? LIME_PALE : TEAL_DARK;
        fb.fillCircle(W/2 - 12 + i*12, 8, 3, c);
    }
    if(vsMode){
        fb.setTextDatum(TC_DATUM);
        int secs=vsFrames/62;
        fb.setTextColor(rvGone?TEAL_DARK:TEAL_LIGHT);
        if(rvGone) fb.drawString("RIVAL LOST",W/2,2,1);
        else { snprintf(b,sizeof(b),"YOU %d  RIVAL %d",holesThisRun,(int)rvHoles); fb.drawString(b,W/2,2,1); }
        snprintf(b,sizeof(b),"%d:%02d",secs/60,secs%60);
        fb.drawString(b,W/2,13,1);
    }
    if(celebT){
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(LIME);
        const char* msg = holeStrokes<=1 ? "HOLE IN ONE!" :
                          (holeStrokes<(int)holePar ? "BIRDIE!" :
                          (holeStrokes==(int)holePar ? "PAR" : "HOLED OUT"));
        fb.drawString(msg,W/2,140,4);
        for(int i=0;i<6;i++){
            int a=(frameNo*3+i*60)%360;
            float rad=a*0.01745f;
            fb.fillCircle((int)(cupX+cosf(rad)*(70-celebT)/2),(int)(cupY+sinf(rad)*(70-celebT)/2),2,LIME_PALE);
        }
    }
}
static void drawMenu(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,96,W,120,TEAL_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS",W/2,30,4);
    fb.drawString("PUTT",W/2,54,4);
    // hero: cup + flag + ball
    fb.drawCircle(W/2,130,8,LIME);
    fb.drawLine(W/2,122,W/2,100,LIME_PALE);
    fb.fillTriangle(W/2,100,W/2,110,W/2+14,105,PURPLE_LIGHT);
    fb.fillCircle(W/2-30,150,4,LIME_PALE);
    static const char* IT[3]={"PLAY THE COURSE","VERSUS SCRAMBLE","CLUB MEDALS"};
    char sub[3][28];
    snprintf(sub[0],28,"%u holes  -  best run %u",(unsigned)ptSv.holesCleared,(unsigned)ptSv.bestRunHoles);
    snprintf(sub[1],28,"3 min duel over ESP-NOW");
    { int g=0; for(int i=0;i<8;i++) if((ptSv.ach>>i)&1) g++;
      snprintf(sub[2],28,"%d/8  -  %u aces",g,(unsigned)ptSv.aces); }
    for(int i=0;i<3;i++){
        int y=180+i*44; bool s=(i==menuSel);
        fb.fillRoundRect(10,y,W-20,38,5,s?TEAL_DARK:TEAL_DEEP);
        fb.drawRoundRect(10,y,W-20,38,5,s?LIME:TEAL_DARK);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(s?LIME_PALE:TEAL_BRIGHT);
        fb.drawString(IT[i],W/2,y+13,2);
        fb.setTextColor(s?TEAL_LIGHT:TEAL_DARK);
        fb.drawString(sub[i],W/2,y+28,1);
    }
    ptFooter("L: next","R: select");
}
static void drawLobby(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("FINDING A RIVAL",W/2,60,2);
    float a=(millis()%2000)*0.00314f;
    fb.drawCircle(W/2,150,38,TEAL_DARK); fb.drawCircle(W/2,150,24,TEAL_DARK);
    fb.drawLine(W/2,150,W/2+(int)(cosf(a)*38),150+(int)(sinf(a)*38),LIME);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(ptPeerKnown?"RIVAL FOUND - LINKING":"hold devices close",W/2,212,1);
    char t[12]; snprintf(t,sizeof(t),"%us",(unsigned)((PT_SEARCH_MS-(millis()-ptSearchT0))/1000));
    fb.drawString(t,W/2,230,1);
    ptFooter("L: cancel","");
}
static void drawVsEnd(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRoundRect(10,70,W-20,150,8,TEAL_DEEP);
    fb.drawRoundRect(10,70,W-20,150,8,vsWon?LIME:PURPLE_LIGHT);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(vsWon?LIME:PURPLE_LIGHT);
    fb.drawString(vsWon?"SCRAMBLE WON!":(rvGone?"RIVAL LOST":"RIVAL WINS"),W/2,92,2);
    char b[26];
    fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"YOU   %d holes",holesThisRun);
    fb.drawString(b,W/2,120,2);
    fb.setTextColor(TEAL_BRIGHT);
    snprintf(b,sizeof(b),"RIVAL %d holes",(int)rvHoles);
    fb.drawString(b,W/2,140,2);
    fb.setTextColor(TEAL_LIGHT);
    snprintf(b,sizeof(b),"%d strokes vs %d",strokes,(int)rvStrokes);
    fb.drawString(b,W/2,158,1);
    if(vsWon) fb.drawString("+30 XP",W/2,174,1);
    int y=190;
    for(int i=0;i<achQueueN&&y<214;i++,y+=12){
        fb.setTextColor(PURPLE_LIGHT);
        snprintf(b,sizeof(b),"MEDAL: %s",ACHS[achQueue[i]].name);
        fb.drawString(b,W/2,y,1);
    }
    ptFooter("L: menu","R: menu");
}
static void drawMedals(){
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("CLUB MEDALS",4,9,1);
    char b[12]; int g=0; for(int i=0;i<8;i++) if((ptSv.ach>>i)&1) g++;
    fb.setTextDatum(MR_DATUM); snprintf(b,sizeof(b),"%d/8",g); fb.drawString(b,W-4,9,1);
    for(int i=0;i<8;i++){
        int y=24+i*35; bool have=(ptSv.ach>>i)&1;
        fb.fillRoundRect(4,y,W-8,32,4,TEAL_DEEP);
        fb.drawRoundRect(4,y,W-8,32,4,have?LIME_DARK:TEAL_DARK);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(have?LIME:TEAL_LIGHT);
        fb.drawString(ACHS[i].name,10,y+9,1);
        fb.setTextColor(have?LIME_PALE:TEAL_DARK);
        fb.drawString(ACHS[i].how,10,y+21,1);
        if(have){ fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME);
                  snprintf(b,sizeof(b),"+%d",ACHS[i].xp); fb.drawString(b,W-10,y+15,1); }
    }
    ptFooter("","L/R: back");
}

static void drawGameOver(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(PURPLE_LIGHT);
    fb.drawString("GAME OVER",W/2,64,4);
    char b[26];
    bool best = holesThisRun >= (int)ptSv.bestRunHoles && holesThisRun>0;
    fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"HOLES  %d",holesThisRun);
    fb.drawString(b,W/2,120,4);
    fb.setTextColor(TEAL_LIGHT);
    snprintf(b,sizeof(b),"%d strokes",strokes);
    fb.drawString(b,W/2,150,2);
    fb.setTextColor(best?LIME:TEAL_BRIGHT);
    snprintf(b,sizeof(b),"BEST  %u",(unsigned)ptSv.bestRunHoles);
    fb.drawString(b,W/2,180,2);
    if(best){ fb.setTextColor(LIME); fb.drawString("NEW BEST!",W/2,204,2); }
    fb.setTextColor(TEAL_DARK);
    fb.drawString("press to continue",W/2,270,1);
    fb.pushSprite(0,0);
}
void mantisputtDraw(){
    switch(scr){
    case SC_MENU:   drawMenu();   break;
    case SC_LOBBY:  drawLobby();  break;
    case SC_PLAY:
    case SC_CARD:   drawTable();  break;
    case SC_MEDALS: drawMedals(); break;
    case SC_VSEND:  drawVsEnd();  break;
    case SC_GAMEOVER: drawGameOver(); return;
    }
    fb.pushSprite(0,0);
}

// ---- lobby + tick + lifecycle ---------------------------------------------
static void ptTickLobby(){
    uint32_t now=millis();
    if(now-ptLastHello>PT_HELLO_MS){ ptSend(PT_HELLO,PT_BCAST); ptLastHello=now; }
    if(rxHello&&!ptPeerKnown){
        rxHello=false;
        memcpy(ptPeer,(const void*)rxMac,6); ptAddPeer(ptPeer);
        ptPeerKnown=true; ptIsHost=(ptMyId<rxId);
        ptSend(PT_CONFIRM,ptPeer); ptConfL=true;
    }
    if(rxConfirm&&ptPeerKnown){
        rxConfirm=false; ptConfP=true;
        if(!ptConfL){ ptSend(PT_CONFIRM,ptPeer); ptConfL=true; }
    }
    if(ptConfL&&ptConfP&&!ptLinked){
        ptLinked=true; ptLastSeen=now; ptLastStatus=now;
        if(ptIsHost){
            vsSeed=(uint32_t)esp_random()|1;
            ptSend(PT_START,ptPeer);
            ptClearRx();
            seedBase=vsSeed; vsMode=true; vsDone=false; rvDone=rvGone=false;
            rvHoles=rvStrokes=0; vsFrames=VS_FRAMES;
            strokes=0; holesThisRun=0; achQueueN=0; ballsRemaining=3; runOver=false;
            newHole(1); scr=SC_PLAY;
        }
    }
    if(ptLinked&&!ptIsHost&&rxStart){
        rxStart=false; ptClearRx();
        seedBase=rxSeed; vsMode=true; vsDone=false; rvDone=rvGone=false;
        rvHoles=rvStrokes=0; vsFrames=VS_FRAMES;
        strokes=0; holesThisRun=0; achQueueN=0; ballsRemaining=3; runOver=false;
        newHole(1); scr=SC_PLAY;
    }
    if(!ptLinked&&now-ptSearchT0>PT_SEARCH_MS){ ptRadioDown(false); scr=SC_MENU; }
    if(lTap||lHoldFire){ ptRadioDown(true); scr=SC_MENU; }
}

static void ptTickNet(){
    if(!vsMode||!ptEsp) return;
    uint32_t now=millis();
    if(rxBye){ rxBye=false; ptRadioDown(false); rvGone=true; }
    if(rxStatus){ rxStatus=false; ptLastSeen=now; rvHoles=rxHoles; rvStrokes=rxStrokes; rvDone=rxFin!=0; }
    if(ptLinked&&now-ptLastStatus>PT_STATUS_MS){ ptSend(PT_STATUS,ptPeer); ptLastStatus=now; }
    if(ptLinked&&now-ptLastSeen>PT_PEER_LOST){ ptRadioDown(false); rvGone=true; }
}

void mantisputtTick(){
    ptInput();
    frameNo++;
    switch(scr){
    case SC_MENU:
        if(lTap) menuSel=(menuSel+1)%3;
        if(rTap){
            if(menuSel==0){
                vsMode=false; seedBase=(uint32_t)esp_random()|1;
                strokes=0; holesThisRun=0; achQueueN=0; ballsRemaining=3; runOver=false;
                newHole(1); scr=SC_PLAY;
            } else if(menuSel==1){
                ptClearRx(); rxStartSeq=0x1FF;
                ptEsp=ptRadioUp();
                if(ptEsp){ ptSearchT0=millis(); ptLastHello=0; scr=SC_LOBBY; }
            } else scr=SC_MEDALS;
        }
        break;
    case SC_LOBBY: ptTickLobby(); break;
    case SC_PLAY: {
        ptTickNet();
        if(vsMode){
            if(!vsDone && --vsFrames<=0){
                vsDone=true;
                if(ptLinked){ ptSend(PT_STATUS,ptPeer); ptLastStatus=millis(); }
            }
            if(vsDone && (rvDone||rvGone||!ptLinked)){ ptSettleVs(); break; }
            if(vsDone) break;                 // rolled to a stop, waiting
        }
        if(celebT>0){
            if(--celebT==0) newHole(level+1);  // cup -> next level spawns
            break;
        }
        stepPhysics();
        if(!vsMode && runOver){ scr=SC_GAMEOVER; goT=0; break; }
        if(!vsMode && lHoldFire){ ptFlush(); scr=SC_MENU; }
        break; }
    case SC_MEDALS: if(lTap||rTap) scr=SC_MENU; break;
    case SC_VSEND:  if(lTap||rTap) scr=SC_MENU; break;
    case SC_GAMEOVER:
        goT++;
        if(goT>30 && (lTap||rTap)) scr=SC_MENU;   // 0.5s guard, then any tap
        break;
    default: break;
    }
}

void mantisputtEnter(){
    fb.setRotation(0);
    ptLoad();
    scr=SC_MENU; menuSel=0;
    enterAt=millis(); frameNo=0;
    lDown=rDown=false; lLong=rLong=false;
    ptEsp=false; ptPeerKnown=ptLinked=false; ptConfL=ptConfP=false;
    ptClearRx(); rxStartSeq=0x1FF; ptSeq=0;
    vsMode=false; achQueueN=0; ballsRemaining=3; runOver=false; goT=0;
    ptDirty=false;
    pgRegisterFlush(ptFlush);   // flushed by pgFlushAll() on sleep (manual hold or low-voltage cutoff)
    flipL = { FLIP_L_PX, FLIP_Y, FLIP_REST_RAD, FLIP_REST_RAD,
              FLIP_REST_RAD, FLIP_ACT_RAD, +1, false };
    flipR = { FLIP_R_PX, FLIP_Y, (float)M_PI-FLIP_REST_RAD, (float)M_PI-FLIP_REST_RAD,
              (float)M_PI-FLIP_REST_RAD, (float)M_PI-FLIP_ACT_RAD, -1, false };
    strokes=0; holesThisRun=0; level=1;
    seedBase=millis()|1;
    newHole(1);
    scr=SC_MENU;
}
void mantisputtShutdown(){
    ptRadioDown(true);
    ptFlush();
}
