// FILE: src/party/pb_hangman.cpp
// SIX-GUN MANTIS (Hangman) - cowboy-mantis themed hmWord guessing.
//
// One player sets a hmWord using the shared on-screen keyboard (pbkbd: L walks
// the keys, R selects, '<' backspaces, '#' enters). Then the other players take
// turns guessing letters with the same keyboard. Instead of a gallows, a
// cowboy mantis faces a six-shooter: every wrong guess loads another chamber,
// and six wrong guesses ends the round.
//
// The hmWord is hidden from the guessers - it is sent to nobody. Each guessed
// letter is broadcast as a move, and every device reveals matching positions
// from its own copy of the masked hmWord, so the setter's device is the only one
// that ever holds the answer.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W=170, H=320;
static constexpr int HM_MAX_WORD=12;
static constexpr int HM_MISSES=6;      // six chambers

enum : uint8_t { OP_WORD=1, OP_GUESS=2 };

static char  hmWord[HM_MAX_WORD+1];      // setter's device only (and length elsewhere)
static int   wordLen=0;
static char  shown[HM_MAX_WORD+1];     // '_' or the revealed letter, all devices
static bool  guessed[26];
static int   misses=0;
static int   setterSeat=0;
static bool  settingPhase=true;
static bool  roundOver=false, guessersWon=false;
static uint32_t animT=0;

static void hmStart(uint32_t sd,int n,int seat){
    (void)sd;(void)n;(void)seat;
    memset(hmWord,0,sizeof(hmWord)); memset(shown,0,sizeof(shown));
    memset(guessed,0,sizeof(guessed));
    wordLen=0; misses=0; settingPhase=true; roundOver=false; guessersWon=false;
    setterSeat=0;                       // seat 0 sets the first hmWord
    pbkbd::begin(HM_MAX_WORD);
}

static bool amSetter(){ return pbMySeat()==setterSeat; }

static void hmApply(const PbMove* m){
    if(m->len<1) return;
    switch(m->data[0]){
    case OP_WORD: {
        // the setter publishes only the LENGTH; the letters stay on its device
        if(m->len>=2){
            wordLen=m->data[1]; if(wordLen>HM_MAX_WORD) wordLen=HM_MAX_WORD;
            for(int i=0;i<wordLen;i++) shown[i]='_';
            shown[wordLen]='\0';
            settingPhase=false;
            pbkbd::begin(1);            // guessers now pick single letters
        }
        break; }
    case OP_GUESS: {
        // data[1] = letter, data[2] = bitmask of matching positions (from the
        // setter's device, which is the only one holding the hmWord)
        if(m->len<3) break;
        char c=(char)m->data[1];
        int idx=c-'A'; if(idx>=0&&idx<26) guessed[idx]=true;
        uint16_t mask=(uint16_t)m->data[2] | ((m->len>3)?((uint16_t)m->data[3]<<8):0);
        // Over the link the guesser can't know if it matched, so it sends an
        // empty mask; the SETTER's device (the only one holding the hmWord)
        // resolves it and re-announces with the real positions.
        if(mask==0 && m->len==3 && amSetter() && hmWord[0]){
            uint16_t real=0;
            for(int i=0;i<wordLen&&i<16;i++) if(hmWord[i]==c) real|=(1u<<i);
            PbMove ans; memset(&ans,0,sizeof(ans));
            ans.player=m->player; ans.len=4;
            ans.data[0]=OP_GUESS; ans.data[1]=(uint8_t)c;
            ans.data[2]=(uint8_t)(real&0xFF); ans.data[3]=(uint8_t)(real>>8);
            pbBroadcastInfo(&ans);
            mask=real;                 // apply it here too
        }
        if(mask==0){ if(misses<HM_MISSES) misses++; }
        else for(int i=0;i<wordLen&&i<16;i++) if(mask&(1u<<i)) shown[i]=c;
        // win / loss
        bool complete=true; for(int i=0;i<wordLen;i++) if(shown[i]=='_') complete=false;
        if(complete){ roundOver=true; guessersWon=true; }
        else if(misses>=HM_MISSES){ roundOver=true; guessersWon=false; }
        break; }
    }
}

