// FILE: src/party/pb_trivia.cpp
// MANTIS QUIZ - buzzer trivia with a rotating question-master.
//
// One player is "IT": their screen shows the question AND the answer, and
// nobody else's does. They read it aloud. Everyone else waits for the buzzers to
// open, then slams a button - first buzz gets the floor, says their answer out
// loud, and the question-master judges it right (R) or wrong (L). Right scores
// the buzzer; wrong re-opens the buzzers for whoever is left. Then "IT" passes
// to the next player and a new question comes up.
//
// Buzzing is simultaneous, not turn-based, so it goes out through pbSubmitInfo -
// applied everywhere with the turn rotation untouched.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "party_board.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W=170, H=320;
static constexpr int TQ_WIN=5;            // points to win

// ---- question bank ----------------------------------------------------------
// Kid-friendly mix: bugs and nature (fits the mantis theme), plus general
// knowledge. Answers are short so the master can judge a spoken answer fairly.
struct TQ { const char* q; const char* a; };
static const TQ BANK[] = {
    {"How many legs does an insect have?","Six"},
    {"What do praying mantises mostly eat?","Other insects"},
    {"How many eyes does a praying mantis have?","Five"},
    {"Which insect makes honey?","The honeybee"},
    {"What is a young frog called?","A tadpole"},
    {"Which bird can fly backwards?","The hummingbird"},
    {"What is the largest mammal on Earth?","The blue whale"},
    {"How many legs does a spider have?","Eight"},
    {"What do you call a group of wolves?","A pack"},
    {"Which planet is closest to the Sun?","Mercury"},
    {"How many planets are in our solar system?","Eight"},
    {"What gas do plants breathe in?","Carbon dioxide"},
    {"What is the hardest natural substance?","Diamond"},
    {"How many continents are there?","Seven"},
    {"What is the tallest mountain on Earth?","Mount Everest"},
    {"Which ocean is the largest?","The Pacific"},
    {"What state are we in?","Idaho or Utah"},
    {"How many states are in the USA?","Fifty"},
    {"What do bees collect from flowers?","Nectar and pollen"},
    {"Which animal is the fastest on land?","The cheetah"},
    {"What do caterpillars turn into?","Butterflies or moths"},
    {"How many sides does a hexagon have?","Six"},
    {"What is frozen water called?","Ice"},
    {"What do we call animals that eat only plants?","Herbivores"},
    {"Which shape has three sides?","A triangle"},
    {"What is the biggest cat in the world?","The tiger"},
    {"How many minutes are in an hour?","Sixty"},
    {"What color do you get mixing blue and yellow?","Green"},
    {"What season comes after winter?","Spring"},
    {"How many teeth does an adult human have?","Thirty-two"},
    {"Which animal carries its home on its back?","A snail or turtle"},
    {"What is the closest star to Earth?","The Sun"},
    {"How many legs does a mantis use to walk?","Four"},
    {"What sound does a cricket make with its wings?","Chirping"},
    {"What is a baby deer called?","A fawn"},
    {"Which fish is famous for swimming upstream?","The salmon"},
    {"What tool tells you which way is north?","A compass"},
    {"What is dried grass used to feed animals?","Hay"},
    {"How many days are in a leap year?","366"},
    {"What do we call a mantis before it grows up?","A nymph"},
};
static const int BANK_N = (int)(sizeof(BANK)/sizeof(BANK[0]));

// ---- opcodes ----------------------------------------------------------------
enum : uint8_t { OP_QUESTION=1, OP_OPEN=2, OP_BUZZ=3, OP_JUDGE=4, OP_NEXT=5 };

enum Phase : uint8_t { PH_READ=0, PH_BUZZ, PH_ANSWER, PH_RESULT };
static Phase  phase=PH_READ;
static int    itSeat=0;
static int    qIdx=0;
static int    buzzSeat=-1;
static bool   lockedOut[PB_MAX_PLAYERS];
static int    score[PB_MAX_PLAYERS];
static bool   lastRight=false;
static uint32_t animT=0, phaseT=0;
static uint32_t qSeedWalk=0;

