// FILE: src/mantisvaders/mantisvaders_mode.cpp
// MANTIS VADERS - a Galaga/Space-Invaders playfield sitting on top of the
// house pinball table.
//
// THE BOTTOM HALF IS THE HOUSE TABLE, COPIED VERBATIM from main.cpp: the same
// constants, the same buildWalls funnels running to the flipper tips, the same
// resetBall serve, the same substepping stepBall, and the same radial-normal
// anti-trap collideFlipper with the CORNER POCKET kick tuning (gain 130 direct,
// cap MAX_VEL*14 while a flipper is active) - because this table has no bumpers
// to help the ball climb, exactly like Corner Pocket and Slots.
//
// THE TOP HALF is the invasion: waves of bug-ships drift down the screen in
// formation. You blast them with the BALL. Hits pay a climbing combo, clearing
// a wave advances you, and a ship that reaches the funnel line steals a life.
// Powerups drop from destroyed ships - BLASTER (the ball fires shots),
// MULTIBALL, POWER FLIPPERS, and SHIELD.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include <Preferences.h>
#include "mantisvaders_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ===========================================================================
//  HOUSE PINBALL PHYSICS - copied from main.cpp, values unchanged
// ===========================================================================
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
static constexpr float FLIP_L_PX       = (float)W * 0.5f - DRAIN_HALF_GAP
                                         - FLIP_LEN * FLIP_REST_COS;
static constexpr float FLIP_R_PX       = (float)W * 0.5f + DRAIN_HALF_GAP
                                         + FLIP_LEN * FLIP_REST_COS;
static constexpr float FUNNEL_TOP_Y    = (float)H - 90.0f;   // 230

static inline float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }
static inline float lerpf(float a,float b,float t){ return a + (b-a)*t; }

struct Ball {
    float x,y,vx,vy;
    bool  on;
};
struct Flipper {
    float pivotX,pivotY;
    float angle,prevAngle;
    float restAngle,activeAngle;
    int   sign;
    bool  active;
};
struct Wall { float x1,y1,x2,y2,nx,ny,restitution; };

static constexpr int MAX_BALLS = 3;
static Ball   balls[MAX_BALLS];
static Flipper flipL, flipR;
static Wall* walls = nullptr;   // [5] on the heap while the app is open

