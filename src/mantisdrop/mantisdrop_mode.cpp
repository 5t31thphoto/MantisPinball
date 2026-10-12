// FILE: src/mantisdrop/mantisdrop_mode.cpp
// MANTIS DROP - a praying-mantis Tetris x Dr. Mario x Bejeweled stacker.
//
// Standard 7 tetromino shapes fall on an 8x16 well. Every CELL of a piece is
// independently colored from three families (TEAL / PURPLE / LIME), with the
// single rule that a freshly spawned piece never contains 3+ contiguous
// same-color cells (at most two touching) - so matches are made by stacking on
// the board, not handed to you.
//
// CLEARING, per the spec:
//   * a full ROW clears (Tetris), AND
//   * any CONTIGUOUS same-color blob of 3+ cells clears (Dr. Mario / Bejeweled),
//     where contiguous = 4-connected (up/down/left/right), any shape.
// After a clear, everything above falls (gravity re-settle) and we re-scan,
// so clears CASCADE into chains with a climbing combo multiplier.
//
// BOMBS are colorless wildcards that come both as rare cells inside pieces and
// as combo rewards. A bomb detonates when:
//   * a color blob or row touching it clears (it goes off with whatever clears
//     next to it, as if it shared that color), or
//   * it is caught in another bomb's blast.
// So bombs CHAIN. Three bomb types, each a different blast: RADIUS (3x3),
// CROSS (full row + column), COLOR (every cell of one color on the board).
//
// House conventions: xEnter/xTick/xDraw/xShutdown into the shared fb; edge
// input (L = move/rotate scroll, R = drop/confirm, long-press hard drop);
// theme colors only; ASCII fonts; event-driven XP (never per-point) + medals;
// flash written only on discrete events + shutdown (dirty flag).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <Preferences.h>
#include "mantisdrop_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ---- board -----------------------------------------------------------------
static constexpr int COLS = 8;
static constexpr int ROWS = 16;
static constexpr int CELL = 18;                 // 8*18 = 144 wide
static constexpr int BOARD_X = (W - COLS*CELL)/2;   // centered: (170-144)/2 = 13
static constexpr int BOARD_Y = 30;

// A cell: empty, or one of three colors, optionally a bomb of some type.
enum Color : uint8_t { C_NONE=0, C_TEAL=1, C_PURPLE=2, C_LIME=3 };
enum Bomb  : uint8_t { B_NONE=0, B_RADIUS=1, B_CROSS=2, B_COLOR=3 };
struct Cell { uint8_t color; uint8_t bomb; };   // color 0 = empty
static Cell grid[ROWS][COLS];

static inline bool inb(int r,int c){ return r>=0 && r<ROWS && c>=0 && c<COLS; }
static inline bool filled(int r,int c){ return inb(r,c) && grid[r][c].color!=C_NONE; }

// ---- pieces (standard tetromino shapes, each a list of 4 offsets) ----------
// Rotations generated at runtime so the data can't be malformed (the draft's
// hand-typed rotation bitmasks were 6/7 wrong). We store the base shape and
// rotate offsets about a pivot.
struct PieceDef { int8_t cells[4][2]; uint16_t col; const char* name; };
// coordinates are (row, col) with the piece's own local origin
static const PieceDef PIECES[7] = {
    {{{0,-1},{0,0},{0,1},{0,2}}, 0, "I"},
    {{{0,0},{0,1},{1,0},{1,1}}, 0, "O"},
    {{{0,-1},{0,0},{0,1},{1,0}}, 0, "T"},
    {{{0,0},{0,1},{1,-1},{1,0}}, 0, "S"},
    {{{0,-1},{0,0},{1,0},{1,1}}, 0, "Z"},
    {{{0,-1},{0,0},{0,1},{1,-1}}, 0, "J"},
    {{{0,-1},{0,0},{0,1},{1,1}}, 0, "L"},
};