static void pickQuestion(){
    // walk the bank deterministically from the match seed so every device shows
    // the same question without needing to send the text
    qSeedWalk = qSeedWalk*1664525u + 1013904223u;
    qIdx = (int)((qSeedWalk>>8) % (uint32_t)BANK_N);
}

static void tqStart(uint32_t sd,int n,int seat){
    (void)n;(void)seat;
    memset(score,0,sizeof(score));
    memset(lockedOut,0,sizeof(lockedOut));
    qSeedWalk = sd ^ 0x51ED270Bu;
    itSeat=0; buzzSeat=-1; phase=PH_READ; phaseT=millis();
    pickQuestion();
}

static void tqApply(const PbMove* m){
    if(m->len<1) return;
    switch(m->data[0]){
    case OP_OPEN:
        phase=PH_BUZZ; buzzSeat=-1;
        memset(lockedOut,0,sizeof(lockedOut));
        phaseT=millis();
        break;
    case OP_BUZZ:
        // first buzz wins; later ones are ignored
        if(phase==PH_BUZZ && buzzSeat<0 && m->player<PB_MAX_PLAYERS && !lockedOut[m->player]){
            buzzSeat=m->player; phase=PH_ANSWER; phaseT=millis();
        }
        break;
    case OP_JUDGE: {
        if(m->len<2) break;
        bool right=m->data[1]!=0;
        lastRight=right;
        if(right && buzzSeat>=0 && buzzSeat<PB_MAX_PLAYERS){
            score[buzzSeat]++;
            phase=PH_RESULT; phaseT=millis();
        } else {
            if(buzzSeat>=0&&buzzSeat<PB_MAX_PLAYERS) lockedOut[buzzSeat]=true;
            // anyone left to buzz?
            int left=0;
            for(int s=0;s<pbPlayerCount();s++) if(s!=itSeat && !lockedOut[s]) left++;
            buzzSeat=-1;
            phase = left>0 ? PH_BUZZ : PH_RESULT;
            phaseT=millis();
        }
        break; }
    case OP_NEXT:
        itSeat=(itSeat+1)%pbPlayerCount();
        pickQuestion();
        buzzSeat=-1; memset(lockedOut,0,sizeof(lockedOut));
        phase=PH_READ; phaseT=millis();
        break;
    }
}

static bool amIt(){ return pbMySeat()==itSeat; }

static void tqTick(){
    animT++;
    uint32_t now=millis();
    bool lE=btnL&&!btnL_prev, rE=btnR&&!btnR_prev;

    switch(phase){
    case PH_READ:
        // the question-master opens the buzzers when they've finished reading
        if(amIt() && rE && now-phaseT>400){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_OPEN;
            pbSubmitInfo(&mv);
        }
        break;
    case PH_BUZZ:
        // everyone but the master can slam either button to buzz in
        if(!amIt() && !lockedOut[pbMySeat()] && (lE||rE) && now-phaseT>150){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_BUZZ;
            pbSubmitInfo(&mv);
        }
        break;
    case PH_ANSWER:
        // the master judges the spoken answer
        if(amIt() && (lE||rE) && now-phaseT>300){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=2;
            mv.data[0]=OP_JUDGE; mv.data[1]=rE?1:0;
            pbSubmitInfo(&mv);
        }
        break;
    case PH_RESULT:
        if(amIt() && (lE||rE) && now-phaseT>500){
            PbMove mv; memset(&mv,0,sizeof(mv));
            mv.player=(uint8_t)pbMySeat(); mv.len=1; mv.data[0]=OP_NEXT;
            pbSubmitInfo(&mv);
        }
        break;
    }
}

static bool tqOver(int* winner){
    for(int s=0;s<pbPlayerCount();s++) if(score[s]>=TQ_WIN){ *winner=s; return true; }
    return false;
}
static void tqEnd(int w){ (void)w; }