static void closestPointOnSegment(float ax,float ay,float bx,float by,
                                  float px,float py,float&cx,float&cy){
    float dx=bx-ax, dy=by-ay;
    float l2=dx*dx+dy*dy;
    if(l2<0.000001f){ cx=ax; cy=ay; return; }
    float t=((px-ax)*dx+(py-ay)*dy)/l2;
    t=clampf(t,0.0f,1.0f);
    cx=ax+t*dx; cy=ay+t*dy;
}
static void reflectVelocity(float&vx,float&vy,float nx,float ny,
                            float restitution,float friction){
    float vn=vx*nx+vy*ny;
    if(vn>0) return;
    float vtx=vx-vn*nx, vty=vy-vn*ny;
    vx=vtx*friction-vn*restitution*nx;
    vy=vty*friction-vn*restitution*ny;
}
static void buildWalls(){
    walls[0]={ 0.0f, 0.0f, 0.0f, FUNNEL_TOP_Y, 1.0f, 0.0f, 0.85f };
    walls[1]={ (float)W, 0.0f, (float)W, FUNNEL_TOP_Y, -1.0f, 0.0f, 0.85f };
    walls[2]={ 0.0f, 0.0f, (float)W, 0.0f, 0.0f, 1.0f, 0.85f };
    walls[3]={ 0.0f, FUNNEL_TOP_Y, FLIP_L_PX-4.0f, FLIP_Y-2.0f, 0.0f,0.0f, 0.57f };
    walls[4]={ (float)W, FUNNEL_TOP_Y, FLIP_R_PX+4.0f, FLIP_Y-2.0f, 0.0f,0.0f, 0.57f };
    auto setNormal=[](Wall&w,bool leftFunnel){
        float dx=w.x2-w.x1, dy=w.y2-w.y1;
        float len=sqrtf(dx*dx+dy*dy);
        if(len<0.001f){ w.nx=0; w.ny=-1; return; }
        if(leftFunnel){ w.nx= dy/len; w.ny=-dx/len; }
        else          { w.nx=-dy/len; w.ny= dx/len; }
    };
    setNormal(walls[3],true);
    setNormal(walls[4],false);
}
static void initFlippers(){
    flipL={ FLIP_L_PX, FLIP_Y, FLIP_REST_RAD, FLIP_REST_RAD,
            FLIP_REST_RAD, FLIP_ACT_RAD, +1, false };
    flipR={ FLIP_R_PX, FLIP_Y,
            (float)M_PI-FLIP_REST_RAD, (float)M_PI-FLIP_REST_RAD,
            (float)M_PI-FLIP_REST_RAD, (float)M_PI-FLIP_ACT_RAD,
            -1, false };
}
static void resetBall(Ball&b){
    b.x=(float)W*0.5f-4.0f;
    b.y=(float)H-70.0f;
    b.vx=1.6f;
    b.vy=-4.4f;
    b.on=true;
}
static void updateFlipper(Flipper&f,bool pressed){
    f.prevAngle=f.angle;
    f.active=pressed;
    float target = pressed? f.activeAngle : f.restAngle;
    float lerpAmt= pressed? FLIP_LERP_UP  : FLIP_LERP_DOWN;
    f.angle=lerpf(f.angle,target,lerpAmt);
}
static inline void flipperEndpoints(const Flipper&f,float&tipX,float&tipY){
    tipX=f.pivotX+cosf(f.angle)*FLIP_LEN;
    tipY=f.pivotY+sinf(f.angle)*FLIP_LEN;
}
static void collideWalls(Ball&b){
    for(int i=0;i<5;i++){
        const Wall&w=walls[i];
        float dx=w.x2-w.x1, dy=w.y2-w.y1;
        if(dx*dx+dy*dy<0.001f) continue;
        float cx,cy; closestPointOnSegment(w.x1,w.y1,w.x2,w.y2,b.x,b.y,cx,cy);
        float pdx=b.x-cx, pdy=b.y-cy;
        float d2=pdx*pdx+pdy*pdy;
        if(d2<BALL_R*BALL_R){
            float d=sqrtf(d2); float nx,ny;
            if(d>0.001f){ nx=pdx/d; ny=pdy/d; } else { nx=w.nx; ny=w.ny; }
            float pen=BALL_R-d;
            b.x+=nx*pen; b.y+=ny*pen;
            reflectVelocity(b.vx,b.vy,nx,ny,w.restitution,0.96f);
        }
    }
}
static int powerFlipFrames=0;    // POWER FLIPPERS powerup, same idea as main.cpp

static void collideFlipper(Ball&b,Flipper&f){
    float tipX,tipY; flipperEndpoints(f,tipX,tipY);
    float cx,cy; closestPointOnSegment(f.pivotX,f.pivotY,tipX,tipY,b.x,b.y,cx,cy);
    float pdx=b.x-cx, pdy=b.y-cy;
    float minDist=BALL_R+FLIP_HALF_THICK;
    float d2=pdx*pdx+pdy*pdy;
    if(d2>=minDist*minDist) return;
    float d=sqrtf(d2); float nx,ny;
    if(d>0.001f){ nx=pdx/d; ny=pdy/d; }
    else {
        float dx=tipX-f.pivotX, dy=tipY-f.pivotY;
        float len=sqrtf(dx*dx+dy*dy);
        nx=-dy/len; ny=dx/len;
    }
    // anti-trap, verbatim from the house table
    float midAngle=(f.restAngle+f.activeAngle)*0.5f;
    bool extended=(f.sign>0)?(f.angle<midAngle):(f.angle>midAngle);
    if(ny>0 && (f.active||extended)){
        nx=-nx; ny=-ny;
        b.x=cx+nx*(minDist+0.5f);
        b.y=cy+ny*(minDist+0.5f);
    }
    float pen=minDist-d;
    b.x+=nx*pen; b.y+=ny*pen;
    reflectVelocity(b.vx,b.vy,nx,ny,0.92f,0.97f);
    // Kick: this table has no bumpers, so it uses the CORNER POCKET tuning -
    // gain 130 applied DIRECTLY (no /1000) and a cap of MAX_VEL*14 while a
    // flipper is active, which is what lets the ball reach the top.
    float omega=f.angle-f.prevAngle;
    float rx=cx-f.pivotX, ry=cy-f.pivotY;
    float tx=-ry, ty=rx;
    float baseGain=130.0f;
    float kickGain=(powerFlipFrames>0)? baseGain*(32.0f/18.0f) : baseGain;
    b.vx+=tx*omega*kickGain;
    b.vy+=ty*omega*kickGain;
    float sp2=b.vx*b.vx+b.vy*b.vy;
    float capSp=MAX_VEL*((powerFlipFrames>0)?1.7f:14.0f);
    if(sp2>capSp*capSp){ float s=capSp/sqrtf(sp2); b.vx*=s; b.vy*=s; }
}