// The active falling piece: 4 cells, each with its own color/bomb.
struct Active {
    int shape;
    int rot;            // 0..3
    int r, c;           // origin on the board
    uint8_t color[4];   // per-cell color
    uint8_t bomb[4];    // per-cell bomb type (mostly B_NONE)
};
static Active cur, nextP;

// rotate a local (dr,dc) offset by rot quarter-turns about the origin
static void rotOff(int dr,int dc,int rot,int&or_,int&oc){
    for(int i=0;i<(rot&3);i++){ int nr=dc, nc=-dr; dr=nr; dc=nc; }
    or_=dr; oc=dc;
}
// world cell positions of the active piece into out[4][2]
static void pieceCells(const Active&a,int out[4][2]){
    for(int i=0;i<4;i++){
        int dr,dc; rotOff(PIECES[a.shape].cells[i][0],PIECES[a.shape].cells[i][1],a.rot,dr,dc);
        out[i][0]=a.r+dr; out[i][1]=a.c+dc;
    }
}
static bool fits(const Active&a){
    int cc[4][2]; pieceCells(a,cc);
    for(int i=0;i<4;i++){
        int r=cc[i][0], c=cc[i][1];
        if(c<0||c>=COLS||r>=ROWS) return false;
        if(r>=0 && grid[r][c].color!=C_NONE) return false;
    }
    return true;
}

// ---- rng -------------------------------------------------------------------
static uint32_t rng=1;
static uint32_t pr(){ rng=rng*1664525u+1013904223u; return rng>>8; }
static int prange(int a,int b){ return b<=a?a:a+(int)(pr()%(uint32_t)(b-a+1)); }

// ---- run state -------------------------------------------------------------
static uint32_t score=0;
static int      lines=0, level=1;
static int      combo=0;
static bool     gameOver=false;
static uint32_t dropTimer=0, dropInterval=700;
static uint32_t frameNo=0;
static int      bombsFromCombo=0;   // pending combo-reward bombs to inject

// achievements
enum { ACH_FIRST=0, ACH_CHAIN3, ACH_BOMB, ACH_COLORCLEAR, ACH_LINE4,
       ACH_LVL5, ACH_COMBO5, ACH_SCORE10K, ACH_COUNT_ };
static uint8_t achMask=0;
static uint8_t achQ[4]; static int achQN=0;

// ---- color assignment with the "no 3 contiguous in a piece" rule -----------
static uint8_t randColor(){ return (uint8_t)prange(1,3); }
// assign colors to the 4 active cells so no 3 of them that are mutually
// adjacent (within the piece) share a color. Simple + safe: retry a few times.
static void colorActive(Active&a){
    int cc[4][2]; 
    // temp place at origin to know adjacency
    Active t=a; t.r=0; t.c=2; pieceCells(t,cc);
    for(int tries=0; tries<20; tries++){
        for(int i=0;i<4;i++) a.color[i]=randColor();
        // check: no cell has 2+ same-color piece-neighbors (that would make 3 contiguous)
        bool bad=false;
        for(int i=0;i<4 && !bad;i++){
            int same=0;
            for(int j=0;j<4;j++) if(j!=i && a.color[j]==a.color[i]){
                int dr=cc[i][0]-cc[j][0], dc=cc[i][1]-cc[j][1];
                if(abs(dr)+abs(dc)==1) same++;
            }
            if(same>=2) bad=true;   // i plus two same-color neighbors = 3 contiguous
        }
        if(!bad) break;
    }
    // bombs: rare per-cell (~4%); at most one per piece
    for(int i=0;i<4;i++) a.bomb[i]=B_NONE;
    if(prange(0,99)<4){
        int i=prange(0,3);
        a.bomb[i]=(uint8_t)prange(1,3);
    }
    // inject a pending combo-reward bomb if owed
    if(bombsFromCombo>0){
        int i=prange(0,3); a.bomb[i]=(uint8_t)prange(1,3);
        bombsFromCombo--;
    }
}