static void hmTick(){
    animT++;
    if(roundOver) return;

    if(settingPhase){
        if(!amSetter()) return;         // others just wait
        pbkbd::tick();
        if(pbkbd::entered()){
            const char* t=pbkbd::text();
            wordLen=0;
            for(int i=0;t[i]&&wordLen<HM_MAX_WORD;i++){
                char c=(char)toupper((unsigned char)t[i]);
                if(c>='A'&&c<='Z') hmWord[wordLen++]=c;
            }
            hmWord[wordLen]='\0';
            if(wordLen<3) { pbkbd::clear(); return; }   // need a real hmWord
            for(int i=0;i<wordLen;i++) shown[i]='_';
            shown[wordLen]='\0';
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=2;
            mv.data[0]=OP_WORD; mv.data[1]=(uint8_t)wordLen;
            pbSubmitMove(&mv);          // publishes length only
            settingPhase=false;
            pbkbd::begin(1);
        }
        return;
    }

    // guessing: everyone except the setter takes turns
    if(pbMyTurn() && !amSetter()){
        pbkbd::tick();
        if(pbkbd::entered()){
            const char* t=pbkbd::text();
            char c=t[0]?(char)toupper((unsigned char)t[0]):0;
            pbkbd::clear();
            if(c<'A'||c>'Z') return;
            if(guessed[c-'A']) return;               // already tried
            // The GUESSER can't know if it matched - the setter's device owns the
            // hmWord. So the guess goes out with an empty mask and the setter
            // answers with the matching positions.
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=3;
            mv.data[0]=OP_GUESS; mv.data[1]=(uint8_t)c; mv.data[2]=0;
            // On a shared device (pass & play) the setter's hmWord is right here,
            // so resolve locally; over the link the setter's device answers.
            if(hmWord[0]){
                uint16_t mask=0;
                for(int i=0;i<wordLen&&i<16;i++) if(hmWord[i]==c) mask|=(1u<<i);
                mv.len=4; mv.data[2]=(uint8_t)(mask&0xFF); mv.data[3]=(uint8_t)(mask>>8);
            }
            pbSubmitMove(&mv);
        }
        return;
    }
    // the SETTER's device answers a guess it sees with the matching positions
    // (handled inside hmApply on the setter, which holds `hmWord`)
}

static bool hmOver(int* winner){
    if(!roundOver) return false;
    *winner = guessersWon ? ((setterSeat+1)%pbPlayerCount()) : setterSeat;
    return true;
}
static void hmEnd(int w){ (void)w; }

// ---- cowboy mantis + six-shooter --------------------------------------------
static void drawCowboyMantis(int cx,int cy){
    float wig=(float)animT*0.12f;
    int sway=(int)(sinf(wig)*2);
    uint16_t g=LIME, gd=LIME_DARK;
    // hat
    fb.fillRect(cx-14+sway,cy-26,28,3,PURPLE_DARK);
    fb.fillRoundRect(cx-8+sway,cy-34,16,9,2,PURPLE_DARK);
    // head + eyes
    fb.fillTriangle(cx-9+sway,cy-22, cx+9+sway,cy-22, cx+sway,cy-12, g);
    fb.fillCircle(cx-4+sway,cy-20,2,TEAL_DEEP);
    fb.fillCircle(cx+4+sway,cy-20,2,TEAL_DEEP);
    // bandana
    fb.fillTriangle(cx-6+sway,cy-13, cx+6+sway,cy-13, cx+sway,cy-6, PURPLE_BRIGHT);
    // thorax + vest
    fb.fillRoundRect(cx-8,cy-8,16,18,4,g);
    fb.fillRect(cx-8,cy-6,4,14,gd);
    fb.fillRect(cx+4,cy-6,4,14,gd);
    // raptorial arms, one on the holster
    int a=(int)(sinf(wig*1.6f)*2);
    fb.drawLine(cx-7,cy-4,cx-15,cy+4+a,g);
    fb.drawLine(cx+7,cy-4,cx+14,cy+2-a,g);
    // legs + boots
    fb.drawLine(cx-4,cy+10,cx-7,cy+20,g); fb.fillRect(cx-10,cy+20,6,3,PURPLE_DARK);
    fb.drawLine(cx+4,cy+10,cx+7,cy+20,g); fb.fillRect(cx+4,cy+20,6,3,PURPLE_DARK);
    // antennae under the hat brim
    fb.drawLine(cx-5+sway,cy-23,cx-9+sway,cy-28,gd);
    fb.drawLine(cx+5+sway,cy-23,cx+9+sway,cy-28,gd);
}
static void drawCylinder(int cx,int cy,int loaded){
    fb.drawCircle(cx,cy,16,TEAL_LIGHT);
    fb.drawCircle(cx,cy,15,TEAL);
    for(int i=0;i<HM_MISSES;i++){
        float a=(float)i*(6.2832f/HM_MISSES) - 1.5708f;
        int x=cx+(int)(cosf(a)*9), y=cy+(int)(sinf(a)*9);
        if(i<loaded){ fb.fillCircle(x,y,3,PURPLE_BRIGHT); fb.drawCircle(x,y,3,LIME_PALE); }
        else        { fb.drawCircle(x,y,3,TEAL_LIGHT); }
    }
    fb.fillCircle(cx,cy,2,TEAL_LIGHT);
}

