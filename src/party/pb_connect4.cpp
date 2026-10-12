// FILE: src/party/pb_connect4.cpp
// CONNECT FOUR for the board-game framework.
// Uses the shared pbgrid module as a column selector (L = move, R = drop).
// Move payload: data[0] = column.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;

static constexpr int W=170, H=320;
static constexpr int C4_COLS=7, C4_ROWS=6;
static constexpr int C4_CELL=22;
static constexpr int C4_X=(W-C4_COLS*C4_CELL)/2;     // 170-154 -> 8
static constexpr int C4_Y=70;

static int8_t board[C4_ROWS][C4_COLS];   // -1 empty, else seat index
static int    lastCol=-1, lastRow=-1;
static int    winA[4][2]; static bool haveWin=false;

static void c4Start(uint32_t seed,int n,int seat){
    (void)seed;(void)n;(void)seat;
    memset(board,-1,sizeof(board));
    lastCol=lastRow=-1; haveWin=false;
    pbgrid::begin(C4_COLS,1);
}

static int dropRow(int col){
    for(int r=C4_ROWS-1;r>=0;r--) if(board[r][col]<0) return r;
    return -1;
}

static void c4Apply(const PbMove* m){
    if(m->len<1) return;
    int col=m->data[0]; if(col<0||col>=C4_COLS) return;
    int r=dropRow(col); if(r<0) return;
    board[r][col]=(int8_t)m->player;
    lastCol=col; lastRow=r;
}

static void c4Tick(){
    if(!pbMyTurn()) return;
    pbgrid::tick();
    if(pbgrid::selected()){
        int col=pbgrid::cx();
        if(dropRow(col)>=0){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=(uint8_t)col;
            pbSubmitMove(&mv);
        }
    }
}

// 4-in-a-row from the last drop, in all four directions
static bool checkWin(int seat){
    const int dirs[4][2]={{0,1},{1,0},{1,1},{1,-1}};
    for(int r=0;r<C4_ROWS;r++)for(int c=0;c<C4_COLS;c++){
        if(board[r][c]!=seat) continue;
        for(int d=0;d<4;d++){
            int dr=dirs[d][0], dc=dirs[d][1], k=1;
            while(k<4){
                int nr=r+dr*k, nc=c+dc*k;
                if(nr<0||nr>=C4_ROWS||nc<0||nc>=C4_COLS||board[nr][nc]!=seat) break;
                k++;
            }
            if(k>=4){
                for(int i=0;i<4;i++){ winA[i][0]=r+dr*i; winA[i][1]=c+dc*i; }
                haveWin=true; return true;
            }
        }
    }
    return false;
}

static bool c4Over(int* winner){
    for(int s=0;s<pbPlayerCount();s++) if(checkWin(s)){ *winner=s; return true; }
    bool full=true;
    for(int c=0;c<C4_COLS;c++) if(board[0][c]<0){ full=false; break; }
    if(full){ *winner=-1; return true; }
    return false;
}
static void c4End(int w){ (void)w; }

static uint16_t seatCol(int s){ return s==0?LIME:PURPLE_LIGHT; }

static void c4Draw(){
    fb.fillSprite(TEAL_DEEP);
    // header: whose turn
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(LIME_PALE); fb.drawString("CONNECT FOUR",W/2,8,2);
    fb.setTextColor(pbMyTurn()?LIME:TEAL_LIGHT);
    fb.drawString(pbMyTurn()?"YOUR TURN":pbPlayerName(pbWhoseTurn()),W/2,30,1);
    // the column cursor marker
    if(pbMyTurn()){
        int cxp=C4_X+pbgrid::cx()*C4_CELL+C4_CELL/2;
        fb.fillTriangle(cxp-6,48,cxp+6,48,cxp,58,seatCol(pbMySeat()));
    }
    // the grid
    fb.fillRoundRect(C4_X-4,C4_Y-4,C4_COLS*C4_CELL+8,C4_ROWS*C4_CELL+8,4,TEAL_DARK);
    fb.drawRoundRect(C4_X-4,C4_Y-4,C4_COLS*C4_CELL+8,C4_ROWS*C4_CELL+8,4,TEAL_LIGHT);
    for(int r=0;r<C4_ROWS;r++)for(int c=0;c<C4_COLS;c++){
        int x=C4_X+c*C4_CELL+C4_CELL/2, y=C4_Y+r*C4_CELL+C4_CELL/2;
        int8_t v=board[r][c];
        if(v<0) fb.fillCircle(x,y,C4_CELL/2-3,TEAL_DEEP);
        else    fb.fillCircle(x,y,C4_CELL/2-3,seatCol(v));
        fb.drawCircle(x,y,C4_CELL/2-3,TEAL);
    }
    // highlight the winning line
    if(haveWin) for(int i=0;i<4;i++){
        int x=C4_X+winA[i][1]*C4_CELL+C4_CELL/2, y=C4_Y+winA[i][0]*C4_CELL+C4_CELL/2;
        fb.drawCircle(x,y,C4_CELL/2-2,LIME_PALE);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString(pbMyTurn()?"L: move   R: drop":"waiting...",W/2,H-6,1);
}

BoardGame CONNECT4_GAME = {
    "CONNECT FOUR", 1, 2, 2,
    c4Start, c4Tick, c4Apply, c4Draw, c4Over, c4End
};