static void spawnPiece(){
    cur = nextP;
    cur.shape = nextP.shape;
    cur.rot=0; cur.r=0; cur.c=COLS/2;
    // build the *next* preview piece now
    nextP.shape=prange(0,6); nextP.rot=0;
    colorActive(nextP);
    if(!fits(cur)) gameOver=true;
}

static void lockPiece(){
    int cc[4][2]; pieceCells(cur,cc);
    for(int i=0;i<4;i++){
        int r=cc[i][0], c=cc[i][1];
        if(inb(r,c)){ grid[r][c].color=cur.color[i]; grid[r][c].bomb=cur.bomb[i]; }
    }
}

// ---- the cascade resolver --------------------------------------------------
// mark[r][c]=1 means "this cell clears this settle". We compute all clears
// (rows + color blobs), then let bombs chain, then remove + gravity, then
// repeat while anything cleared - each pass is one cascade step (combo++).
static uint8_t mark[ROWS][COLS];

static void markColorBlobs(){
    static uint8_t seen[ROWS][COLS];
    memset(seen,0,sizeof(seen));
    int stackR[ROWS*COLS], stackC[ROWS*COLS];
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
        if(seen[r][c]||grid[r][c].color==C_NONE) continue;
        uint8_t col=grid[r][c].color;
        // flood-fill this 4-connected same-color blob
        int n=0, sp=0; int cellsR[ROWS*COLS], cellsC[ROWS*COLS];
        stackR[sp]=r; stackC[sp]=c; sp++; seen[r][c]=1;
        while(sp>0){
            sp--; int cr=stackR[sp], cc2=stackC[sp];
            cellsR[n]=cr; cellsC[n]=cc2; n++;
            const int dr[4]={-1,1,0,0}, dc[4]={0,0,-1,1};
            for(int k=0;k<4;k++){
                int nr=cr+dr[k], nc=cc2+dc[k];
                if(inb(nr,nc)&&!seen[nr][nc]&&grid[nr][nc].color==col){
                    seen[nr][nc]=1; stackR[sp]=nr; stackC[sp]=nc; sp++;
                }
            }
        }
        if(n>=3) for(int i=0;i<n;i++) mark[cellsR[i]][cellsC[i]]=1;   // 3+ contiguous -> clear
    }
}
static void markFullRows(){
    for(int r=0;r<ROWS;r++){
        bool full=true;
        for(int c=0;c<COLS;c++) if(grid[r][c].color==C_NONE){ full=false; break; }
        if(full) for(int c=0;c<COLS;c++) mark[r][c]=1;
    }
}
// A bomb detonates if it is marked, or if any 4-neighbor is marked (wildcard).
// Detonation stamps its blast into mark, which can catch more bombs -> chain.
static bool detonateBombs(){
    bool any=false;
    bool again=true;
    while(again){
        again=false;
        for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
            if(grid[r][c].bomb==B_NONE) continue;
            if(mark[r][c]==2) continue;                 // already fired
            bool touch = mark[r][c]==1;
            if(!touch){
                const int dr[4]={-1,1,0,0}, dc[4]={0,0,-1,1};
                for(int k=0;k<4 && !touch;k++){ int nr=r+dr[k],nc=c+dc[k]; if(inb(nr,nc)&&mark[nr][nc])touch=true; }
            }
            if(!touch) continue;
            // FIRE
            mark[r][c]=2; any=true; again=true;
            uint8_t bt=grid[r][c].bomb;
            if(bt==B_RADIUS){
                for(int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++){ int nr=r+dr,nc=c+dc; if(inb(nr,nc)&&grid[nr][nc].color!=C_NONE&&mark[nr][nc]==0) mark[nr][nc]=1; }
            } else if(bt==B_CROSS){
                for(int c2=0;c2<COLS;c2++) if(grid[r][c2].color!=C_NONE&&mark[r][c2]==0) mark[r][c2]=1;
                for(int r2=0;r2<ROWS;r2++) if(grid[r2][c].color!=C_NONE&&mark[r2][c]==0) mark[r2][c]=1;
            } else if(bt==B_COLOR){
                // clear every cell of the most common neighboring color; if the
                // bomb was in a row clear with no color context, pick globally.
                uint8_t tc=C_NONE;
                const int dr[4]={-1,1,0,0}, dc[4]={0,0,-1,1};
                for(int k=0;k<4&&tc==C_NONE;k++){ int nr=r+dr[k],nc=c+dc[k]; if(inb(nr,nc)&&grid[nr][nc].color!=C_NONE) tc=grid[nr][nc].color; }
                if(tc==C_NONE) tc=(uint8_t)prange(1,3);
                for(int r2=0;r2<ROWS;r2++)for(int c2=0;c2<COLS;c2++) if(grid[r2][c2].color==tc&&mark[r2][c2]==0) mark[r2][c2]=1;
            }
        }
    }
    return any;
}
static int removeMarked(){
    int cleared=0;
    for(int r=0;r<ROWS;r++)for(int c=0;c<COLS;c++) if(mark[r][c]){ grid[r][c].color=C_NONE; grid[r][c].bomb=B_NONE; cleared++; }
    return cleared;
}
static void applyGravity(){
    for(int c=0;c<COLS;c++){
        int write=ROWS-1;
        for(int r=ROWS-1;r>=0;r--) if(grid[r][c].color!=C_NONE){
            if(write!=r){ grid[write][c]=grid[r][c]; grid[r][c].color=C_NONE; grid[r][c].bomb=B_NONE; }
            write--;
        }
    }
}

