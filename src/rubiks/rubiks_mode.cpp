// MantisCubeSolver — Mantis Crypto Pinball OS app.
//
// UI flow is unchanged from the version this replaces: face-by-face colour
// entry, a verify pass, long-press edit, then the solution walkthrough.
//
// What changed underneath:
//   * The solve starts the moment VERIFY is entered, on core 0, in the
//     background. By the time the user has flipped through six faces the
//     answer is usually already waiting, so perceived latency is zero.
//   * Sticker entry is validated with the parity laws before solving, and a
//     bad entry jumps the cursor to the offending square instead of hanging
//     the search on an unsolvable cube.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "rubiks_mode.h"
#include "../theme_colors.h"

#include "cube_core.h"
#include "cube_policy.h"
#include "cube_task.h"
#include "cube_endgame_api.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint8_t g_stickers[54];
static McCube g_cube;
static McSolverJob g_job;
static McValidation g_valid = MC_OK;

static const uint16_t g_pal[6] = {
    0xFFFF, 0xF800, 0x07E0, 0xFFE0, 0xFC00, 0x001F
};
static const char* g_colorName[6] = {"WHT","RED","GRN","YEL","ORG","BLU"};
static const char* g_faceName[6]  = {"UP","RIGHT","FRONT","DOWN","LEFT","BACK"};
static const char  g_moveFace[6]  = {'U','R','F','D','L','B'};

enum class State { INPUT, ORIENT, VERIFY, EDIT, SOLVING, SOLVE, ERROR, WIN };

static State g_state = State::INPUT;
static int g_face = 0, g_pos = 0, g_colorSel = 0;
static int g_verifyFace = 0, g_solIdx = 0;
static uint32_t g_lHold = 0;
static bool g_lHeld = false;
static float g_spin = 0;

// ---------------------------------------------------------------------------
// 3D preview
// ---------------------------------------------------------------------------

struct V3 { float x, y, z; };
struct V2 { float x, y; };

static V3 g_v[8];
static const int g_e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                               {0,4},{1,5},{2,6},{3,7}};
static const int g_fv[6][4] = {{3,2,6,7},{1,5,6,2},{5,4,7,6},
                               {0,1,5,4},{0,4,7,3},{1,0,3,2}};

static void initVerts() {
    const float s = 38.0f;
    g_v[0]={-s,-s,-s}; g_v[1]={s,-s,-s}; g_v[2]={s,s,-s};  g_v[3]={-s,s,-s};
    g_v[4]={-s,-s,s};  g_v[5]={s,-s,s};  g_v[6]={s,s,s};   g_v[7]={-s,s,s};
}

static V3 rY(V3 v, float a){ float c=cosf(a),s=sinf(a);
    return {v.x*c-v.z*s, v.y, v.x*s+v.z*c}; }
static V3 rX(V3 v, float a){ float c=cosf(a),s=sinf(a);
    return {v.x, v.y*c-v.z*s, v.y*s+v.z*c}; }
static V2 pr(V3 v, float cx, float cy){
    float z=v.z+200.0f; if(z<1) z=1;
    return {cx+v.x*230.0f/z, cy+v.y*230.0f/z}; }

