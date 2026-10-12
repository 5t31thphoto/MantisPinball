// FILE: src/party/party_hub.cpp
// PARTY GAME hub - the board-stack chooser.
//
// Selecting "PARTY GAME" from the main menu lands here: a stack of board-game
// boxes you scroll through and pick from. CHARADES IS ONE OF THE SLOTS, sitting
// alongside the turn-based board games, which is how party mode was always meant
// to work - one modular chooser, many games.
//
// The box artwork is baked (src/party/party_sprites.h, three variants cycled
// down the stack so it looks hand-made) and each box carries a blank pale label
// plate; the game's name is drawn onto that plate at runtime, so adding a game
// needs no new art.
//
// Delegation:
//   * CHARADES  -> the existing charades framework (party_net.cpp), untouched.
//   * any other -> the board-game framework (party_board.cpp) with that game
//                  selected. When the user backs out (pbWantsExit) we return
//                  here to the stack.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "party_board.h"
#include "party_sprites.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

// the charades framework (party_net.cpp) - delegated to, never modified
extern void partyEnter();
extern void partyTick();
extern void partyDraw();
extern void partyShutdown();

static constexpr int W = 170, H = 320;
static constexpr int BOX_STEP = 40;      // vertical spacing in the stack
static constexpr int STACK_TOP = 52;
static constexpr int VISIBLE = 6;        // boxes drawn at once

enum HubMode : uint8_t { HM_STACK = 0, HM_CHARADES, HM_BOARD };
static HubMode mode = HM_STACK;
static int  sel = 0;                     // slot 0 = charades, 1.. = board games
static int  scroll = 0;
static uint32_t enterAt = 0;
static uint32_t animT = 0;
static constexpr uint32_t HUB_GUARD = 250;

static int slotCount(){ return 1 + pbGameCount(); }
static const char* slotName(int i){ return i==0 ? "CHARADES" : pbGameName(i-1); }

// ---- 4bpp blit, house convention (low nibble = even x, 0 = transparent) ----
static void pbBlit(const uint8_t* sp,int sw,int sh,int x,int y){
    int rowB=(sw+1)/2;
    for(int py=0;py<sh;py++){
        int dy=y+py; if(dy<0||dy>=H) continue;
        for(int px=0;px<sw;px++){
            uint8_t b=sp[py*rowB+px/2];
            uint8_t v=(px&1)?(b>>4):(b&0x0F);
            if(!v||v>PB_PAL_N) continue;
            int dx=x+px; if(dx<0||dx>=W) continue;
            fb.drawPixel(dx,dy,PB_PAL[v-1]);
        }
    }
}

void partyHubEnter(){
    fb.setRotation(0);
    mode=HM_STACK; sel=0; scroll=0; enterAt=millis(); animT=0;
}

void partyHubShutdown(){
    if(mode==HM_CHARADES) partyShutdown();
    else if(mode==HM_BOARD) partyBoardShutdown();
    mode=HM_STACK;
}

void partyHubTick(){
    animT++;
    if(mode==HM_CHARADES){ partyTick(); return; }
    if(mode==HM_BOARD){
        partyBoardTick();
        if(pbWantsExit()){            // user backed out of the game
            partyBoardShutdown();
            mode=HM_STACK; enterAt=millis();
        }
        return;
    }
    // ---- stack menu ----
    if(millis()-enterAt < HUB_GUARD) return;   // swallow the entering press
    if(btnL&&!btnL_prev){
        sel=(sel+1)%slotCount();
        if(sel<scroll) scroll=sel;
        if(sel>=scroll+VISIBLE) scroll=sel-VISIBLE+1;
        if(sel==0) scroll=0;
    }
    if(btnR&&!btnR_prev){
        if(sel==0){ mode=HM_CHARADES; partyEnter(); }
        else { pbSelectGame(sel-1); mode=HM_BOARD; partyBoardEnter(); }
    }
}

void partyHubDraw(){
    if(mode==HM_CHARADES){ partyDraw(); return; }
    if(mode==HM_BOARD){ partyBoardDraw(); return; }

    fb.fillSprite(TEAL_DEEP);
    // title
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("PARTY GAMES",W/2,8,2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("pick one off the stack",W/2,30,1);

    // the stack, drawn back-to-front so lower boxes tuck under the ones above
    int n=slotCount();
    int last=scroll+VISIBLE; if(last>n) last=n;
    for(int i=last-1;i>=scroll;i--){
        int row=i-scroll;
        bool isSel=(i==sel);
        // the selected box slides out to the right, like pulling it off the pile
        int nudge = isSel ? 8 + (int)((animT/10)%2) : 0;
        int bx = (W-PB_BOX0_W)/2 + nudge;
        int by = STACK_TOP + row*BOX_STEP;
        const uint8_t* art = PB_BOXES[i % PB_BOX_COUNT];
        pbBlit(art,PB_BOX0_W,PB_BOX0_H,bx,by);
        // the game's name, drawn onto the blank label plate
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(isSel?PURPLE_DEEP:TEAL_DEEP);
        fb.drawString(slotName(i),
                      bx+PB_LABEL_X+PB_LABEL_W/2,
                      by+PB_LABEL_Y+PB_LABEL_H/2, 1);
        // selection glow around the pulled-out box
        if(isSel){
            fb.drawRoundRect(bx-2,by,PB_BOX0_W+4,PB_BOX0_H-10,5,LIME);
            fb.fillTriangle(bx-10,by+14, bx-4,by+18, bx-10,by+22, LIME);
        }
    }
    // scroll hint when the stack runs past the screen
    if(n>VISIBLE){
        fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
        char b[20]; snprintf(b,sizeof(b),"%d / %d",sel+1,n);
        fb.drawString(b,W/2,H-18,1);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("L: next   R: play",W/2,H-4,1);
    fb.pushSprite(0,0);
}