static void grantAch(int a);
// Resolve all cascades after a lock. Returns total cells cleared.
static int resolveBoard(){
    int totalCleared=0; int step=0;
    while(true){
        memset(mark,0,sizeof(mark));
        markFullRows();
        markColorBlobs();
        bool bombFired = detonateBombs();
        // count marks
        bool anyMark=false;
        for(int r=0;r<ROWS&&!anyMark;r++)for(int c=0;c<COLS;c++) if(mark[r][c]){ anyMark=true; break; }
        if(!anyMark) break;
        if(bombFired) grantAch(ACH_BOMB);
        int cleared=removeMarked();
        totalCleared+=cleared;
        step++;
        combo=step;
        // scoring: base per cell * cascade step (combo multiplier)
        score += (uint32_t)cleared * 10u * (uint32_t)step;
        if(step>=3) grantAch(ACH_CHAIN3);
        if(step>=5) grantAch(ACH_COMBO5);
        applyGravity();
        // big cascades award a combo bomb on a future piece
        if(step>=3 && cleared>=6) bombsFromCombo++;
    }
    if(totalCleared>0){
        grantAch(ACH_FIRST);
        lines += totalCleared/COLS;   // rough "lines" tally for level curve
        int newLevel = 1 + lines/10;
        if(newLevel>level){ level=newLevel; if(level>=5) grantAch(ACH_LVL5);
            dropInterval = dropInterval>180 ? dropInterval-70 : 180; }
        if(score>=10000) grantAch(ACH_SCORE10K);
    }
    combo=0;
    return totalCleared;
}

// ---- persistence (dirty flag, event + shutdown writes only) ----------------
static const char* NVS_MD="mdrop";
struct __attribute__((packed)) MdSave { uint16_t magic; uint8_t version; uint8_t ach; uint32_t hiScore; uint16_t hiLines; };
static MdSave md; static bool mdDirty=false;
static void mdLoad(){ memset(&md,0,sizeof(md)); md.magic=0x4D44; md.version=1;
    Preferences p; if(!p.begin(NVS_MD,true))return; MdSave t; size_t n=p.getBytes("blob",&t,sizeof(t)); p.end();
    if(n==sizeof(t)&&t.magic==0x4D44&&t.version==1){ md=t; achMask=md.ach; } }
