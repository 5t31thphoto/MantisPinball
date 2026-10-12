// FILE: src/party/pb_tictactoe.cpp
// TIC TAC TOE - two mantises fighting for territory. Each claimed square shows
// a mantis in that player's colors, and a claim plays a short strike animation.
// Uses pbgrid as a 3x3 cursor (L = move, R = claim).
// Move payload: data[0] = cell index 0..8.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
static constexpr int W=170, H=320;
static constexpr int TT_CELL=46;
static constexpr int TT_X=(W-3*TT_CELL)/2;   // 16
static constexpr int TT_Y=90;

static int8_t cell[9];
static int    strikeAt=-1; static uint32_t strikeT=0;
static int    winLine[3]; static bool haveWin=false;
static uint32_t animT=0;

static void ttStart(uint32_t s,int n,int seat){
    (void)s;(void)n;(void)seat;
    memset(cell,-1,sizeof(cell)); strikeAt=-1; haveWin=false; animT=0;
    pbgrid::begin(3,3);
}
static void ttApply(const PbMove* m){
    if(m->len<1) return;
    int i=m->data[0]; if(i<0||i>8||cell[i]>=0) return;
    cell[i]=(int8_t)m->player;
    strikeAt=i; strikeT=millis();
}
static void ttTick(){
    animT++;
    if(!pbMyTurn()) return;
    pbgrid::tick();
    if(pbgrid::selected()){
        int i=pbgrid::cy()*3+pbgrid::cx();
        if(i>=0&&i<9&&cell[i]<0){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=(uint8_t)i;
            pbSubmitMove(&mv);
        }
    }
}
static bool ttOver(int* winner){
    static const int L[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for(int i=0;i<8;i++){
        int a=L[i][0],b=L[i][1],c=L[i][2];
        if(cell[a]>=0&&cell[a]==cell[b]&&cell[b]==cell[c]){
            winLine[0]=a;winLine[1]=b;winLine[2]=c; haveWin=true;
            *winner=cell[a]; return true;
        }
    }
    for(int i=0;i<9;i++) if(cell[i]<0) return false;
    *winner=-1; return true;   // full board, draw
}
static void ttEnd(int w){ (void)w; }

static uint16_t seatCol(int s){ return s==0?LIME:PURPLE_LIGHT; }

// a small mantis, facing left or right depending on the seat
static void drawMantisIn(int cx,int cy,int seat,float wig){
    uint16_t c=seatCol(seat);
    int dir = seat==0?1:-1;
    int hx=cx+dir*6;
    fb.fillTriangle(hx-dir*4,cy-2, hx+dir*4,cy-6, hx+dir*4,cy+2, c);  // head
    fb.fillCircle(hx+dir*2,cy-4,1,TEAL_DEEP);                          // eye
    fb.fillRoundRect(cx-8,cy-2,16,7,2,c);                              // body
    int a=(int)(sinf(wig)*3);
    fb.drawLine(hx-dir*2,cy+1, hx+dir*6,cy+5+a, c);                    // raptor arms
    fb.drawLine(hx-dir*3,cy+2, hx+dir*4,cy+8-a, c);
    fb.drawLine(cx-2,cy+5, cx-6,cy+10, c);                             // legs
    fb.drawLine(cx+2,cy+5, cx+6,cy+10, c);
    fb.drawLine(hx-dir*4,cy-5, hx-dir*8,cy-9, c);                      // antenna
}

static void ttDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("TERRITORY",W/2,8,2);
    fb.setTextColor(pbMyTurn()?LIME:TEAL_LIGHT);
    fb.drawString(pbMyTurn()?"YOUR MOVE":pbPlayerName(pbWhoseTurn()),W/2,30,1);
    // the arena
    fb.fillRoundRect(TT_X-6,TT_Y-6,3*TT_CELL+12,3*TT_CELL+12,5,TEAL_DARK);
    for(int i=1;i<3;i++){
        fb.drawFastVLine(TT_X+i*TT_CELL,TT_Y,3*TT_CELL,TEAL);
        fb.drawFastHLine(TT_X,TT_Y+i*TT_CELL,3*TT_CELL,TEAL);
    }
    for(int i=0;i<9;i++){
        int cx=TT_X+(i%3)*TT_CELL+TT_CELL/2, cy=TT_Y+(i/3)*TT_CELL+TT_CELL/2;
        if(cell[i]>=0){
            float wig=(float)animT*0.15f + i;
            // a fresh claim flashes
            bool fresh = (strikeAt==i) && (millis()-strikeT<400);
            if(fresh) fb.fillRoundRect(TT_X+(i%3)*TT_CELL+2,TT_Y+(i/3)*TT_CELL+2,TT_CELL-4,TT_CELL-4,3,PURPLE_DARK);
            drawMantisIn(cx,cy,cell[i],wig);
        }
    }
    // cursor
    if(pbMyTurn()){
        int i=pbgrid::cy()*3+pbgrid::cx();
        int x=TT_X+(i%3)*TT_CELL, y=TT_Y+(i/3)*TT_CELL;
        uint16_t c=(animT/8)%2?LIME_PALE:seatCol(pbMySeat());
        fb.drawRoundRect(x+2,y+2,TT_CELL-4,TT_CELL-4,3,c);
    }
    // winning line
    if(haveWin) for(int k=0;k<3;k++){
        int i=winLine[k]; int x=TT_X+(i%3)*TT_CELL, y=TT_Y+(i/3)*TT_CELL;
        fb.drawRoundRect(x+1,y+1,TT_CELL-2,TT_CELL-2,3,LIME_PALE);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString(pbMyTurn()?"L: move   R: claim":"waiting...",W/2,H-6,1);
}

BoardGame TICTACTOE_GAME = {
    "MANTIS TERRITORY", 2, 2, 2,
    ttStart, ttTick, ttApply, ttDraw, ttOver, ttEnd
};
