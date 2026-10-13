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
#include "cube3d.h"

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

static const uint16_t g_pal[7] = {
    0xFFFF, 0xF800, 0x07E0, 0xFFE0, 0xFC00, 0x001F,
    0x6B4D                                   // 6 = not entered yet (grey)
};
static constexpr uint8_t UNSET = 6;
static const char* g_colorName[6] = {"WHT","RED","GRN","YEL","ORG","BLU"};
static const char* g_faceName[6]  = {"UP","RIGHT","FRONT","DOWN","LEFT","BACK"};
static const char  g_moveFace[6]  = {'U','R','F','D','L','B'};

enum class State { ENTRY, ORIENT, VERIFY, EDIT, SOLVING, SOLVE, ERROR, WIN };

static State g_state = State::ENTRY;
static int g_face = 0, g_pos = 0, g_colorSel = 0;
static int g_verifyFace = 0, g_solIdx = 0;
static uint32_t g_lHold = 0;
static bool g_lHeld = false;
static float g_spin = 0;

// ---------------------------------------------------------------------------
// 3D preview
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 3D cube - drawn exactly as the solver sees it (cube3d.h, proven against
// mcApplyMove by tools/tests/test_cube3d.cpp)
// ---------------------------------------------------------------------------
static C3 rotX(C3 p, float a){ float c=cosf(a), s=sinf(a); return {p.x, p.y*c - p.z*s, p.y*s + p.z*c}; }
static C3 rotY(C3 p, float a){ float c=cosf(a), s=sinf(a); return {p.x*c + p.z*s, p.y, -p.x*s + p.z*c}; }
static const float DEG = 0.01745329f;

struct CubeView { float holdRx, holdRy, tiltX, tiltY; int cx, cy; float K; };
static C3 viewXf(const CubeView& v, C3 p){
    p = rotY(p, v.holdRy * DEG); p = rotX(p, v.holdRx * DEG);     // how you hold the cube
    p = rotY(p, v.tiltY);        p = rotX(p, v.tiltX);            // 3/4 view: see top + side too
    return p;
}
static void proj(const CubeView& v, C3 p, int& x, int& y){
    const float D = 9.0f, k = v.K / (D - p.z);
    x = v.cx + (int)(p.x * k); y = v.cy - (int)(p.y * k);
}
static void quadFill(const int* x, const int* y, uint16_t c){
    fb.fillTriangle(x[0],y[0],x[1],y[1],x[2],y[2],c);
    fb.fillTriangle(x[0],y[0],x[2],y[2],x[3],y[3],c);
}
// st = 54 sticker colours; animMove/animA turn one layer part-way; hl = highlighted facelet
static void drawCube3D(const uint8_t* st, const CubeView& v, int animMove, float animA, int hl, bool blink){
    struct Q { float z; int16_t x[4], y[4], bx[4], by[4]; uint8_t c; bool hl; };
    static Q q[54]; int n = 0;
    for (int i = 0; i < 54; i++) {
        C3 body[4], stk[4];
        mcFaceletQuad(i, 0.0f, body); mcFaceletQuad(i, 0.07f, stk);
        const int8_t* nn = MC_FACE_FRAME[i / 9][0];
        C3 nrm = {(float)nn[0], (float)nn[1], (float)nn[2]};
        if (animMove >= 0 && mcInTurnLayer(i, animMove)) {
            for (int k = 0; k < 4; k++) { body[k] = mcRotAboutFace(body[k], animMove / 3, animA);
                                          stk[k]  = mcRotAboutFace(stk[k],  animMove / 3, animA); }
            nrm = mcRotAboutFace(nrm, animMove / 3, animA);
        }
        nrm = viewXf(v, nrm);
        if (nrm.z <= 0.05f) continue;                       // facing away: hidden
        Q& o = q[n++]; o.z = 0; o.c = st[i]; o.hl = (i == hl);
        for (int k = 0; k < 4; k++) {
            C3 pb = viewXf(v, body[k]), ps = viewXf(v, stk[k]);
            int x, y; proj(v, pb, x, y); o.bx[k] = x; o.by[k] = y;
            proj(v, ps, x, y); o.x[k] = x; o.y[k] = y; o.z += pb.z;
        }
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && q[j-1].z > q[j].z; j--) { Q t = q[j]; q[j] = q[j-1]; q[j-1] = t; }
    for (int i = 0; i < n; i++) {                           // far to near
        int bx[4], by[4], sx[4], sy[4];
        for (int k = 0; k < 4; k++) { bx[k] = q[i].bx[k]; by[k] = q[i].by[k]; sx[k] = q[i].x[k]; sy[k] = q[i].y[k]; }
        quadFill(bx, by, 0x0000);                           // the black plastic
        quadFill(sx, sy, g_pal[q[i].c > UNSET ? UNSET : q[i].c]);
        if (q[i].hl && blink)
            for (int k = 0; k < 4; k++) { int k2 = (k + 1) & 3;
                fb.drawLine(sx[k], sy[k], sx[k2], sy[k2], LIME); fb.drawLine(sx[k]+1, sy[k], sx[k2]+1, sy[k2], LIME); }
    }
}

// Entry order you follow with the cube in your hands. Hold: WHITE on top,
// GREEN facing you. Faces: F, R, B, L (turning the whole cube LEFT each time),
// then U (tip the top toward you), then D (turn the cube over).
static const int   PHYS_FACE[6]  = { 2, 1, 5, 4, 0, 3 };
static const float HOLD[6][2]    = { {0,0}, {0,-90}, {0,-180}, {0,-270}, {90,0}, {-90,0} };
static const char* STEP_TITLE[6] = { "FRONT - GREEN", "RIGHT - RED", "BACK - BLUE",
                                     "LEFT - ORANGE", "TOP - WHITE", "BOTTOM - YELLOW" };
static const char* STEP_HOW[6]   = {
    "Hold your cube with WHITE on top and GREEN facing you.",
    "Turn the whole cube to the LEFT. RED now faces you, WHITE still on top.",
    "Turn it LEFT again. BLUE faces you.",
    "Turn it LEFT again. ORANGE faces you.",
    "Turn it LEFT once more so GREEN faces you, then tip the TOP toward you. WHITE faces you, BLUE on top.",
    "Turn the cube over so YELLOW faces you, with GREEN on top." };
static const int ENTRY_POS[8] = { 0, 1, 2, 3, 5, 6, 7, 8 };   // the centre is fixed by the solver
static int physOf(int face){ for (int k = 0; k < 6; k++) if (PHYS_FACE[k] == face) return k; return 0; }

static uint32_t g_animT0 = 0;
static int      g_step = 0;            // 0..5 in PHYS order
static int      g_entIdx = 0;          // 0..7 into ENTRY_POS (centre is fixed)
static uint8_t  g_solveSt[54];         // stickers before the current solution move
static int      g_solveStIdx = -1;

static float ease(float t){ t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

static void wrapText(const char* s, int y, uint16_t col){
    char buf[160]; snprintf(buf, sizeof(buf), "%s", s);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(col);
    char line[40]; int ln = 0; line[0] = 0;
    for (char* w = strtok(buf, " "); w; w = strtok(nullptr, " ")) {
        char trial[48]; snprintf(trial, sizeof(trial), ln ? "%s %s" : "%s%s", line, w);
        if (fb.textWidth(trial, 2) > W - 8 && ln) { fb.drawString(line, W/2, y, 2); y += 17; snprintf(line, sizeof(line), "%s", w); }
        else snprintf(line, sizeof(line), "%s", trial);
        ln = 1;
    }
    if (ln) fb.drawString(line, W/2, y, 2);
}

static void flatFace(int face, int hl, bool blink){
    const int sz = 26, fx = (W - 3*sz)/2, fy = 186;
    for (int i = 0; i < 9; i++) {
        int x = fx + (i%3)*sz, y = fy + (i/3)*sz;
        fb.fillRect(x, y, sz-2, sz-2, g_pal[g_stickers[face*9+i]]);
        if (i == 4) fb.drawCircle(x + sz/2 - 1, y + sz/2 - 1, 3, 0x0000);     // fixed centre
        if (i == hl && blink) { fb.drawRect(x-2, y-2, sz+2, sz+2, LIME); fb.drawRect(x-1, y-1, sz, sz, LIME); }
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
    const int y=278, cs=22, sx=(W-6*cs)/2;
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("L: next colour",W/2,y-12,1);
    for(int c=0;c<6;c++){
        int cx=sx+c*cs+cs/2;
        if(c==g_colorSel) fb.drawRect(cx-cs/2-2,y-cs/2-2,cs+4,cs+4,LIME);
        fb.fillRect(cx-cs/2,y-cs/2,cs-2,cs-2,g_pal[c]);
    }

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

static CubeView holdView(int step, int cy, float K){
    return { HOLD[step][0], HOLD[step][1], 0.40f, -0.45f, W/2, cy, K };
}

static void drawInput(){
    fb.fillSprite(TEAL_DEEP);
    char t[32]; snprintf(t, sizeof(t), "%d/6  %s", g_step + 1, STEP_TITLE[g_step]);
    hdr(t);
    const bool blink = (millis() / 300) % 2;
    // the cube the way you're holding it: this face toward you, square 1 top-left
    drawCube3D(g_stickers, holdView(g_step, 96, 300.0f), -1, 0, g_face*9 + g_pos, blink);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    char p[32]; snprintf(p, sizeof(p), "square %d of 9  (top-left first)", g_pos + 1);
    fb.drawString(p, W/2, 172, 1);
    flatFace(g_face, g_pos, blink);
    colorBar();
    ftr("L:colour  hold L:back", "R:set");
}

// the whole-cube turn you make between faces, played on a loop
static void drawOrient(){
    fb.fillSprite(TEAL_DEEP);
    char t[32]; snprintf(t, sizeof(t), "NEXT: %s", STEP_TITLE[g_step]);
    hdr(t);
    const uint32_t e = (millis() - g_animT0) % 2600;
    float rx = HOLD[g_step][0], ry = HOLD[g_step][1];
    if (g_step > 0) {
        const float pr = HOLD[g_step-1][0], py = HOLD[g_step-1][1];
        if (g_step == 4) {                       // turn left once more, then tip the top toward you
            float t1 = ease((e - 200) / 800.0f), t2 = ease((e - 1100) / 800.0f);
            ry = py + (-360.0f - py) * t1;  rx = pr + (90.0f - pr) * t2;
        } else if (g_step == 5) {                // turn it over: top-facing -> bottom-facing
            float t1 = ease((e - 200) / 1400.0f);
            ry = 0; rx = 90.0f + (-90.0f - 90.0f) * t1;
        } else {                                 // turn the whole cube LEFT
            float t1 = ease((e - 200) / 1200.0f);
            ry = py + (HOLD[g_step][1] - py) * t1; rx = 0;
        }
    }
    CubeView v = { rx, ry, 0.40f, -0.45f, W/2, 120, 300.0f };
    drawCube3D(g_stickers, v, -1, 0, -1, false);
    wrapText(STEP_HOW[g_step], 205, LIME);
    ftr("", "R: ready");
}

static void drawVerify(){
    fb.fillSprite(TEAL_DEEP);
    const int step = physOf(g_verifyFace);
    char t[32]; snprintf(t, sizeof(t), "CHECK %s", STEP_TITLE[step]);
    hdr(t);
    drawCube3D(g_stickers, holdView(step, 110, 300.0f), -1, 0, -1, false);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("Hold your cube this way.", W/2, 196, 1);
    fb.drawString("Does it match?  Hold L to fix.", W/2, 210, 1);
    if(g_job.running){
        fb.setTextColor(TEAL_LIGHT); fb.drawString("solving in background", W/2, 240, 1);
        progressBar(252, mcProgressFraction(g_job), TEAL_BRIGHT);
    } else if(g_job.complete && g_job.solution.found){
        char b[32]; snprintf(b, sizeof(b), "Solution ready: %d moves", g_job.solution.length);
        fb.setTextColor(LIME); fb.drawString(b, W/2, 244, 1);
    }
    ftr("L:next face", "R:solve!");
}

static void drawEdit(){
    fb.fillSprite(TEAL_DEEP);
    const int step = physOf(g_verifyFace);
    char t[32]; snprintf(t, sizeof(t), "FIX %s", STEP_TITLE[step]);
    hdr(t);
    const bool blink = (millis() / 300) % 2;
    drawCube3D(g_stickers, holdView(step, 96, 300.0f), -1, 0, g_verifyFace*9 + g_pos, blink);
    flatFace(g_verifyFace, g_pos, blink);
    colorBar();
    ftr("L:colour", "R:set");
}

static void drawSolving(){
    fb.fillSprite(TEAL_DEEP);
    hdr("SOLVING");
    g_spin += 0.04f; if(g_spin > 6.28f) g_spin -= 6.28f;
    CubeView v = { 0, g_spin / DEG, 0.40f, -0.45f, W/2, 110, 280.0f };
    drawCube3D(g_stickers, v, -1, 0, -1, false);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString("THINKING", W/2, 190, 2);
    char b[40];
    snprintf(b, sizeof(b), "frame %d  depth %d", g_job.progress.phase, g_job.progress.ply);
    fb.setTextColor(TEAL_BRIGHT); fb.drawString(b, W/2, 212, 1);
    if(g_job.progress.bestLength){
        snprintf(b, sizeof(b), "best so far: %d moves", g_job.progress.bestLength);
        fb.setTextColor(LIME_PALE); fb.drawString(b, W/2, 228, 1);
    }
    snprintf(b, sizeof(b), "%lu states  %lu ms", (unsigned long)g_job.progress.statesSeen,
             (unsigned long)mcElapsedMs(g_job));
    fb.setTextColor(TEAL_LIGHT); fb.drawString(b, W/2, 244, 1);
    progressBar(262, mcProgressFraction(g_job), LIME);
    ftr("L:cancel", "");
}

// Plain words for every move, derived from the verified geometry while you
// hold WHITE on top and GREEN facing you (see tools/tests/test_cube3d.cpp).
static const char* MOVE_WORDS[6][3] = {
    { "TOP layer: front row goes LEFT",      "TOP layer: front row goes RIGHT",     "TOP layer: turn it twice" },
    { "RIGHT side: front column goes UP",    "RIGHT side: front column goes DOWN",  "RIGHT side: turn it twice" },
    { "FRONT face: turn it clockwise",       "FRONT face: turn it counter-clockwise","FRONT face: turn it twice" },
    { "BOTTOM layer: front row goes RIGHT",  "BOTTOM layer: front row goes LEFT",   "BOTTOM layer: turn it twice" },
    { "LEFT side: front column goes DOWN",   "LEFT side: front column goes UP",     "LEFT side: turn it twice" },
    { "BACK layer: top row goes LEFT",       "BACK layer: top row goes RIGHT",      "BACK layer: turn it twice" },
};
// camera per moving face: always WHITE on top / GREEN in front, angled so the
// turning layer is in view
static const float SOLVE_CAM[6][2] = { {0.40f,-0.45f}, {0.40f,-0.45f}, {0.40f,-0.45f},
                                       {-0.45f,-0.45f}, {0.40f,0.45f}, {1.05f,-0.45f} };

static void drawSolve(){
    fb.fillSprite(TEAL_DEEP);
    const McSolution& s = g_job.solution;
    if(g_solIdx >= s.length){ g_state = State::WIN; return; }
    if (g_solveStIdx != g_solIdx) {                      // stickers before this move
        McCube c = g_cube;
        for (int i = 0; i < g_solIdx; i++) mcApplyMove(c, s.moves[i]);
        mcCubeToFacelets(c, g_solveSt);
        g_solveStIdx = g_solIdx; g_animT0 = millis();
    }
    const int m = s.moves[g_solIdx], face = m / 3, kind = m % 3;
    char t[32]; snprintf(t, sizeof(t), "MOVE %d / %d", g_solIdx + 1, s.length); hdr(t);
    // turn, pause, show the turned result, repeat
    const uint32_t e = (millis() - g_animT0) % 2800;
    const float turn = ease((e - 300) / (kind == 2 ? 1300.0f : 900.0f));
    CubeView v = { 0, 0, SOLVE_CAM[face][0], SOLVE_CAM[face][1], W/2, 112, 300.0f };
    drawCube3D(g_solveSt, v, m, mcTurnAngle(m) * turn, -1, false);
    char mv[6]; snprintf(mv, sizeof(mv), "%c%s", g_moveFace[face], kind == 1 ? "'" : kind == 2 ? "2" : "");
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(mv, W/2, 200, 4);
    wrapText(MOVE_WORDS[face][kind], 218, LIME);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("keep WHITE on top, GREEN facing you", W/2, 262, 1);
    progressBar(276, (float)(g_solIdx + 1) / (float)s.length, LIME);
    ftr("L:back", "R:done, next");
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
    g_spin += 0.04f; if(g_spin > 6.28f) g_spin -= 6.28f;
    uint8_t solved[54]; for (int i = 0; i < 54; i++) solved[i] = (uint8_t)(i / 9);
    CubeView v = { g_spin * 20.0f, g_spin / DEG, 0.40f, -0.45f, W/2, 130, 300.0f };
    drawCube3D(solved, v, -1, 0, -1, false);
    for(int i = 0; i < 10; i++){
        int sx = W/2 + (int)(sinf(g_spin*2 + i)*62), sy = 130 + (int)(cosf(g_spin*1.5f + i*0.7f)*58);
        fb.fillCircle(sx, sy, 2, LIME);
    }
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
    fb.drawString("CUBE SOLVED!", W/2, 240, 2);
    char b[32];
    snprintf(b, sizeof(b), "%d moves", g_job.solution.length);
    fb.setTextColor(TEAL_BRIGHT); fb.drawString(b, W/2, 262, 1);
    ftr("L:new", "R:new");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void rubiksEnter(){
    fb.setRotation(0);
    // ~24 KB search arena, taken from the heap for as long as this app is open
    // and handed straight back on Shutdown. If the heap can't spare it right
    // now we show the error screen instead of dereferencing a null arena.
    if(!mcPolicyArenaAcquire()){ g_state = State::ERROR; return; }
    // a BLANK cube: grey squares, centres fixed (the solver reads colours by centre)
    for (int i = 0; i < 54; i++) g_stickers[i] = (i % 9 == 4) ? (uint8_t)(i / 9) : UNSET;
    g_step = 0; g_face = PHYS_FACE[0]; g_entIdx = 0; g_pos = ENTRY_POS[0];
    g_colorSel = 2;                                     // green: the front face comes first
    g_verifyFace = PHYS_FACE[0]; g_solIdx = 0; g_solveStIdx = -1;
    g_lHeld = false; g_spin = 0; g_animT0 = millis();
    g_state = State::ORIENT;                            // "hold WHITE on top, GREEN facing you"
    g_job.running=false; g_job.complete=false;
    g_job.solution.found=false; g_job.solution.length=0;
}

static void beginFace(){ g_face = PHYS_FACE[g_step]; g_entIdx = 0; g_pos = ENTRY_POS[0]; g_state = State::ENTRY; }

void rubiksTick(){
    const uint32_t now = millis();
    const bool rEdge = btnR && !btnR_prev;
    // L: a tap counts on RELEASE; a hold (600 ms) is its own action and never a tap
    static bool lDown = false, lLong = false; static uint32_t lAt = 0;
    bool lTap = false, lHoldFired = false;
    if (btnL && !btnL_prev) { lDown = true; lLong = false; lAt = now; }
    if (lDown && btnL && !lLong && now - lAt > 600) { lLong = true; lHoldFired = true; }
    if (lDown && !btnL) { if (!lLong) lTap = true; lDown = false; }

    switch(g_state){
    case State::ORIENT:
        if (rEdge) beginFace();
        break;

    case State::ENTRY:
        if (lTap) g_colorSel = (g_colorSel + 1) % 6;
        if (lHoldFired) {                                   // step back one square
            if (g_entIdx > 0) { g_entIdx--; }
            else if (g_step > 0) { g_step--; g_face = PHYS_FACE[g_step]; g_entIdx = 7; }
            g_pos = ENTRY_POS[g_entIdx];
            g_stickers[g_face*9 + g_pos] = UNSET;
        }
        if (rEdge) {
            g_stickers[g_face*9 + g_pos] = (uint8_t)g_colorSel;
            if (++g_entIdx >= 8) {
                if (g_step >= 5) {
                    g_verifyFace = PHYS_FACE[0];
                    g_state = State::VERIFY;
                    startSolve();                           // begins the moment entry finishes
                } else {
                    g_step++; g_animT0 = now; g_state = State::ORIENT;
                }
            } else g_pos = ENTRY_POS[g_entIdx];
        }
        break;

    case State::VERIFY:
        if (lTap) g_verifyFace = PHYS_FACE[(physOf(g_verifyFace) + 1) % 6];
        if (lHoldFired) {
            mcCancelAsync(g_job);
            g_state = State::EDIT; g_entIdx = 0; g_pos = ENTRY_POS[0];
            g_colorSel = g_stickers[g_verifyFace*9 + g_pos] < 6 ? g_stickers[g_verifyFace*9 + g_pos] : 0;
        }
        if (rEdge) {
            if (g_job.complete && g_job.solution.found) { g_solIdx = 0; g_solveStIdx = -1; g_state = State::SOLVE; }
            else if (g_valid != MC_OK) g_state = State::ERROR;
            else g_state = State::SOLVING;
        }
        break;

    case State::EDIT:
        if (lTap) g_colorSel = (g_colorSel + 1) % 6;
        if (rEdge) {
            g_stickers[g_verifyFace*9 + g_pos] = (uint8_t)g_colorSel;
            if (++g_entIdx >= 8) { g_state = State::VERIFY; startSolve(); }   // re-solve with the fix
            else { g_pos = ENTRY_POS[g_entIdx];
                   g_colorSel = g_stickers[g_verifyFace*9 + g_pos] < 6 ? g_stickers[g_verifyFace*9 + g_pos] : g_colorSel; }
        }
        break;

    case State::SOLVING:
        if (lTap) { mcCancelAsync(g_job); g_state = State::VERIFY; }
        if (g_job.complete) {
            if (g_job.solution.found) { g_solIdx = 0; g_solveStIdx = -1; g_state = State::SOLVE; }
            else { g_valid = MC_ERR_PARITY; g_state = State::ERROR; }
        }
        break;

    case State::SOLVE:
        if (lTap && g_solIdx > 0) g_solIdx--;
        if (rEdge && ++g_solIdx >= g_job.solution.length) g_state = State::WIN;
        break;

    case State::ERROR:
        if (lTap) {
            int bad = mcValidationFacelet();
            g_verifyFace = bad >= 0 ? bad / 9 : PHYS_FACE[0];
            int pos = bad >= 0 ? bad % 9 : 0;
            g_entIdx = 0; for (int k = 0; k < 8; k++) if (ENTRY_POS[k] == pos) g_entIdx = k;
            g_pos = ENTRY_POS[g_entIdx];
            g_colorSel = g_stickers[g_verifyFace*9 + g_pos] < 6 ? g_stickers[g_verifyFace*9 + g_pos] : 0;
            g_state = State::EDIT;
        }
        if (rEdge) rubiksEnter();
        break;

    case State::WIN:
        if (lTap || rEdge) rubiksEnter();
        break;
    }
}

void rubiksDraw(){
    switch(g_state){
    case State::ENTRY:   drawInput();   break;
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