// ===========================================================================
//  THE INVASION (top of the playfield)
// ===========================================================================
static constexpr int MAX_SHIPS = 24;
static constexpr float SHIP_R  = 7.0f;
// A ship reaching this line breaches the table and costs a life. It sits above
// the funnel mouth so the pinball bottom half is never crowded.
static constexpr float BREACH_Y = FUNNEL_TOP_Y - 26.0f;

struct Ship {
    float x,y;         // live position, updated every tick
    float hx;          // home x within the formation (drift is added to it)
    int8_t hp;
    uint8_t kind;      // 0 grunt, 1 armored, 2 carrier (drops a powerup)
    uint8_t flash;
    bool on;
};
static Ship* ships = nullptr;   // [MAX_SHIPS] on the heap while the app is open
static int  shipCount=0;
static float formDrift=0;      // horizontal sway of the whole formation
static float sinkRate=0.014f;

static constexpr int MAX_SHOTS=6;
struct Shot { float x,y,vy; bool on; };
static Shot* shots = nullptr;   // [MAX_SHOTS] on the heap while the app is open

static constexpr int MAX_PU=3;
struct Powerup { float x,y,vy; uint8_t kind; bool on; };  // 0 blaster,1 multiball,2 powerflip,3 shield
static Powerup pus[MAX_PU];

// run state
static uint32_t score=0;
static int   wave=1, lives=3;
static int   combo=0, comboCool=0;
static int   blasterFrames=0, shieldFrames=0;
static int   blasterCooldown=0;
static bool  gameOver=false;
static int   goT=0;
static uint32_t frameNo=0;
static float flashLevel=0;
static char  toast[28]=""; static uint32_t toastT=0;

// rng
static uint32_t rng=1;
static uint32_t pr(){ rng=rng*1664525u+1013904223u; return rng>>8; }
static int prange(int a,int b){ return b<=a?a:a+(int)(pr()%(uint32_t)(b-a+1)); }

static void say(const char* t){ snprintf(toast,sizeof(toast),"%s",t); toastT=millis(); }

// ---- wave construction -----------------------------------------------------
static void buildWave(int w){
    memset(ships,0,(sizeof(Ship)*(MAX_SHIPS)));
    shipCount=0;
    int cols = 5;
    int rows = 2 + (w>2?1:0) + (w>5?1:0);
    if(rows>4) rows=4;
    float spanW = (float)W - 36.0f;
    for(int r=0;r<rows;r++){
        for(int c=0;c<cols;c++){
            if(shipCount>=MAX_SHIPS) break;
            Ship&s=ships[shipCount++];
            s.hx = 18.0f + spanW*((float)c/(float)(cols-1));
            s.x  = s.hx;
            s.y  = 34.0f + r*20.0f;
            // deeper rows are tougher; one carrier per wave holds a powerup
            s.kind = (r==0 && c==cols/2) ? 2 : ((r>=rows-1 && w>=3) ? 1 : 0);
            s.hp   = s.kind==1 ? 2 : 1;
            if(w>=6 && s.kind==0 && ((r+c)&1)) s.hp=2;
            s.on=true;
        }
    }
    formDrift=0;
    // Descent pacing: ~2 minutes to breach on wave 1, tightening to ~40s by
    // wave 8. Slow enough to feel like a creeping swarm, fast enough to press.
    sinkRate = 0.022f + 0.007f*(float)(w-1);
    if(sinkRate>0.070f) sinkRate=0.070f;
}