// word-wrapped question text
static void drawWrapped(const char* txt,int x,int y,int maxW,int lineH,int font){
    char line[40]; int li=0; int cy=y;
    const char* p=txt;
    while(*p){
        const char* sp=strchr(p,' ');
        int wlen = sp? (int)(sp-p) : (int)strlen(p);
        if(wlen>38) wlen=38;
        char word[40]; memcpy(word,p,wlen); word[wlen]='\0';
        char test[80]; snprintf(test,sizeof(test),"%s%s%s",line,li?" ":"",word);
        if(fb.textWidth(test,font)>maxW && li>0){
            fb.drawString(line,x,cy,font); cy+=lineH; li=0; line[0]='\0';
            snprintf(line,sizeof(line),"%s",word); li=(int)strlen(line);
        } else {
            snprintf(line,sizeof(line),"%s",test); li=(int)strlen(line);
        }
        if(!sp) break;
        p=sp+1;
    }
    if(li) fb.drawString(line,x,cy,font);
}

static void tqDraw(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("MANTIS QUIZ",W/2,6,2);
    // scores
    for(int s=0;s<pbPlayerCount()&&s<4;s++){
        int x=6+s*40;
        bool it=(s==itSeat);
        fb.fillRoundRect(x,26,36,20,3,it?PURPLE_DARK:TEAL_DARK);
        if(s==pbMySeat()) fb.drawRoundRect(x,26,36,20,3,LIME);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(it?LIME_PALE:TEAL_LIGHT);
        char b[8]; snprintf(b,sizeof(b),"%d",score[s]);
        fb.drawString(b,x+18,36,2);
    }
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_DARK);
    char hdr[30];
    snprintf(hdr,sizeof(hdr),"%s asks   first to %d",
             amIt()?"YOU":pbPlayerName(itSeat),TQ_WIN);
    fb.drawString(hdr,W/2,52,1);

    if(amIt()){
        // the master sees the question AND the answer
        fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
        drawWrapped(BANK[qIdx].q,8,72,W-16,16,2);
        fb.setTextColor(TEAL);
        fb.drawString("answer:",8,168,1);
        fb.setTextColor(LIME);
        drawWrapped(BANK[qIdx].a,8,182,W-16,14,2);
        fb.drawFastHLine(8,164,W-16,TEAL_DARK);
    } else {
        // everyone else sees no text at all - they listen
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString("listen to the question",W/2,100,1);
        // a big buzzer
        bool live = (phase==PH_BUZZ && !lockedOut[pbMySeat()]);
        uint16_t bc = live ? ((animT/6)%2?PURPLE_BRIGHT:PURPLE_LIGHT)
                          : (lockedOut[pbMySeat()]?TEAL_DARK:TEAL);
        fb.fillCircle(W/2,168,42,bc);
        fb.drawCircle(W/2,168,42,LIME_PALE);
        fb.setTextColor(live?LIME_PALE:TEAL_DEEP);
        fb.drawString(live?"BUZZ!":(lockedOut[pbMySeat()]?"LOCKED":"WAIT"),W/2,168,2);
    }

    // phase banner
    fb.setTextDatum(BC_DATUM);
    const char* foot="";
    switch(phase){
    case PH_READ:   foot = amIt()?"read it out, R: open buzzers":"get ready..."; break;
    case PH_BUZZ:   foot = amIt()?"buzzers are open":"hit a button to buzz"; break;
    case PH_ANSWER: {
        static char fbuf[36];
        if(amIt()) snprintf(fbuf,sizeof(fbuf),"%s buzzed - L: wrong  R: right",pbPlayerName(buzzSeat));
        else snprintf(fbuf,sizeof(fbuf),"%s is answering",pbPlayerName(buzzSeat));
        foot=fbuf; break; }
    case PH_RESULT: foot = amIt()?"R: next question":(lastRight?"correct!":"nobody got it"); break;
    }
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(foot,W/2,H-6,1);
    if(phase==PH_ANSWER && buzzSeat>=0){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
        fb.drawString(pbPlayerName(buzzSeat),W/2,H-40,4);
    }
}

BoardGame TRIVIA_GAME = {
    "MANTIS QUIZ", 6, 2, PB_MAX_PLAYERS,
    tqStart, tqTick, tqApply, tqDraw, tqOver, tqEnd
};