static void hmDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("SIX-GUN MANTIS",W/2,6,2);

    if(settingPhase){
        fb.setTextColor(TEAL_LIGHT);
        if(amSetter()){
            fb.drawString("set the hmWord",W/2,26,1);
            // masked so a shoulder-surfer can't read it
            char mask[HM_MAX_WORD+1]; const char* t=pbkbd::text();
            int n=0; for(;t[n]&&n<HM_MAX_WORD;n++) mask[n]='*'; mask[n]='\0';
            fb.setTextColor(LIME); fb.drawString(mask,W/2,44,4);
        } else {
            fb.drawString(pbPlayerName(setterSeat),W/2,26,1);
            fb.drawString("is picking a hmWord",W/2,40,1);
            drawCowboyMantis(W/2,150);
            fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
            fb.drawString("hold your horses",W/2,H-4,1);
            return;
        }
    } else {
        // the hmWord so far
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
        char sp[HM_MAX_WORD*2+1]; int k=0;
        for(int i=0;i<wordLen&&k<HM_MAX_WORD*2-1;i++){ sp[k++]=shown[i]; sp[k++]=' '; }
        sp[k]='\0';
        fb.drawString(sp,W/2,26,2);
        // the six-shooter cylinder fills up with wrong guesses
        drawCylinder(W-30,66,misses);
        drawCowboyMantis(44,80);
        // letters already tried
        fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL);
        char tried[40]; int tn=0;
        for(int i=0;i<26&&tn<38;i++) if(guessed[i]) tried[tn++]=(char)('A'+i);
        tried[tn]='\0';
        fb.drawString(tried,6,108,1);
        if(roundOver){
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(guessersWon?LIME:PURPLE_BRIGHT);
            fb.drawString(guessersWon?"WORD CRACKED!":"OUT OF CHAMBERS",W/2,150,2);
            if(!guessersWon&&hmWord[0]){ fb.setTextColor(LIME_PALE); fb.drawString(hmWord,W/2,175,2); }
            fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
            fb.drawString("R: rematch   L: game list",W/2,H-4,1);
            return;
        }
    }

    // the shared keyboard, drawn for whoever is typing
    bool myInput = settingPhase ? amSetter() : (pbMyTurn() && !amSetter());
    if(myInput){
        int ky=H-84;
        for(int r=0;r<4;r++){
            const char* row=pbkbd::rowStr(r);
            int len=(int)strlen(row);
            int totalW=len*20;
            int x0=(W-totalW)/2;
            for(int c=0;c<len;c++){
                int idx=0; for(int rr=0;rr<r;rr++) idx+=(int)strlen(pbkbd::rowStr(rr));
                idx+=c;
                bool sel=(idx==pbkbd::cursorKey());
                int kx=x0+c*20;
                fb.fillRoundRect(kx,ky+r*19,18,17,3,sel?PURPLE_DARK:TEAL_DARK);
                if(sel) fb.drawRoundRect(kx,ky+r*19,18,17,3,LIME);
                fb.setTextDatum(MC_DATUM); fb.setTextColor(sel?LIME_PALE:TEAL_LIGHT);
                char lbl[2]={row[c],0};
                fb.drawString(lbl,kx+9,ky+r*19+8,1);
            }
        }
        fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("L: next key   R: press   < bksp  # ok",W/2,H-4,1);
    } else if(!settingPhase){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString(pbPlayerName(pbWhoseTurn()),W/2,H-60,1);
        fb.drawString(amSetter()?"you know the answer":"is guessing",W/2,H-44,1);
    }
}

BoardGame HANGMAN_GAME = {
    "SIX-GUN MANTIS", 5, 2, PB_MAX_PLAYERS,
    hmStart, hmTick, hmApply, hmDraw, hmOver, hmEnd
};