static int shipsAlive(){
    int n=0; for(int i=0;i<shipCount;i++) if(ships[i].on) n++;
    return n;
}

static void spawnPowerupAt(float x,float y){
    for(int i=0;i<MAX_PU;i++) if(!pus[i].on){
        pus[i]={x,y,0.7f,(uint8_t)prange(0,3),true};
        return;
    }
}
static void grantPowerup(uint8_t kind){
    switch(kind){
    case 0: blasterFrames=600; say("BLASTER!"); break;
    case 1: {
        // MULTIBALL: wake a spare ball at the serve point
        for(int i=1;i<MAX_BALLS;i++) if(!balls[i].on){ resetBall(balls[i]); say("MULTIBALL!"); break; }
        break; }
    case 2: powerFlipFrames=480; say("POWER FLIPPERS!"); break;
    default: shieldFrames=720; say("SHIELD!"); break;
    }
}

// ---- ball vs ship ----------------------------------------------------------
static void killShip(Ship&s){
    s.on=false;
    combo++; comboCool=0;
    score += (uint32_t)(25*(s.kind==1?2:1)) * (uint32_t)(combo<12?combo:12);
    flashLevel=0.7f;
    if(s.kind==2) spawnPowerupAt(s.x,s.y);
}
static void collideShips(Ball&b){
    for(int i=0;i<shipCount;i++){
        Ship&s=ships[i];
        if(!s.on) continue;
        float dx=b.x-s.x, dy=b.y-s.y;
        float rr=BALL_R+SHIP_R;
        float d2=dx*dx+dy*dy;
        if(d2>=rr*rr) continue;
        float d=sqrtf(d2); if(d<0.001f){ d=0.001f; dx=0; dy=-1; }
        float nx=dx/d, ny=dy/d;
        b.x=s.x+nx*rr; b.y=s.y+ny*rr;
        reflectVelocity(b.vx,b.vy,nx,ny,0.78f,0.95f);
        s.flash=3;
        if(--s.hp<=0) killShip(s);
        else { score+=5; }
    }
}
static void collidePowerupPickups(Ball&b){
    for(int i=0;i<MAX_PU;i++){
        Powerup&p=pus[i];
        if(!p.on) continue;
        float dx=b.x-p.x, dy=b.y-p.y;
        if(dx*dx+dy*dy < (BALL_R+6.0f)*(BALL_R+6.0f)){
            p.on=false;
            grantPowerup(p.kind);
        }
    }
}

// ---- stepBall: the house version, with the invaders spliced into the substep
static void stepBall(Ball&b){
    b.vy += GRAVITY;
    b.vx = clampf(b.vx,-MAX_VEL,MAX_VEL);
    b.vy = clampf(b.vy,-MAX_VEL,MAX_VEL);
    float speed=sqrtf(b.vx*b.vx+b.vy*b.vy);
    int steps=(int)ceilf(speed/(BALL_R*0.6f));
    if(steps<1) steps=1;
    if(steps>4) steps=4;
    float dt=1.0f/steps;
    for(int s=0;s<steps;s++){
        b.x+=b.vx*dt;
        b.y+=b.vy*dt;
        collideWalls(b);
        collideShips(b);
        collideFlipper(b,flipL);
        collideFlipper(b,flipR);
    }
    collidePowerupPickups(b);
    if(b.y > (float)H + 6.0f) b.on=false;
}