static void drawCube(float rx, float ry, int hlFace, int hlPos, int cy) {
    V3 r[8];
    for (int i=0;i<8;i++){ r[i]=rY(g_v[i],ry); r[i]=rX(r[i],rx); }

    int ord[6]={0,1,2,3,4,5};
    float z[6];
    for(int f=0;f<6;f++){ z[f]=0; for(int j=0;j<4;j++) z[f]+=r[g_fv[f][j]].z; }
    for(int i=0;i<5;i++) for(int j=0;j<5-i;j++)
        if(z[ord[j]]>z[ord[j+1]]){ int t=ord[j];ord[j]=ord[j+1];ord[j+1]=t; }

    for(int fi=0;fi<6;fi++){
        int f=ord[fi];
        // Backface cull: skip faces pointing away, halving the sticker work.
        V2 p[4]; for(int j=0;j<4;j++) p[j]=pr(r[g_fv[f][j]],W/2,cy);
        float cross=(p[1].x-p[0].x)*(p[2].y-p[0].y)
                   -(p[1].y-p[0].y)*(p[2].x-p[0].x);
        if(cross<=0) continue;

        fb.fillTriangle((int)p[0].x,(int)p[0].y,(int)p[1].x,(int)p[1].y,
                        (int)p[2].x,(int)p[2].y,TEAL_DARK);
        fb.fillTriangle((int)p[0].x,(int)p[0].y,(int)p[2].x,(int)p[2].y,
                        (int)p[3].x,(int)p[3].y,TEAL_DARK);
        for(int s=0;s<9;s++){
            float u=(s%3)*0.33f+0.17f, v=(s/3)*0.33f+0.17f;
            V3 sp={
              r[g_fv[f][0]].x*(1-u)*(1-v)+r[g_fv[f][1]].x*u*(1-v)+
              r[g_fv[f][3]].x*(1-u)*v+r[g_fv[f][2]].x*u*v,
              r[g_fv[f][0]].y*(1-u)*(1-v)+r[g_fv[f][1]].y*u*(1-v)+
              r[g_fv[f][3]].y*(1-u)*v+r[g_fv[f][2]].y*u*v,
              r[g_fv[f][0]].z*(1-u)*(1-v)+r[g_fv[f][1]].z*u*(1-v)+
              r[g_fv[f][3]].z*(1-u)*v+r[g_fv[f][2]].z*u*v};
            V2 q=pr(sp,W/2,cy);
            int rad=3;
            if(f==hlFace && s==hlPos){ fb.fillCircle((int)q.x,(int)q.y,rad+2,LIME); rad=2; }
            fb.fillCircle((int)q.x,(int)q.y,rad,g_pal[g_stickers[f*9+s]]);
        }
    }
    for(int e=0;e<12;e++){
        V2 a=pr(r[g_e[e][0]],W/2,cy), b=pr(r[g_e[e][1]],W/2,cy);
        fb.drawLine((int)a.x,(int)a.y,(int)b.x,(int)b.y,TEAL_BRIGHT);
    }
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

static void hdr(const char* t){
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(t,W/2,2,2);
}

static void ftr(const char* l, const char* r){
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(l,2,H-12,1);
    fb.setTextDatum(TR_DATUM); fb.drawString(r,W-2,H-12,1);
}

static void progressBar(int y, float frac, uint16_t col){
    const int x=12, w=W-24, h=8;
    fb.drawRect(x,y,w,h,TEAL_BRIGHT);
    int fill=(int)(frac*(w-4));
    if(fill>0) fb.fillRect(x+2,y+2,fill,h-4,col);
}

static void colorBar(){
    const int y=245, cs=20, sx=(W-6*cs)/2;
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("PICK COLOR",W/2,y-12,1);
    for(int c=0;c<6;c++){
        int cx=sx+c*cs+cs/2;
        if(c==g_colorSel) fb.drawRect(cx-cs/2-2,y-cs/2-2,cs+4,cs+4,LIME);
        fb.fillRect(cx-cs/2,y-cs/2,cs-2,cs-2,g_pal[c]);
    }
    fb.setTextDatum(MC_DATUM); fb.setTextColor(g_pal[g_colorSel]);
    fb.drawString(g_colorName[g_colorSel],W/2,y+20,2);
}

static void flatFace(int face,int hl){
    const int sz=22, fx=(W-3*sz)/2, fy=200;
    for(int i=0;i<9;i++){
        int cx=fx+(i%3)*sz, cy=fy+(i/3)*sz;
        if(i==hl) fb.drawRect(cx-1,cy-1,sz+2,sz+2,LIME);
        fb.fillRect(cx,cy,sz-1,sz-1,g_pal[g_stickers[face*9+i]]);
    }
}

static void viewFor(int face,float& rx,float& ry){
    static const float v[6][2]={{0.9f,0},{0,1.57f},{0,0},
                                {-0.9f,0},{0,-1.57f},{0,3.14f}};
    rx=v[face][0]; ry=v[face][1];
}

// ---------------------------------------------------------------------------
// Solve kickoff
// ---------------------------------------------------------------------------

static void startSolve(){
    g_valid = mcFaceletsToCube(g_stickers, g_cube);
    if(g_valid != MC_OK){ g_state = State::ERROR; return; }

    McPolicyOptions o; mcDefaultPolicyOptions(o);
    // frames / depth / goodEnough come from mcDefaultPolicyOptions(), which
    // is tuned on the trained weights - don't override them here.
    o.symmetryDedup = true;
    mcSolveAsync(g_job, g_cube, o);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static void drawInput(){
    fb.fillSprite(TEAL_DEEP);
    char t[32]; snprintf(t,sizeof(t),"%s  %d/6",g_faceName[g_face],g_face+1);
    hdr(t);
    float rx,ry; viewFor(g_face,rx,ry);
    drawCube(rx,ry,g_face,g_pos,120);
    flatFace(g_face,g_pos);
    char p[16]; snprintf(p,sizeof(p),"SQUARE %d/9",g_pos+1);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(p,W/2,190,1);
    colorBar();
    ftr("L:Color","R:Set");
}

static void drawOrient(){
    fb.fillSprite(TEAL_DEEP);
    hdr("ROTATE CUBE");
    float rx,ry; viewFor(g_face,rx,ry);
    drawCube(rx,ry,-1,-1,130);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString("TURN TO",W/2,55,2);
    fb.drawString(g_faceName[g_face],W/2,78,2);
    ftr("","R:Ready");
}

static void drawVerify(){
    fb.fillSprite(TEAL_DEEP);
    hdr("VERIFY CUBE");
    float rx,ry; viewFor(g_verifyFace,rx,ry);
    drawCube(rx,ry,-1,-1,120);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString(g_faceName[g_verifyFace],W/2,190,2);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("Does this match?",W/2,210,1);
    fb.drawString("Hold L to edit",W/2,224,1);

    // The solve has been running since this screen opened.
    if(g_job.running){
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("solving in background",W/2,250,1);
        progressBar(262, mcProgressFraction(g_job), TEAL_BRIGHT);
    } else if(g_job.complete && g_job.solution.found){
        char b[32];
        snprintf(b,sizeof(b),"Solution ready: %d moves",g_job.solution.length);
        fb.setTextColor(LIME); fb.drawString(b,W/2,250,1);
    }
    ftr("L:Face","R:Solve!");
}

static void drawEdit(){
    fb.fillSprite(TEAL_DEEP);
    char t[32]; snprintf(t,sizeof(t),"EDIT %s",g_faceName[g_verifyFace]);
    hdr(t);
    float rx,ry; viewFor(g_verifyFace,rx,ry);
    drawCube(rx,ry,g_verifyFace,g_pos,120);
    flatFace(g_verifyFace,g_pos);
    colorBar();
    ftr("L:Color","R:Set");
}

static void drawSolving(){
    fb.fillSprite(TEAL_DEEP);
    hdr("SOLVING");
    g_spin += 0.05f; if(g_spin>6.28f) g_spin-=6.28f;
    drawCube(0.4f,g_spin,-1,-1,110);

    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString("THINKING",W/2,190,2);

    char b[40];
    snprintf(b,sizeof(b),"frame %d  depth %d",
             g_job.progress.phase, g_job.progress.ply);
    fb.setTextColor(TEAL_BRIGHT); fb.drawString(b,W/2,212,1);

    if(g_job.progress.bestLength){
        snprintf(b,sizeof(b),"best so far: %d moves",g_job.progress.bestLength);
        fb.setTextColor(LIME_PALE); fb.drawString(b,W/2,228,1);
    }
    snprintf(b,sizeof(b),"%lu states  %lu ms",
             (unsigned long)g_job.progress.statesSeen,
             (unsigned long)mcElapsedMs(g_job));
    fb.setTextColor(TEAL_LIGHT); fb.drawString(b,W/2,244,1);

    progressBar(262, mcProgressFraction(g_job), LIME);
    ftr("L:Cancel","");
}

static void drawSolve(){
    fb.fillSprite(TEAL_DEEP);
    hdr("SOLUTION");
    const McSolution& s = g_job.solution;
    if(g_solIdx >= s.length){ g_state = State::WIN; return; }

    int m = s.moves[g_solIdx];
    char face = g_moveFace[m/3];
    int kind = m%3;

    char mv[8];
    snprintf(mv,sizeof(mv),"%c%s",face,kind==1?"'":kind==2?"2":"");
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(mv,W/2,55,6);

    char inst[40];
    snprintf(inst,sizeof(inst),"Turn %s %s",g_faceName[m/3],
             kind==1?"counter-clockwise":kind==2?"180 degrees":"clockwise");
    fb.setTextColor(LIME); fb.drawString(inst,W/2,95,1);

    g_spin += 0.03f; if(g_spin>6.28f) g_spin-=6.28f;
    drawCube(0.35f,g_spin,-1,-1,170);

    char p[24]; snprintf(p,sizeof(p),"MOVE %d / %d",g_solIdx+1,s.length);
    fb.setTextColor(TEAL_BRIGHT); fb.drawString(p,W/2,250,2);
    progressBar(268,(float)(g_solIdx+1)/(float)s.length,LIME);
    ftr("L:Back","R:Next");
}

static void drawError(){
    fb.fillSprite(TEAL_DEEP);
    hdr("CHECK THE CUBE");
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME);

    // Word-wrapped, because these messages are the difference between a user
    // fixing one square and giving up.
    char buf[160];
    snprintf(buf,sizeof(buf),"%s",mcValidationMessage(g_valid));
    int y=40, cx=6;
    char* w = strtok(buf," ");
    while(w){
        int wl = fb.textWidth(w,1);
        if(cx+wl > W-6 && cx > 6){ cx=6; y+=13; }
        fb.drawString(w,cx,y,1);
        cx += wl+4;
        w = strtok(nullptr," ");
    }

    int bad = mcValidationFacelet();
    if(bad >= 0){
        char b[40];
        snprintf(b,sizeof(b),"Face %s, square %d",g_faceName[bad/9],bad%9+1);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
        fb.drawString(b,W/2,y+34,2);
    }
    ftr("L:Edit","R:Restart");
}

static void drawWin(){
    fb.fillSprite(TEAL_DEEP);
    hdr("SOLVED!");
    g_spin += 0.04f; if(g_spin>6.28f) g_spin-=6.28f;

    V3 r[8];
    for(int i=0;i<8;i++){ r[i]=rY(g_v[i],g_spin); r[i]=rX(r[i],g_spin*0.7f); }
    for(int f=0;f<6;f++){
        V2 p[4]; for(int j=0;j<4;j++) p[j]=pr(r[g_fv[f][j]],W/2,140);
        fb.fillTriangle((int)p[0].x,(int)p[0].y,(int)p[1].x,(int)p[1].y,
                        (int)p[2].x,(int)p[2].y,g_pal[f]);
        fb.fillTriangle((int)p[0].x,(int)p[0].y,(int)p[2].x,(int)p[2].y,
                        (int)p[3].x,(int)p[3].y,g_pal[f]);
    }
    for(int i=0;i<10;i++){
        int sx=W/2+(int)(sinf(g_spin*2+i)*60);
        int sy=140+(int)(cosf(g_spin*1.5f+i*0.7f)*50);
        fb.fillCircle(sx,sy,2,LIME);
    }
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString("CUBE SOLVED!",W/2,240,2);
    char b[32];
    snprintf(b,sizeof(b),"%d moves in %lu ms",g_job.solution.length,
             (unsigned long)mcElapsedMs(g_job));
    fb.setTextColor(TEAL_BRIGHT); fb.drawString(b,W/2,262,1);
    ftr("L:New","R:New");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void rubiksEnter(){
    fb.setRotation(0);
    // ~24 KB search arena, taken from the heap for as long as this app is open
    // and handed straight back on Shutdown. If the heap can't spare it right
    // now we show the error screen instead of dereferencing a null arena.
    if(!mcPolicyArenaAcquire()){
        initVerts();
        g_state=State::ERROR;
        return;
    }
    initVerts();
    for(int f=0;f<6;f++) for(int s=0;s<9;s++) g_stickers[f*9+s]=(uint8_t)f;
    g_state=State::INPUT;
    g_face=0; g_pos=0; g_colorSel=0; g_verifyFace=0; g_solIdx=0;
    g_lHeld=false; g_spin=0;
    g_job.running=false; g_job.complete=false;
    g_job.solution.found=false; g_job.solution.length=0;
}

void rubiksTick(){
    bool lEdge = btnL && !btnL_prev;
    bool rEdge = btnR && !btnR_prev;

    if(btnL && !btnL_prev){ g_lHold=millis(); g_lHeld=false; }
    if(btnL && (millis()-g_lHold > 800) && !g_lHeld){
        g_lHeld=true;
        if(g_state==State::VERIFY){
            mcCancelAsync(g_job);
            g_state=State::EDIT; g_pos=0;
            g_colorSel=g_stickers[g_verifyFace*9];
        }
    }

    switch(g_state){
    case State::INPUT:
        if(lEdge){ g_colorSel=(g_colorSel+1)%6; }
        if(rEdge){
            g_stickers[g_face*9+g_pos]=(uint8_t)g_colorSel;
            if(++g_pos>=9){
                g_pos=0;
                if(++g_face>=6){
                    g_verifyFace=0;
                    g_state=State::VERIFY;
                    startSolve();          // begins the moment entry finishes
                } else {
                    g_state=State::ORIENT;
                }
            }
        }
        break;

    case State::ORIENT:
        if(rEdge) g_state=State::INPUT;
        break;

    case State::VERIFY:
        if(lEdge) g_verifyFace=(g_verifyFace+1)%6;
        if(rEdge){
            if(g_job.complete && g_job.solution.found){
                g_solIdx=0; g_state=State::SOLVE;   // already done: no wait
            } else if(g_valid!=MC_OK){
                g_state=State::ERROR;
            } else {
                g_state=State::SOLVING;
            }
        }
        break;

    case State::EDIT:
        if(lEdge){ g_colorSel=(g_colorSel+1)%6; }
        if(rEdge){
            g_stickers[g_verifyFace*9+g_pos]=(uint8_t)g_colorSel;
            if(++g_pos>=9){
                g_pos=0;
                g_state=State::VERIFY;
                startSolve();               // re-solve with the correction
            }
        }
        break;

    case State::SOLVING:
        if(lEdge){ mcCancelAsync(g_job); g_state=State::VERIFY; }
        if(g_job.complete){
            if(g_job.solution.found){ g_solIdx=0; g_state=State::SOLVE; }
            else { g_valid=MC_ERR_PARITY; g_state=State::ERROR; }
        }
        break;

    case State::SOLVE:
        if(lEdge && g_solIdx>0) g_solIdx--;
        if(rEdge && ++g_solIdx >= g_job.solution.length) g_state=State::WIN;
        break;

    case State::ERROR:
        if(lEdge){
            int bad=mcValidationFacelet();
            g_verifyFace = bad>=0 ? bad/9 : 0;
            g_pos = bad>=0 ? bad%9 : 0;
            g_colorSel = g_stickers[g_verifyFace*9+g_pos];
            g_state=State::EDIT;
        }
        if(rEdge) rubiksEnter();
        break;

    case State::WIN:
        if(lEdge||rEdge) rubiksEnter();
        break;
    }
}

void rubiksDraw(){
    switch(g_state){
    case State::INPUT:   drawInput();   break;
    case State::ORIENT:  drawOrient();  break;
    case State::VERIFY:  drawVerify();  break;
    case State::EDIT:    drawEdit();    break;
    case State::SOLVING: drawSolving(); break;
    case State::SOLVE:   drawSolve();   break;
    case State::ERROR:   drawError();   break;
    case State::WIN:     drawWin();     break;
    }
    fb.pushSprite(0,0);
}

void rubiksShutdown(){
    mcCancelAsync(g_job);
    mcPolicyArenaRelease();   // give the 24 KB back to the other 25 apps
}