static void mdFlush(){ if(!mdDirty)return; md.ach=achMask; Preferences p; if(!p.begin(NVS_MD,false))return; p.putBytes("blob",&md,sizeof(md)); p.end(); mdDirty=false; }

static const char* ACH_NAME[ACH_COUNT_]={
    "FIRST CLEAR","CHAIN x3","DEMOLITION","COLOR BLOB","TETRIS",
    "LEVEL 5","COMBO x5","10K CLUB"};
static const uint8_t ACH_XP[ACH_COUNT_]={10,25,20,15,30,30,40,60};
static void grantAch(int a){
    if((achMask>>a)&1) return;
    achMask|=(uint8_t)(1<<a); qpAddXp(ACH_XP[a]); mdDirty=true;
    if(achQN<4) achQ[achQN++]=(uint8_t)a;
}

// ---- input (house edge idiom; long-press = hard drop) ----------------------
static constexpr uint32_t MD_LONG_MS=300, MD_ENTER_GUARD=250;
static uint32_t enterAt=0;
static bool lTap,rTap,lHold,rHold;
static bool lDown,rDown,lLongDone,rLongDone; static uint32_t lAt,rAt;
static void mdInput(){
    lTap=rTap=lHold=rHold=false; uint32_t now=millis();
    if(btnL&&!btnL_prev){lDown=true;lAt=now;lLongDone=false;}
    if(btnL&&lDown&&!lLongDone&&now-lAt>MD_LONG_MS){lLongDone=true;lHold=true;}
    if(!btnL&&btnL_prev){ if(lDown&&!lLongDone)lTap=true; lDown=false; }
    if(btnR&&!btnR_prev){rDown=true;rAt=now;rLongDone=false;}
    if(btnR&&rDown&&!rLongDone&&now-rAt>MD_LONG_MS){rLongDone=true;rHold=true;}
    if(!btnR&&btnR_prev){ if(rDown&&!rLongDone)rTap=true; rDown=false; }
    if(now-enterAt<MD_ENTER_GUARD){lTap=rTap=lHold=rHold=false;}
}

// controls: L tap = move left-cycling? No - we need move + rotate on two btns.
// Convention here: L tap = move left (wrap by re-tap), R tap = rotate CW,
// L long = move right (alternate), R long = hard drop. Soft drop is automatic
// via the interval. This keeps everything on two buttons and matches the
// "L navigate / R select" spirit (R acts on the piece).
static void tryMove(int dc){ Active t=cur; t.c+=dc; if(fits(t)) cur=t; }
static void tryRotate(){ Active t=cur; t.rot=(t.rot+1)&3; if(fits(t)){cur=t;return;}
    // wall kicks: try nudging in/out
    for(int k=-2;k<=2;k++){ Active u=t; u.c+=k; if(fits(u)){cur=u;return;} } }
static void softDrop(){ Active t=cur; t.r+=1; if(fits(t)) cur=t; else { lockPiece(); resolveBoard(); mdDirty=true; spawnPiece(); } }
static void hardDrop(){ while(true){ Active t=cur; t.r+=1; if(fits(t)) cur=t; else break; } lockPiece(); resolveBoard(); mdDirty=true; spawnPiece(); }

// ---- mascot (frantic eye-candy mantis, no gameplay effect) -----------------
static float mantisX=0, mantisPhase=0;
static void drawMantis(){
    mantisPhase += 0.35f;
    int mx = BOARD_X + (int)((sinf(mantisPhase*0.5f)*0.5f+0.5f)*(COLS*CELL-16));
    int my = BOARD_Y - 14 + (int)(sinf(mantisPhase)*2);
    uint16_t g = LIME;
    fb.fillTriangle(mx,my+6, mx+6,my, mx+12,my+6, g);      // head
    fb.fillCircle(mx+3,my+4,1,TEAL_DEEP); fb.fillCircle(mx+9,my+4,1,TEAL_DEEP);
    fb.drawLine(mx+2,my, mx-2,my-4, g);                    // antennae
    fb.drawLine(mx+10,my, mx+14,my-4, g);
    // raptorial arms waving
    int aw=(int)(sinf(mantisPhase*1.7f)*4);
    fb.drawLine(mx+2,my+6, mx-4,my+10+aw, LIME_PALE);
    fb.drawLine(mx+10,my+6, mx+16,my+10-aw, LIME_PALE);
}