// ===========================================================================
//  PERSISTENCE (dirty flag, discrete events + shutdown only - never per tick)
// ===========================================================================
static const char* NVS_MV="mvaders";
struct __attribute__((packed)) MvSave {
    uint16_t magic; uint8_t version; uint8_t ach;
    uint32_t hiScore; uint16_t bestWave;
};
static MvSave mv; static bool mvDirty=false;
static void mvLoad(){
    memset(&mv,0,sizeof(mv)); mv.magic=0x4D56; mv.version=1;
    Preferences p; if(!p.begin(NVS_MV,true)) return;
    MvSave t; size_t n=p.getBytes("blob",&t,sizeof(t)); p.end();
    if(n==sizeof(t)&&t.magic==0x4D56&&t.version==1) mv=t;
}
static void mvFlush(){
    if(!mvDirty) return;
    Preferences p; if(!p.begin(NVS_MV,false)) return;
    p.putBytes("blob",&mv,sizeof(mv)); p.end(); mvDirty=false;
}

// achievements - event driven XP only, never per point
enum { A_FIRSTKILL=0, A_WAVE3, A_WAVE6, A_COMBO10, A_BLASTER, A_MULTI, A_NOLOSS, A_SCORE, A_COUNT_ };
static const char* A_NAME[A_COUNT_]={
    "FIRST BLOOD","WAVE 3","WAVE 6","COMBO x10","GUNNER","SWARM","UNTOUCHED","10K CLUB" };
static const uint8_t A_XP[A_COUNT_]={10,20,35,25,15,15,40,50};
static uint8_t achQ[4]; static int achQN=0;
static void grantAch(int a){
    if((mv.ach>>a)&1) return;
    mv.ach|=(uint8_t)(1<<a); mvDirty=true;
    qpAddXp(A_XP[a]);
    if(achQN<4) achQ[achQN++]=(uint8_t)a;
}

// ===========================================================================
//  TICK
// ===========================================================================
static void nextWave(){
    wave++;
    if(wave==3) grantAch(A_WAVE3);
    if(wave==6) grantAch(A_WAVE6);
    // wave clear pays XP through the achievement/level path, not per point
    if((wave%3)==0){ qpAddXp(25); qpGrantPetXp(4); }
    if((uint16_t)wave>mv.bestWave){ mv.bestWave=(uint16_t)wave; mvDirty=true; }
    buildWave(wave);
    say("WAVE CLEAR!");
}

static void loseLife(const char* why){
    if(shieldFrames>0){ shieldFrames=0; say("SHIELD HELD!"); return; }
    lives--;
    say(why);
    if(lives<=0){
        gameOver=true; goT=0;
        if(score>mv.hiScore){ mv.hiScore=score; mvDirty=true; }
        if(score>=10000) grantAch(A_SCORE);
        mvFlush();
    }
}

