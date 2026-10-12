// charades.cpp - Charades, built on the host-authoritative party framework.
//
// This module is now purely presentational: the framework owns whose turn it
// is, the phase, the scores and the synced timer. Charades just derives the
// prompt from the shared round seed and draws the four screens, including the
// demoscene hourglass that is the visual centerpiece.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include "charades.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;

static constexpr int W = 170, H = 320;

static const char* CH_PROMPTS[] = {
    "Riding a bike","Brushing teeth","A hungry cat","Astronaut","Volcano",
    "Playing guitar","Swimming","A sneeze","Robot dance","Making pizza",
    "Fishing","Ghost","Superhero landing","Tying shoes","A grumpy bear",
    "Skateboarding","Painting a wall","Chef tasting soup","Karate kick",
    "Butterfly","Juggling","Climbing a ladder","A crying baby","Bowling",
    "Praying mantis","Photographer","Doing laundry","A spider","Rowing a boat",
    "Conducting music","Sneaky ninja","Blowing bubbles","A wobbly penguin",
    "Flipping pancakes","Zombie walk","Casting a spell","Milking a cow",
    "A busy bee","Surfing a wave","Sleeping dog",
};
static const int CH_PROMPT_COUNT = sizeof(CH_PROMPTS)/sizeof(CH_PROMPTS[0]);

static int chPrompt = 0;

static void chRoundStart(uint32_t seed) { chPrompt = (int)(seed % CH_PROMPT_COUNT); }
static const char* chPromptText() { return CH_PROMPTS[chPrompt]; }

// ============================================================================
//  DEMOSCENE HOURGLASS  (the visual centerpiece)
// ----------------------------------------------------------------------------
//  Not a plain outline: a metaballs-ish glowing glass with a falling sand
//  column, a shimmering plasma fill in the bulbs, a swirling particle stream at
//  the neck, and a glowing rim. Driven by the framework's synced clock so every
//  device animates together.
// ============================================================================

// cheap 565 lerp
static inline uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
    int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
    int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
    int r=ar+((br-ar)*t)/255, g=ag+((bg-ag)*t)/255, bl=ab+((bb-ab)*t)/255;
    return (r<<11)|(g<<5)|bl;
}

// One glass bulb as a filled diamond; fillFrac (0..1) fills sand from the point.
// topBulb=true means sand sits at the BOTTOM (draining downward); false = pile
// grows from the bottom point upward.
static void drawSandBulb(int cx, int cyTip, int cyBase, int halfW,
                         float fill, uint32_t t, bool topBulb) {
    int h = abs(cyBase - cyTip);
    int dir = (cyBase > cyTip) ? 1 : -1;   // +1 = base below tip
    for (int i=0;i<=h;i++){
        int y = cyTip + dir*i;
        float f = (float)i/h;                    // 0 at tip .. 1 at base
        int rowHw = (int)(halfW * f);
        // glass shading: rim brighter than center (fake refraction)
        uint16_t glass = lerp565(TEAL_DEEP, TEAL_DARK, (uint8_t)(120*f));
        fb.drawFastHLine(cx-rowHw, y, rowHw*2, glass);
    }
    // sand region
    if (topBulb) {
        // sand drains: remaining sand hugs the neck (tip side), height ~fill
        int sandH = (int)(h * fill);
        for (int i=0;i<sandH;i++){
            int y = cyTip + dir*i;
            float f=(float)i/h; int rowHw=(int)(halfW*f);
            // plasma shimmer in the sand
            uint8_t sh = (uint8_t)(120 + 100*sinf(t/180.0f + y*0.25f));
            uint16_t sand = lerp565(LIME_DARK, LIME, sh);
            fb.drawFastHLine(cx-rowHw, y, rowHw*2, sand);
        }
    } else {
        // bottom pile grows from base(point) upward by (1-fill)
        int pileH = (int)(h * (1.0f - fill));
        for (int i=0;i<pileH;i++){
            int y = cyBase - dir*i;                 // from base upward
            float f = 1.0f - (float)i/h; int rowHw=(int)(halfW*f);
            if (rowHw<0) rowHw=0;
            uint8_t sh = (uint8_t)(120 + 100*sinf(t/200.0f + y*0.3f));
            uint16_t sand = lerp565(LIME_DARK, LIME, sh);
            fb.drawFastHLine(cx-rowHw, y, rowHw*2, sand);
        }
    }
}