// ---- draw ------------------------------------------------------------------
static uint16_t colOf(uint8_t c){ switch(c){case C_TEAL:return TEAL_BRIGHT;case C_PURPLE:return PURPLE_BRIGHT;case C_LIME:return LIME;default:return TEAL_DEEP;} }
static uint16_t colDim(uint8_t c){ switch(c){case C_TEAL:return TEAL;case C_PURPLE:return PURPLE_DARK;case C_LIME:return LIME_MED;default:return TEAL_DEEP;} }
static void drawCellAt(int r,int c,uint8_t color,uint8_t bomb){
    int x=BOARD_X+c*CELL, y=BOARD_Y+r*CELL;
    fb.fillRoundRect(x+1,y+1,CELL-2,CELL-2,3,colOf(color));
    fb.drawRoundRect(x+1,y+1,CELL-2,CELL-2,3,colDim(color));
    if(bomb){
        // bomb glyph varies by type
        uint16_t bc=LIME_PALE;
        if(bomb==B_RADIUS){ fb.drawCircle(x+CELL/2,y+CELL/2,4,bc); fb.fillCircle(x+CELL/2,y+CELL/2,1,bc); }
        else if(bomb==B_CROSS){ fb.drawLine(x+3,y+CELL/2,x+CELL-3,y+CELL/2,bc); fb.drawLine(x+CELL/2,y+3,x+CELL/2,y+CELL-3,bc); }
        else { fb.drawRect(x+4,y+4,CELL-8,CELL-8,bc); fb.drawPixel(x+CELL/2,y+CELL/2,bc); }
    }
}
static void drawBoard(){
    fb.fillRect(BOARD_X-2,BOARD_Y-2,COLS*CELL+4,ROWS*CELL+4,TEAL_DARK);
    fb.drawRect(BOARD_X-2,BOARD_Y-2,COLS*CELL+4,ROWS*CELL+4,TEAL_LIGHT);
    for(int r=0;r<ROWS;r++)for(int c=0;c<COLS;c++) if(grid[r][c].color!=C_NONE) drawCellAt(r,c,grid[r][c].color,grid[r][c].bomb);
    // ghost + active
    int cc[4][2]; pieceCells(cur,cc);
    Active g=cur; while(true){ Active t=g; t.r++; if(fits(t)) g=t; else break; }
    int gc[4][2]; pieceCells(g,gc);
    for(int i=0;i<4;i++){ int r=gc[i][0],c=gc[i][1]; if(inb(r,c)){ int x=BOARD_X+c*CELL,y=BOARD_Y+r*CELL; fb.drawRoundRect(x+1,y+1,CELL-2,CELL-2,3,colDim(cur.color[i])); } }
    for(int i=0;i<4;i++){ int r=cc[i][0],c=cc[i][1]; if(r>=0&&inb(r,c)) drawCellAt(r,c,cur.color[i],cur.bomb[i]); }
}
static void drawHud(){
    char b[24];
    fb.fillRect(0,0,W,28,PURPLE_DEEP);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"%lu",(unsigned long)score); fb.drawString(b,4,2,2);
    fb.setTextColor(TEAL_LIGHT); snprintf(b,sizeof(b),"LV%d",level); fb.drawString(b,4,16,1);
    // next preview
    fb.setTextDatum(TR_DATUM); fb.setTextColor(TEAL_DARK); fb.drawString("NEXT",W-4,2,1);
    Active np=nextP; np.r=0; np.c=0;
    for(int i=0;i<4;i++){ int dr,dc; rotOff(PIECES[np.shape].cells[i][0],PIECES[np.shape].cells[i][1],0,dr,dc);
        int x=W-40+dc*8, y=14+dr*8; fb.fillRoundRect(x,y,7,7,1,colOf(np.color[i])); }
    if(combo>1){ fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME); snprintf(b,sizeof(b),"COMBO x%d",combo); fb.drawString(b,W/2,H-12,2); }
}
static int goT=0;
static void drawGameOver(){
    fb.fillRect(BOARD_X-2,H/2-40,COLS*CELL+4,80,TEAL_DEEP);
    fb.drawRect(BOARD_X-2,H/2-40,COLS*CELL+4,80,PURPLE_LIGHT);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(PURPLE_LIGHT);
    fb.drawString("GAME OVER",W/2,H/2-22,2);
    char b[24]; fb.setTextColor(LIME_PALE);
    snprintf(b,sizeof(b),"%lu",(unsigned long)score); fb.drawString(b,W/2,H/2,4);
    fb.setTextColor(TEAL_LIGHT);
    bool best=score>=md.hiScore;
    snprintf(b,sizeof(b),"BEST %lu",(unsigned long)md.hiScore); fb.drawString(b,W/2,H/2+20,1);
    if(best){ fb.setTextColor(LIME); fb.drawString("NEW BEST!",W/2,H/2+32,1); }
}