void mantisvadersTick(){
    frameNo++;
    if(flashLevel>0) flashLevel-=0.06f;
    if(toast[0] && millis()-toastT>1800) toast[0]='\0';
    if(achQN>0 && (frameNo%100)==0){ for(int i=1;i<achQN;i++) achQ[i-1]=achQ[i]; achQN--; }

    if(gameOver){
        if(++goT>30 && ((btnL&&!btnL_prev)||(btnR&&!btnR_prev))) mantisvadersEnter();
        return;
    }

    // ---- flippers: the house update, both buttons independent -------------
    updateFlipper(flipL, btnL);
    updateFlipper(flipR, btnR);
    if(powerFlipFrames>0) powerFlipFrames--;
    if(shieldFrames>0)    shieldFrames--;
    if(blasterFrames>0)   blasterFrames--;
    if(blasterCooldown>0) blasterCooldown--;

    // ---- combo decay -------------------------------------------------------
    if(combo>0){
        if(++comboCool>110){ combo=0; comboCool=0; }
        if(combo>=10) grantAch(A_COMBO10);
    }

    // ---- formation drift + sink -------------------------------------------
    formDrift = sinf((float)frameNo*0.012f)*10.0f;
    bool breached=false;
    for(int i=0;i<shipCount;i++){
        Ship&s=ships[i];
        if(!s.on) continue;
        if(s.flash) s.flash--;
        s.x = s.hx + formDrift;      // sway
        s.y += sinkRate;             // creep down
        if(s.y > BREACH_Y) breached=true;
    }
    if(breached){
        loseLife("BREACH!");
        if(!gameOver){ buildWave(wave); }
        return;
    }

    // ---- blaster: the ball fires upward while the powerup is live ----------
    if(blasterFrames>0 && blasterCooldown==0){
        for(int b=0;b<MAX_BALLS;b++){
            if(!balls[b].on) continue;
            for(int i=0;i<MAX_SHOTS;i++) if(!shots[i].on){
                shots[i]={balls[b].x, balls[b].y-BALL_R-2.0f, -3.4f, true};
                blasterCooldown=10;
                break;
            }
            break;
        }
    }
    for(int i=0;i<MAX_SHOTS;i++){
        Shot&s=shots[i];
        if(!s.on) continue;
        s.y+=s.vy;
        if(s.y<-4){ s.on=false; continue; }
        for(int k=0;k<shipCount;k++){
            Ship&sh=ships[k];
            if(!sh.on) continue;
            float dx=s.x-sh.x, dy=s.y-sh.y;
            if(dx*dx+dy*dy < (SHIP_R+2.0f)*(SHIP_R+2.0f)){
                s.on=false; sh.flash=3;
                if(--sh.hp<=0){ killShip(sh); grantAch(A_BLASTER); }
                break;
            }
        }
    }

    // ---- powerup capsules drift down; a flipper-side miss just expires -----
    for(int i=0;i<MAX_PU;i++){
        Powerup&p=pus[i];
        if(!p.on) continue;
        p.y+=p.vy;
        if(p.y>H-6) p.on=false;
    }

    // ---- balls -------------------------------------------------------------
    int live=0;
    for(int i=0;i<MAX_BALLS;i++){
        if(!balls[i].on) continue;
        stepBall(balls[i]);
        if(balls[i].on) live++;
    }
    if(live==0){
        loseLife("BALL LOST");
        if(!gameOver){
            for(int i=1;i<MAX_BALLS;i++) balls[i].on=false;
            resetBall(balls[0]);
        }
        return;
    }
    if(!gameOver && combo>0 && shipsAlive()>0) { /* combo still running */ }

    // ---- wave cleared? -----------------------------------------------------
    if(shipsAlive()==0){
        if(lives==3) grantAch(A_NOLOSS);
        grantAch(A_FIRSTKILL);
        nextWave();
    }
}

// ===========================================================================
//  DRAW
// ===========================================================================
static void drawShip(const Ship&s){
    uint16_t body = s.flash ? LIME_PALE
                  : (s.kind==2 ? LIME_MED : (s.kind==1 ? PURPLE_LIGHT : TEAL_BRIGHT));
    uint16_t trim = s.kind==2 ? PURPLE : (s.kind==1 ? PURPLE_BRIGHT : TEAL_LIGHT);
    int x=(int)s.x, y=(int)s.y;
    // bug-ship: winged carapace with a bright core and antennae
    fb.fillTriangle(x-7,y+3, x, y-5, x+7,y+3, body);
    fb.fillRoundRect(x-5,y+1,10,6,2,trim);
    fb.fillCircle(x,y+1,2,LIME_PALE);
    // wings sweep with the formation drift so the swarm feels alive
    int flap = ((frameNo/8)%2)?1:-1;
    fb.drawLine(x-7,y+2, x-11,y+5+flap, trim);
    fb.drawLine(x+7,y+2, x+11,y+5-flap, trim);
    // antennae
    fb.drawLine(x-2,y-4, x-5,y-8, trim);
    fb.drawLine(x+2,y-4, x+5,y-8, trim);
    if(s.kind==1 && s.hp>1) fb.drawCircle(x,y,SHIP_R,PURPLE_BRIGHT);   // armor ring
    if(s.kind==2) fb.drawCircle(x,y,SHIP_R+1,LIME_MED);                  // carrier halo
}
static void drawFlipperShape(const Flipper&f){
    float tipX,tipY; flipperEndpoints(f,tipX,tipY);
    uint16_t c = powerFlipFrames>0 ? LIME_MED : LIME;
    // thick line = the paddle, same look as the house table
    for(int o=-3;o<=3;o++)
        fb.drawLine((int)f.pivotX,(int)f.pivotY+o,(int)tipX,(int)tipY+o,c);
    fb.fillCircle((int)f.pivotX,(int)f.pivotY,4,LIME_MED);
    fb.fillCircle((int)tipX,(int)tipY,3,LIME_PALE);
}
static void drawBallShape(const Ball&b){
    if(!b.on) return;
    fb.fillCircle((int)b.x,(int)b.y,(int)BALL_R,LIME_PALE);
    fb.drawCircle((int)b.x,(int)b.y,(int)BALL_R,LIME_PALE);
    if(blasterFrames>0) fb.drawCircle((int)b.x,(int)b.y,(int)BALL_R+2,LIME_MED);
}