static void drawHourglass(int cx, int cy, float frac /*1=full time..0*/) {
    if (frac<0) frac=0; if (frac>1) frac=1;
    uint32_t t = millis();
    const int hw = 34;    // half width at caps
    const int bulb = 52;  // height of each bulb
    int topTip = cy;          // neck
    int topCapY = cy - bulb;
    int botTip = cy;
    int botCapY = cy + bulb;

    // outer glow rim (pulsing)
    uint8_t rimB = (uint8_t)(150 + 90*sinf(t/300.0f));
    uint16_t rim = lerp565(TEAL_DARK, TEAL_LIGHT, rimB);

    // caps
    fb.drawFastHLine(cx-hw-2, topCapY-1, (hw+2)*2, rim);
    fb.drawFastHLine(cx-hw-2, botCapY+1, (hw+2)*2, rim);
    fb.fillRect(cx-hw-3, topCapY-4, (hw+3)*2, 4, PURPLE_DARK);
    fb.fillRect(cx-hw-3, botCapY,   (hw+3)*2, 4, PURPLE_DARK);

    // top bulb: sand drains as time passes -> remaining sand = frac
    drawSandBulb(cx, topTip, topCapY, hw, frac, t, true);
    // bottom bulb: pile grows as time passes -> fill arg is frac (so 1-frac pile)
    drawSandBulb(cx, botTip, botCapY, hw, frac, t, false);

    // glass edges (bright refraction lines)
    fb.drawLine(cx-hw, topCapY, cx, topTip, rim);
    fb.drawLine(cx+hw, topCapY, cx, topTip, rim);
    fb.drawLine(cx-hw, botCapY, cx, botTip, rim);
    fb.drawLine(cx+hw, botCapY, cx, botTip, rim);
    // a moving specular highlight down the left glass
    float sp=(sinf(t/500.0f)*0.5f+0.5f);
    int syl=topCapY+(int)(sp*bulb);
    fb.drawPixel(cx-(int)(hw*(1.0f-(float)(syl-topCapY)/bulb)), syl, LIME_PALE);

    // falling sand stream through the neck: swirling particles
    if (frac>0.001f && frac<0.999f) {
        for (int i=0;i<14;i++){
            float p = fmodf(t/90.0f + i*0.37f, 1.0f);    // 0..1 down the stream
            int y = topTip + (int)(p*bulb*0.9f);
            float sw = sinf(t/60.0f + i*1.7f + p*6.28f); // swirl
            int x = cx + (int)(sw*3*(0.4f+p));
            uint16_t c = (i&1)?LIME_PALE:LIME;
            fb.drawPixel(x, y, c);
            if (p>0.6f) fb.drawPixel(x, y+1, LIME_DARK);
        }
        // impact sparkle where the stream hits the pile
        int pileTop = botCapY - (int)(bulb*(1.0f-frac));
        for (int k=0;k<4;k++){
            float a=t/70.0f + k*1.57f;
            fb.drawPixel(cx+(int)(cosf(a)*3), pileTop+(int)(fabsf(sinf(a))*2), LIME_PALE);
        }
    }
}

// ============================================================================
//  SCREENS
// ============================================================================
static void chDrawPerform() {
    // The UP player: prompt + the big hourglass. (Caller has already filled bg
    // and will pushSprite.)
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    // prompt card up top
    fb.fillRoundRect(6, 22, W-12, 44, 6, PURPLE_DARK);
    fb.setTextColor(LIME_PALE); fb.drawString("ACT THIS OUT", W/2, 34, 1);
    fb.setTextColor(LIME);      fb.drawString(chPromptText(), W/2, 52, 2);
    // hourglass centerpiece
    float frac=(float)partyMsLeft()/(float)partyMsTotal();
    drawHourglass(W/2, 150, frac);
    // seconds
    fb.setTextColor(LIME_PALE);
    char tb[8]; snprintf(tb,sizeof(tb),"%lus",(unsigned long)(partyMsLeft()/1000));
    fb.drawString(tb, W/2, 232, 2);
    fb.setTextColor(TEAL_DARK);
    fb.drawString(partyPhase()==PH_LIVE?"waiting for a guess":"press R to start", W/2, 252, 1);
}

static void chDrawWatch() {
    // Non-UP player during LIVE/GUESS: same synced hourglass + a GUESS prompt.
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    PartyPlayer* u=partyPlayer(partyWhoUp());
    fb.setTextColor(LIME_PALE);
    char b[24]; snprintf(b,sizeof(b),"%s is acting", u?u->name:"?");
    fb.drawString(b, W/2, 20, 1);
    float frac=(float)partyMsLeft()/(float)partyMsTotal();
    drawHourglass(W/2, 140, frac);
    fb.setTextColor(LIME_PALE);
    char tb[8]; snprintf(tb,sizeof(tb),"%lus",(unsigned long)(partyMsLeft()/1000));
    fb.drawString(tb, W/2, 222, 2);
    if (partyPhase()==PH_LIVE) {
        fb.fillRoundRect(20,244,W-40,40,6,PURPLE);
        fb.setTextColor(LIME); fb.drawString("L / R : GUESS", W/2, 264, 2);
    }
}

static void chDrawJudge() {
    // UP player judges the buzzed-in guess.
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    PartyPlayer* g=partyPlayer(partyGuesser());
    fb.setTextColor(LIME_PALE);
    char b[24]; snprintf(b,sizeof(b),"%s guessed!", g?g->name:"someone");
    fb.drawString(b, W/2, 30, 2);
    fb.setTextColor(TEAL_LIGHT); fb.drawString("Were they right?", W/2, 56, 1);
    fb.fillRoundRect(6,90,(W-18)/2,150,8,TFT_RED);
    fb.fillRoundRect(W/2+3,90,(W-18)/2,150,8,TFT_GREEN);
    fb.setTextColor(TFT_WHITE); fb.drawString("L", 6+(W-18)/4, 150, 4);
    fb.drawString("WRONG", 6+(W-18)/4, 182, 1);
    fb.setTextColor(TFT_BLACK); fb.drawString("R", W/2+3+(W-18)/4, 150, 4);
    fb.drawString("RIGHT", W/2+3+(W-18)/4, 182, 1);
}

static void chDrawReveal() {
    fb.fillSprite(LIME_DARK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE); fb.drawString("CORRECT!", W/2, 90, 4);
    fb.setTextColor(TFT_WHITE); fb.drawString("The answer:", W/2, 140, 1);
    fb.setTextColor(LIME);      fb.drawString(chPromptText(), W/2, 166, 2);
}

PartyGame CHARADES_GAME = {
    "Charades",
    chRoundStart,
    chDrawPerform,
    chDrawWatch,
    chDrawJudge,
    chDrawReveal,
    chPromptText,
};