void mantisdropDraw(){
    fb.fillSprite(TEAL_DEEP);
    drawBoard();
    drawMantis();
    drawHud();
    if(gameOver) drawGameOver();
    // achievement toast
    if(achQN>0){ fb.fillRect(0,H-30,W,16,PURPLE_DARK); fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        char b[28]; snprintf(b,sizeof(b),"MEDAL: %s",ACH_NAME[achQ[0]]); fb.drawString(b,W/2,H-22,1); }
    fb.pushSprite(0,0);
}

// ---- tick ------------------------------------------------------------------
void mantisdropTick(){
    mdInput(); frameNo++;
    if(gameOver){
        if(++goT>30 && (lTap||rTap)){ mdFlush(); mantisdropEnter(); }
        return;
    }
    // controls
    if(lTap) tryMove(-1);
    if(rTap) tryRotate();
    if(lHold) tryMove(+1);
    if(rHold) hardDrop();
    // gravity tick
    if(millis()-dropTimer>dropInterval){ dropTimer=millis(); softDrop(); }
    // decay achievement toast
    if(achQN>0 && (frameNo%90)==0){ for(int i=1;i<achQN;i++) achQ[i-1]=achQ[i]; achQN--; }
    // a lock may have ended the game; bank the score once, immediately
    if(gameOver){
        if(score>md.hiScore){ md.hiScore=score; mdDirty=true; }
        if((uint16_t)lines>md.hiLines){ md.hiLines=(uint16_t)lines; mdDirty=true; }
        mdFlush(); goT=0;
    }
}

void mantisdropEnter(){
    fb.setRotation(0);
    mdLoad();
    memset(grid,0,sizeof(grid));
    score=0; lines=0; level=1; combo=0; gameOver=false;
    dropInterval=700; dropTimer=millis(); enterAt=millis(); frameNo=0;
    lDown=rDown=false; lLongDone=rLongDone=false; achQN=0; goT=0; bombsFromCombo=0;
    rng=millis()|1;
    // prime next + current
    nextP.shape=prange(0,6); colorActive(nextP);
    spawnPiece();     // moves nextP->cur and rolls a fresh nextP
}
void mantisdropShutdown(){
    if(score>md.hiScore){ md.hiScore=score; mdDirty=true; }
    if((uint16_t)lines>md.hiLines){ md.hiLines=(uint16_t)lines; mdDirty=true; }
    mdFlush();
}