void mantisvadersDraw(){
    fb.fillSprite(TEAL_DEEP);
    // starfield, drifting down slowly
    for(int i=0;i<26;i++){
        int sx=(int)((i*61)%W);
        int sy=(int)(((i*97)+(frameNo/3))%(uint32_t)H);
        fb.drawPixel(sx,sy, (i%4)?TEAL_DARK:TEAL);
    }
    if(flashLevel>0){
        // hit flash across the field, like the house table's flash
        uint16_t c = flashLevel>0.4f?TEAL_DARK:TEAL_DEEP;
        fb.fillRect(0,26,W,(int)FUNNEL_TOP_Y-26,c);
    }

    // the breach line the swarm must not cross
    for(int x=4;x<W-4;x+=8) fb.drawPixel(x,(int)BREACH_Y,PURPLE_DARK);

    // funnels + rails (the house geometry, drawn so you can see the table)
    fb.drawLine(0,0,0,(int)FUNNEL_TOP_Y,TEAL);
    fb.drawLine(W-1,0,W-1,(int)FUNNEL_TOP_Y,TEAL);
    fb.drawLine((int)walls[3].x1,(int)walls[3].y1,(int)walls[3].x2,(int)walls[3].y2,TEAL_LIGHT);
    fb.drawLine((int)walls[4].x1,(int)walls[4].y1,(int)walls[4].x2,(int)walls[4].y2,TEAL_LIGHT);

    for(int i=0;i<shipCount;i++) if(ships[i].on) drawShip(ships[i]);
    for(int i=0;i<MAX_SHOTS;i++) if(shots[i].on)
        fb.drawLine((int)shots[i].x,(int)shots[i].y,(int)shots[i].x,(int)shots[i].y+3,LIME_MED);
    for(int i=0;i<MAX_PU;i++) if(pus[i].on){
        const Powerup&p=pus[i];
        uint16_t c = p.kind==0?LIME_MED : p.kind==1?LIME : p.kind==2?PURPLE_LIGHT : TEAL_BRIGHT;
        fb.fillRoundRect((int)p.x-6,(int)p.y-5,12,10,3,c);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DEEP);
        const char* g = p.kind==0?"B":p.kind==1?"M":p.kind==2?"F":"S";
        fb.drawString(g,(int)p.x,(int)p.y,1);
    }

    drawFlipperShape(flipL);
    drawFlipperShape(flipR);
    for(int i=0;i<MAX_BALLS;i++) drawBallShape(balls[i]);

    if(shieldFrames>0){
        // a shimmer across the drain gap
        uint16_t c=((frameNo/4)%2)?TEAL_BRIGHT:TEAL_LIGHT;
        fb.drawLine((int)FLIP_L_PX,(int)FLIP_Y+8,(int)FLIP_R_PX,(int)FLIP_Y+8,c);
    }

    // HUD
    fb.fillRect(0,0,W,24,PURPLE_DEEP);
    char b[26];
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"%lu",(unsigned long)score); fb.drawString(b,4,2,2);
    fb.setTextDatum(TR_DATUM); fb.setTextColor(TEAL_LIGHT);
    snprintf(b,sizeof(b),"WAVE %d",wave); fb.drawString(b,W-4,2,1);
    fb.setTextColor(LIME_MED);
    snprintf(b,sizeof(b),"BEST %lu",(unsigned long)mv.hiScore); fb.drawString(b,W-4,13,1);
    // life pips
    for(int i=0;i<3;i++)
        fb.fillCircle(60+i*10,10,3,(i<lives)?LIME:TEAL_DARK);
    if(combo>1){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
        snprintf(b,sizeof(b),"x%d",combo); fb.drawString(b,W/2,26,2);
    }
    if(toast[0]){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(toast,W/2,(int)FUNNEL_TOP_Y-8,2);
    }
    if(achQN>0){
        fb.fillRect(0,H-16,W,14,PURPLE_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        snprintf(b,sizeof(b),"MEDAL: %s",A_NAME[achQ[0]]);
        fb.drawString(b,W/2,H-9,1);
    }
    if(gameOver){
        fb.fillRect(8,110,W-16,96,TEAL_DEEP);
        fb.drawRect(8,110,W-16,96,PURPLE_LIGHT);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(PURPLE_LIGHT); fb.drawString("INVASION WINS",W/2,128,2);
        fb.setTextColor(LIME_PALE);
        snprintf(b,sizeof(b),"%lu",(unsigned long)score); fb.drawString(b,W/2,156,4);
        fb.setTextColor(TEAL_LIGHT);
        snprintf(b,sizeof(b),"wave %d   best %lu",wave,(unsigned long)mv.hiScore);
        fb.drawString(b,W/2,180,1);
        fb.setTextColor(TEAL_DARK); fb.drawString("press to play again",W/2,196,1);
    }
    fb.pushSprite(0,0);
}

// ===========================================================================
//  LIFECYCLE
// ===========================================================================
// ---- house rule: an app holds its working buffers ONLY while it is open ----
// Taken at the very start of Enter, freed at the very end of Shutdown, so a
// closed app costs no RAM and every other app (TLS especially) gets it back.
struct AppHeap_vaders {
    Ship ships[MAX_SHIPS];
    Wall walls[5];
    Shot shots[MAX_SHOTS];
};
static AppHeap_vaders* g_heap_vaders = nullptr;
static void heapUp_vaders(){
    if (!g_heap_vaders) g_heap_vaders = (AppHeap_vaders*)malloc(sizeof(AppHeap_vaders));
    if (!g_heap_vaders) return;
    memset(g_heap_vaders, 0, sizeof(AppHeap_vaders));
    ships = g_heap_vaders->ships;
    walls = g_heap_vaders->walls;
    shots = g_heap_vaders->shots;
}
static void heapDown_vaders(){
    free(g_heap_vaders); g_heap_vaders = nullptr;
    ships = nullptr;
    walls = nullptr;
    shots = nullptr;
}

void mantisvadersEnter(){
    heapUp_vaders();                       // first: everything below may use the buffers
    fb.setRotation(0);
    mvLoad();
    buildWalls();
    initFlippers();
    memset(shots,0,(sizeof(Shot)*(MAX_SHOTS)));
    memset(pus,0,sizeof(pus));
    for(int i=0;i<MAX_BALLS;i++) balls[i].on=false;
    resetBall(balls[0]);
    score=0; wave=1; lives=3; combo=0; comboCool=0;
    blasterFrames=0; shieldFrames=0; powerFlipFrames=0; blasterCooldown=0;
    gameOver=false; goT=0; frameNo=0; flashLevel=0; achQN=0; toast[0]='\0';
    rng=millis()|1u;
    buildWave(1);
}
void mantisvadersShutdown(){
    if(score>mv.hiScore){ mv.hiScore=score; mvDirty=true; }
    if((uint16_t)wave>mv.bestWave){ mv.bestWave=(uint16_t)wave; mvDirty=true; }
    mvFlush();
    heapDown_vaders();                     // last: after saves and radio teardown
}
