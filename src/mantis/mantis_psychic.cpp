// FILE: src/mantis/mantis_psychic.cpp
// MANTIS PSYCHIC - a procedural mentalism engine.
//
// TWO MODES off the start screen:
//   SOLO      the mantis deals you one trick and amazes you. You never type
//             your number in - that absence IS the proof. One trick per run;
//             click again from the start screen for another.
//   PERFORMER a library of self-working tricks it TEACHES you to do to someone
//             else. Automatic magic: follow the script, it always lands.
//
// THIS RUNTIME PERFORMS NO FORCE ARITHMETIC. Every force is proven offline by
// tools/gen_psychic.py, which refuses to emit an effect unless what the device
// is told cannot account for what it reveals. The device just plays beats.
//
// The few effects that DO take a value (the card, the birthday, the clock) are
// derivation effects: the user reports one composite number that names neither
// of the two facts the mantis then states. The validator proved the gap.
//
// The mantis remembers nothing. There is deliberately no NVS namespace here -
// nothing to save, nothing to flush.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <string.h>
#include <math.h>
#include "mantis_psychic.h"
#include "../theme_colors.h"
#include "../chord_guard.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ── pack index ───────────────────────────────────────────────────────────────
static constexpr int  MP_MAX_FX    = 64;
static constexpr int  MP_TITLE_LEN = 24;
static constexpr int  MP_BUF       = 1400;   // largest effect measured: 584 B
static constexpr int  MP_MAX_BEATS = 48;
static constexpr int  MP_LINE      = 720;    // longest pack line + margin (gen_psychic.py checks)

struct FxIndex {
    uint32_t off;                  // byte offset of the @FX line
    char     title[MP_TITLE_LEN];
    uint8_t  performer;            // 1 = performer mode, 0 = solo
};
static FxIndex* fxIdx = nullptr;   // [MP_MAX_FX] on the heap while the app is open
static int     fxCount = 0, soloCount = 0, perfCount = 0;

// ── the loaded effect ────────────────────────────────────────────────────────
static char* buf = nullptr;   // [MP_BUF] on the heap while the app is open
static int   bufLen = 0;
struct Beat { char kind; uint16_t at; };   // at = offset into buf of the arg
static Beat* beats = nullptr;   // [MP_MAX_BEATS] on the heap while the app is open
static int   beatCount = 0, beatIdx = 0;

// ── slots + tiny postfix evaluator (derivation effects only) ────────────────
static long slots[4];
static long evalPostfix(const char* e){
    long st[8]; int sp = 0;
    const char* p = e;
    while (*p){
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == 's' && p[1] >= '0' && p[1] <= '3'){
            if (sp < 8) st[sp++] = slots[p[1]-'0'];
            p += 2;
        } else if (*p == '-' && p[1] >= '0' && p[1] <= '9'){
            long v = 0; p++; while (*p >= '0' && *p <= '9'){ v = v*10 + (*p-'0'); p++; }
            if (sp < 8) st[sp++] = -v;
        } else if (*p >= '0' && *p <= '9'){
            long v = 0; while (*p >= '0' && *p <= '9'){ v = v*10 + (*p-'0'); p++; }
            if (sp < 8) st[sp++] = v;
        } else {
            char op = *p++;
            if (sp >= 2){
                long b = st[--sp], a = st[--sp];
                long r = 0;
                switch (op){
                    case '+': r = a + b; break;
                    case '-': r = a - b; break;
                    case '*': r = a * b; break;
                    case '/': r = b ? a / b : 0; break;
                    case '%': r = b ? a % b : 0; break;
                    default:  r = a;    break;
                }
                st[sp++] = r;
            }
        }
    }
    return sp ? st[sp-1] : 0;
}

// ── reveal formatting (mirrors the generator's fmt()) ────────────────────────
static const char* CARD_V[14] = {"","ACE","TWO","THREE","FOUR","FIVE","SIX","SEVEN",
                                 "EIGHT","NINE","TEN","JACK","QUEEN","KING"};
static const char* CARD_S[5]  = {"","CLUBS","HEARTS","SPADES","DIAMONDS"};
static const char* MON[13]    = {"","JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG",
                                 "SEP","OCT","NOV","DEC"};
// shared by the reveal tokens and the 'G'/'Q' beats (defined further down)
static const int   NSYM = 10;
static const char* SYM_NAME[NSYM] = {"STAR","MOON","HEART","EYE","SUN",
                                     "BOLT","DIAMOND","RING","CROSS","TRIANGLE"};
static uint8_t* chart = nullptr;   // [100] on the heap while the app is open
static int     chartTarget = 0;
static bool    chartOn = false;
static char* qText = nullptr;   // [260] on the heap while the app is open
static char    qWord[24];
static bool    textOn = false;
static char revBuf[48];
static bool revIsSymbol = false;
static const char* formatReveal(const char* t){
    // Only a handful of tokens exist; anything else passes through verbatim.
    if (!strncmp(t, "{CARD", 5)){
        long v = slots[0], s = slots[1];
        snprintf(revBuf, sizeof(revBuf), "%s OF %s",
                 (v >= 1 && v <= 13) ? CARD_V[v] : "?",
                 (s >= 1 && s <= 4)  ? CARD_S[s] : "?");
        return revBuf;
    }
    if (!strncmp(t, "{MONTH", 6)){
        long m = slots[0];
        snprintf(revBuf, sizeof(revBuf), "%s %ld",
                 (m >= 1 && m <= 12) ? MON[m] : "?", slots[1]);
        return revBuf;
    }
    if (!strncmp(t, "{SYMBOL", 7)){
        revIsSymbol = true;
        snprintf(revBuf, sizeof(revBuf), "THE %s", SYM_NAME[chartTarget % NSYM]);
        return revBuf;
    }
    if (!strncmp(t, "{WORD", 5)){
        snprintf(revBuf, sizeof(revBuf), "%s", qWord[0] ? qWord : "?");
        return revBuf;
    }
    if (!strncmp(t, "{CLOCK", 6)){
        snprintf(revBuf, sizeof(revBuf), "%ld:%02ld", slots[0], slots[1]);
        return revBuf;
    }
    return t;
}

// ── screens ──────────────────────────────────────────────────────────────────
enum Screen : uint8_t { SC_START = 0, SC_LIST, SC_PLAY, SC_DONE };
static Screen scr = SC_START;
static int    modeSel = 0;        // 0 = solo, 1 = performer
static int    listSel = 0, listTop = 0;
static bool   listPerformer = false;

// ── beat runner state ────────────────────────────────────────────────────────
static const char* sayLine   = "";
static const char* promptTxt = "";
static const char* revealTxt = nullptr;
static uint8_t  pose = 0;
static uint32_t holdUntil = 0;
static bool     awaitPress = false;
static uint32_t frameNo = 0, enterAt = 0, revealAt = 0;
static constexpr uint32_t MP_ENTER_GUARD = 250;

// value-ask state
static bool  asking = false;
static long  askLo = 0, askHi = 0, askVal = 0;
// Digit entry for big ranges: scrolling 101 -> 1231 one step at a time was
// over a thousand presses. L turns the current digit, R moves on.
static bool  askDigits = false;
static int   askNd = 0, askPos = 0;
static uint8_t askD[5];
static bool  askBad = false;
static int   askSlot = 0;
static uint32_t askRepeat = 0;
// choice state
static bool  choosing = false;
static int   choiceN = 0, choiceSel = 0;
static const char* choiceLbl[4] = {nullptr,nullptr,nullptr,nullptr};

static uint32_t rng = 1;
static uint32_t pr(){ rng = rng*1664525u + 1013904223u; return rng >> 8; }

// ── 'G' symbol chart and 'Q' counted text (replay-proof reveals) ─────────────
// The old solo reveals were fixed ("NINE", "1089", "FOUR"), so a second play
// exposed the force. Now the forced number only says WHERE to look, and what
// is there is re-dealt every play:
//   M  chart 0..99: every number the force can produce (multiples of 9,
//      9..81) shows the SAME symbol, chosen at random this play; no other
//      cell shows it. The user's own number varies, the symbol varies.
//   Q  a text picked at random from several; the user counts N words in.

static void dealChart(){
    chartTarget = (int)(pr() % NSYM);
    for (int v = 0; v < 100; v++){
        if (v >= 9 && v <= 81 && v % 9 == 0) { chart[v] = (uint8_t)chartTarget; continue; }
        int s = (int)(pr() % (NSYM - 1));
        chart[v] = (uint8_t)(s >= chartTarget ? s + 1 : s);   // never the target
    }
}

// arg: prompt|N|text1~text2~...   pick one text, remember its Nth word
static void dealText(const char* a){
    const char* b1 = strchr(a, '|'); if (!b1) return;
    int n = atoi(b1 + 1);
    const char* b2 = strchr(b1 + 1, '|'); if (!b2) return;
    const char* t = b2 + 1;
    int count = 1; for (const char* p = t; *p; p++) if (*p == '~') count++;
    int want = (int)(pr() % (uint32_t)count);
    for (int k = 0; k < want; k++) { t = strchr(t, '~'); t++; }
    const char* e = strchr(t, '~'); int len = e ? (int)(e - t) : (int)strlen(t);
    if (len > (int)(sizeof(char)*(260)) - 1) len = (sizeof(char)*(260)) - 1;
    memcpy(qText, t, (size_t)len); qText[len] = 0;
    qWord[0] = 0;
    int w = 0; const char* p = qText;
    while (*p){
        while (*p == ' ') p++;
        if (!*p) break;
        const char* ws = p; while (*p && *p != ' ') p++;
        if (++w == n){
            int wl = (int)(p - ws); if (wl > 23) wl = 23;
            memcpy(qWord, ws, (size_t)wl); qWord[wl] = 0;
            // trim trailing punctuation so the reveal reads cleanly
            while (wl && (qWord[wl-1]=='.'||qWord[wl-1]==','||qWord[wl-1]=='!'||qWord[wl-1]=='?')) qWord[--wl] = 0;
            break;
        }
    }
}

// small procedural glyphs - no bitmap assets needed
static void drawSymbol(int id, int cx, int cy, int r, uint16_t c){
    switch (id){
    case 0: for (int i = 0; i < 5; i++){ float a1 = -1.5708f + i*1.2566f, a2 = a1 + 2.5133f;
                fb.drawLine(cx + (int)(cosf(a1)*r), cy + (int)(sinf(a1)*r),
                            cx + (int)(cosf(a2)*r), cy + (int)(sinf(a2)*r), c); } break;   // star
    case 1: fb.fillCircle(cx, cy, r, c); fb.fillCircle(cx + r/2, cy - r/3, r, TEAL_DEEP); break; // moon
    case 2: fb.fillCircle(cx - r/2, cy - r/4, r/2 + 1, c); fb.fillCircle(cx + r/2, cy - r/4, r/2 + 1, c);
            fb.fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r, c); break;                     // heart
    case 3: fb.drawCircle(cx, cy, r, c); fb.fillCircle(cx, cy, r/2, c); break;                  // eye
    case 4: fb.fillCircle(cx, cy, r/2, c);
            for (int i = 0; i < 8; i++){ float a = i*0.7854f;
                fb.drawLine(cx + (int)(cosf(a)*(r*0.7f)), cy + (int)(sinf(a)*(r*0.7f)),
                            cx + (int)(cosf(a)*r), cy + (int)(sinf(a)*r), c); } break;         // sun
    case 5: fb.drawLine(cx + r/2, cy - r, cx - r/3, cy, c); fb.drawLine(cx - r/3, cy, cx + r/3, cy, c);
            fb.drawLine(cx + r/3, cy, cx - r/2, cy + r, c); break;                             // bolt
    case 6: fb.drawLine(cx, cy - r, cx + r, cy, c); fb.drawLine(cx + r, cy, cx, cy + r, c);
            fb.drawLine(cx, cy + r, cx - r, cy, c); fb.drawLine(cx - r, cy, cx, cy - r, c); break; // diamond
    case 7: fb.drawCircle(cx, cy, r, c); fb.drawCircle(cx, cy, r/2, c); break;                  // ring
    case 8: fb.fillRect(cx - r, cy - 1, 2*r + 1, 3, c); fb.fillRect(cx - 1, cy - r, 3, 2*r + 1, c); break; // cross
    default: fb.drawLine(cx, cy - r, cx + r, cy + r, c); fb.drawLine(cx + r, cy + r, cx - r, cy + r, c);
             fb.drawLine(cx - r, cy + r, cx, cy - r, c); break;                                // triangle
    }
}

// ── pack loading ─────────────────────────────────────────────────────────────
// The content partition is named "content" (partitions_16MB_pinball.csv).
// LittleFS.begin(false) looks for "spiffs", fails, and Psychic indexed ZERO
// tricks - every selection bounced back to the welcome screen. Mount it the
// same way the working content apps do (no format-on-fail here).
static bool psyMount(){ return LittleFS.begin(false, "/littlefs", 10, "content"); }
static void buildIndex(){
    fxCount = soloCount = perfCount = 0;
    if (!psyMount()) return;
    fs::File f = LittleFS.open("/content/psychic.mpk", "r");
    if (!f) return;
    uint32_t pos = 0;
    char line[MP_LINE];   // one beat per line; the book trick's line is ~610 B
    while (f.available() && fxCount < MP_MAX_FX){
        uint32_t lineStart = pos;
        int n = f.readBytesUntil('\n', line, sizeof(line)-1);
        if (n < 0) break;
        line[n] = '\0';
        pos += (uint32_t)n + 1;
        if (n > 4 && line[0]=='@' && line[1]=='F' && line[2]=='X'){
            // @FX id|title|mode|method
            char* p = line + 4;
            char* bar1 = strchr(p, '|');           if(!bar1) continue;
            char* bar2 = strchr(bar1+1, '|');      if(!bar2) continue;
            FxIndex& e = fxIdx[fxCount];
            e.off = lineStart;
            int tl = (int)(bar2 - (bar1+1));
            if (tl > MP_TITLE_LEN-1) tl = MP_TITLE_LEN-1;
            memcpy(e.title, bar1+1, tl); e.title[tl] = '\0';
            const char* mode = bar2 + 1;
            e.performer = (strncmp(mode, "performer", 9) == 0) ? 1 : 0;
            if (e.performer) perfCount++; else soloCount++;
            fxCount++;
        }
    }
    f.close();
}

// Load one effect's beats into buf. Skips @GAP (that is documentation for the
// validator and for you, not something the runtime needs).
static bool loadEffect(int i){
    beatCount = 0; bufLen = 0; beatIdx = 0;
    for (int k = 0; k < 4; k++) slots[k] = 0;
    if (i < 0 || i >= fxCount) return false;
    if (!psyMount()) return false;
    fs::File f = LittleFS.open("/content/psychic.mpk", "r");
    if (!f) return false;
    f.seek(fxIdx[i].off);
    char line[MP_LINE];   // one beat per line; the book trick's line is ~610 B
    bool first = true;
    while (f.available() && beatCount < MP_MAX_BEATS){
        int n = f.readBytesUntil('\n', line, sizeof(line)-1);
        if (n <= 0) break;
        line[n] = '\0';
        while (n > 0 && (line[n-1]=='\r' || line[n-1]==' ')) line[--n] = '\0';
        if (n < 2 || line[0] != '@') continue;
        char kind = line[1];
        if (first){ first = false; continue; }          // the @FX line itself
        if (kind == 'G') continue;                       // gap contract: docs
        if (kind == 'E') break;                          // end of effect
        if (kind == 'F') break;                          // next effect started
        const char* arg = (n > 3) ? line + 3 : "";
        int alen = (int)strlen(arg);
        if (bufLen + alen + 1 > MP_BUF) break;           // out of room: stop clean
        beats[beatCount].kind = kind;
        beats[beatCount].at   = (uint16_t)bufLen;
        memcpy(buf + bufLen, arg, alen + 1);
        bufLen += alen + 1;
        beatCount++;
    }
    f.close();
    return beatCount > 0;
}

static const char* argOf(int b){ return buf + beats[b].at; }

// ── beat advance ─────────────────────────────────────────────────────────────
static void startBeat();

static void nextBeat(){
    beatIdx++;
    if (beatIdx >= beatCount){ scr = SC_DONE; revealAt = millis(); return; }
    startBeat();
}

static void startBeat(){
    awaitPress = false; asking = false; choosing = false; chartOn = false; textOn = false;
    holdUntil = 0;
    const char* a = argOf(beatIdx);
    switch (beats[beatIdx].kind){
    case 'P': {                                   // pose
        const char* names[6] = {"idle","think","gaze","raise","reveal","bow"};
        pose = 0;
        for (int i = 0; i < 6; i++) if (!strcmp(a, names[i])) { pose = (uint8_t)i; break; }
        nextBeat(); return;
    }
    case 'S': sayLine = a; nextBeat(); return;    // say (text persists on screen)
    case 'W': holdUntil = millis() + (uint32_t)atol(a); return;
    case 'B': promptTxt = a; awaitPress = true; return;
    case 'M': dealChart(); chartOn = true; promptTxt = a; awaitPress = true; return;   // 'G' is the gap-contract doc line
    case 'Q': {                                   // counted text: prompt|N|t1~t2...
        static char qp[48];
        const char* b1 = strchr(a, '|');
        int pl = b1 ? (int)(b1 - a) : (int)strlen(a); if (pl > 47) pl = 47;
        memcpy(qp, a, (size_t)pl); qp[pl] = 0;
        dealText(a); textOn = true; promptTxt = qp; awaitPress = true; return;
    }
    case 'A': {                                   // ask a value: prompt|lo|hi|slot
        static char pbuf[40];
        const char* b1 = strchr(a, '|');
        if (!b1){ nextBeat(); return; }
        int pl = (int)(b1 - a); if (pl > 39) pl = 39;
        memcpy(pbuf, a, pl); pbuf[pl] = '\0';
        promptTxt = pbuf;
        askLo  = atol(b1 + 1);
        const char* b2 = strchr(b1 + 1, '|');
        askHi  = b2 ? atol(b2 + 1) : askLo;
        const char* b3 = b2 ? strchr(b2 + 1, '|') : nullptr;
        askSlot = b3 ? atoi(b3 + 1) : 0;   // slot index follows the third bar
        if (askSlot < 0 || askSlot > 3) askSlot = 0;
        askVal = askLo;
        askDigits = askHi >= 100;
        if (askDigits){
            askNd = 0; for (long h = askHi; h > 0; h /= 10) askNd++;
            if (askNd > 5) askNd = 5;
            memset(askD, 0, sizeof(askD)); askPos = 0; askBad = false;
        }
        asking = true;
        return;
    }
    case 'C': {                                   // choice: prompt|a|b|c...
        static char cbuf[120];
        int cl = (int)strlen(a); if (cl > 119) cl = 119;
        memcpy(cbuf, a, cl); cbuf[cl] = '\0';
        char* p = cbuf;
        char* bar = strchr(p, '|');
        if (!bar){ nextBeat(); return; }
        *bar = '\0'; promptTxt = p; p = bar + 1;
        choiceN = 0;
        while (p && *p && choiceN < 4){
            char* nb = strchr(p, '|');
            if (nb) *nb = '\0';
            choiceLbl[choiceN++] = p;
            p = nb ? nb + 1 : nullptr;
        }
        choiceSel = 0; choosing = true;
        return;
    }
    case 'X': {                                   // compute slot|expr
        const char* bar = strchr(a, '|');
        if (bar){
            int sl = atoi(a);
            if (sl >= 0 && sl <= 3) slots[sl] = evalPostfix(bar + 1);
        }
        nextBeat(); return;
    }
    case 'R': revIsSymbol = false; revealTxt = formatReveal(a); revealAt = millis();
              holdUntil = millis() + 400; return;
    default:  nextBeat(); return;
    }
}

// ── lifecycle ────────────────────────────────────────────────────────────────
static void beginEffect(int i){
    if (!loadEffect(i)) { scr = SC_START; return; }
    sayLine = ""; promptTxt = ""; revealTxt = nullptr; pose = 0;
    scr = SC_PLAY;
    startBeat();
}

// ---- house rule: an app holds its working buffers ONLY while it is open ----
// Taken at the very start of Enter, freed at the very end of Shutdown, so a
// closed app costs no RAM and every other app (TLS especially) gets it back.
struct AppHeap_psychic {
    FxIndex fxIdx[MP_MAX_FX];
    char buf[MP_BUF];
    Beat beats[MP_MAX_BEATS];
    char qText[260];
    uint8_t chart[100];
};
static AppHeap_psychic* g_heap_psychic = nullptr;
static void heapUp_psychic(){
    if (!g_heap_psychic) g_heap_psychic = (AppHeap_psychic*)malloc(sizeof(AppHeap_psychic));
    if (!g_heap_psychic) return;
    memset(g_heap_psychic, 0, sizeof(AppHeap_psychic));
    fxIdx = g_heap_psychic->fxIdx;
    buf = g_heap_psychic->buf;
    beats = g_heap_psychic->beats;
    qText = g_heap_psychic->qText;
    chart = g_heap_psychic->chart;
}
static void heapDown_psychic(){
    free(g_heap_psychic); g_heap_psychic = nullptr;
    fxIdx = nullptr;
    buf = nullptr;
    beats = nullptr;
    qText = nullptr;
    chart = nullptr;
}

void mantisPsychicEnter(){
    heapUp_psychic();                       // first: everything below may use the buffers
    fb.setRotation(0);
    chordGuardReset();
    buildIndex();
    scr = SC_START; modeSel = 0; listSel = 0; listTop = 0;
    sayLine = ""; promptTxt = ""; revealTxt = nullptr;
    frameNo = 0; enterAt = millis();
    rng = millis() | 1u;
}
void mantisPsychicShutdown(){ /* the mantis remembers nothing */     heapDown_psychic();                     // last: after saves and radio teardown
}

void mantisPsychicTick(){
    chordGuardTick();
    frameNo++;
    uint32_t now = millis();
    // swallow the R press that selected this app from the menu
    if (now - enterAt < MP_ENTER_GUARD) return;

    bool lE = chordLEdge(), rE = chordREdge();

    switch (scr){
    case SC_START:
        if (lE) modeSel ^= 1;
        if (rE){
            if (modeSel == 0){
                // SOLO: the mantis chooses. One trick, then back here.
                if (soloCount > 0){
                    int want = (int)(pr() % (uint32_t)soloCount), seen = -1;
                    for (int i = 0; i < fxCount; i++){
                        if (fxIdx[i].performer) continue;
                        if (++seen == want){ beginEffect(i); break; }
                    }
                }
            } else {
                listPerformer = true; listSel = 0; listTop = 0; scr = SC_LIST;
            }
        }
        break;

    case SC_LIST: {
        int n = 0;
        for (int i = 0; i < fxCount; i++) if (fxIdx[i].performer == (listPerformer?1:0)) n++;
        if (n == 0){ scr = SC_START; break; }
        if (lE){ listSel = (listSel + 1) % n;
                 if (listSel < listTop) listTop = listSel;
                 if (listSel >= listTop + 6) listTop = listSel - 5;
                 if (listSel == 0) listTop = 0; }
        if (rE){
            int seen = -1;
            for (int i = 0; i < fxCount; i++){
                if (fxIdx[i].performer != (listPerformer?1:0)) continue;
                if (++seen == listSel){ beginEffect(i); break; }
            }
        }
        break; }

    case SC_PLAY: {
        if (holdUntil){                       // timed hold
            if (now >= holdUntil){ holdUntil = 0;
                if (beats[beatIdx].kind != 'R') nextBeat(); }
            break;
        }
        if (awaitPress){                      // a BEAT: carries no data at all
            if (lE || rE) nextBeat();
            break;
        }
        if (asking && askDigits){             // digit entry: L turns, R next
            bool held = chordLHeld();
            if (lE){ askD[askPos] = (uint8_t)((askD[askPos] + 1) % 10); askRepeat = now; askBad = false; }
            else if (held && now - askRepeat > 160){ askD[askPos] = (uint8_t)((askD[askPos] + 1) % 10); askRepeat = now; }
            if (rE){
                if (askPos < askNd - 1) askPos++;
                else {
                    long v = 0; for (int i = 0; i < askNd; i++) v = v*10 + askD[i];
                    if (v < askLo || v > askHi){ askBad = true; askPos = 0; }   // let them fix it
                    else { askVal = v; slots[askSlot] = v; nextBeat(); }
                }
            }
            break;
        }
        if (asking){                          // scroll a value, confirm with R
            bool held = chordLHeld();
            if (lE){ askVal++; if (askVal > askHi) askVal = askLo; askRepeat = now; }
            else if (held && now - askRepeat > 90){   // hold L to run the digits
                askVal++; if (askVal > askHi) askVal = askLo; askRepeat = now;
            }
            if (rE){ slots[askSlot] = askVal; nextBeat(); }
            break;
        }
        if (choosing){
            if (lE) choiceSel = (choiceSel + 1) % (choiceN ? choiceN : 1);
            if (rE) nextBeat();               // every option converges
            break;
        }
        if (beats[beatIdx].kind == 'R'){      // reveal is on screen; wait
            if (lE || rE) nextBeat();
            break;
        }
        nextBeat();
        break; }

    case SC_DONE:
        if ((now - revealAt) > 500 && (lE || rE)){
            scr = SC_START; sayLine = ""; revealTxt = nullptr;
        }
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  THE MANTIS - drawn procedurally, six poses, theme palette only
// ═══════════════════════════════════════════════════════════════════════════
static void drawMantis(int cx, int cy, uint8_t p, uint32_t t){
    float sway = sinf((float)t * 0.05f);
    uint16_t g = LIME, gd = LIME_DARK, gp = LIME_PALE;
    int lean = (int)(sway * 2.0f);

    // abdomen + thorax
    fb.fillRoundRect(cx - 7, cy - 2, 14, 22, 5, gd);
    fb.fillRoundRect(cx - 5, cy - 10, 10, 14, 4, g);

    // head, angled by pose
    int hx = cx + lean, hy = cy - 18;
    if (p == 2) hx = cx + 5 + lean;                  // gaze: turned toward you
    if (p == 5) hy = cy - 12;                        // bow: head dipped
    fb.fillTriangle(hx - 6, hy + 4, hx + 6, hy + 4, hx, hy - 5, g);

    // eyes - the reveal pose flares them
    uint16_t eye = (p == 4) ? PURPLE_BRIGHT : TEAL_DEEP;
    int er = (p == 4) ? 2 : 1;
    fb.fillCircle(hx - 3, hy + 1, er, eye);
    fb.fillCircle(hx + 3, hy + 1, er, eye);
    if (p == 4){                                     // reveal: eye-shine
        fb.drawCircle(hx - 3, hy + 1, 3, PURPLE_LIGHT);
        fb.drawCircle(hx + 3, hy + 1, 3, PURPLE_LIGHT);
    }

    // antennae
    int aw = (int)(sway * 3.0f);
    fb.drawLine(hx - 3, hy - 3, hx - 8 + aw, hy - 12, gd);
    fb.drawLine(hx + 3, hy - 3, hx + 8 - aw, hy - 12, gd);

    // raptorial forelegs - the pose is mostly in these
    switch (p){
    case 0:                                          // idle: folded, praying
        fb.drawLine(cx - 4, cy - 6, cx - 9, cy + 1, g);
        fb.drawLine(cx - 9, cy + 1, cx - 3, cy + 4, g);
        fb.drawLine(cx + 4, cy - 6, cx + 9, cy + 1, g);
        fb.drawLine(cx + 9, cy + 1, cx + 3, cy + 4, g);
        break;
    case 1:                                          // think: one at the head
        fb.drawLine(cx - 4, cy - 6, cx - 10, cy - 2 + aw, g);
        fb.drawLine(cx + 4, cy - 7, hx + 4, hy + 3, g);
        break;
    case 2:                                          // gaze: low and still
        fb.drawLine(cx - 4, cy - 4, cx - 12, cy + 3, g);
        fb.drawLine(cx + 4, cy - 4, cx + 12, cy + 3, g);
        break;
    case 3:                                          // raise: both lifted high
        fb.drawLine(cx - 4, cy - 7, cx - 12, cy - 18 + aw, g);
        fb.drawLine(cx - 12, cy - 18 + aw, cx - 8, cy - 24 + aw, gp);
        fb.drawLine(cx + 4, cy - 7, cx + 12, cy - 18 - aw, g);
        fb.drawLine(cx + 12, cy - 18 - aw, cx + 8, cy - 24 - aw, gp);
        break;
    case 4:                                          // reveal: thrown wide
        fb.drawLine(cx - 4, cy - 7, cx - 18, cy - 12, gp);
        fb.drawLine(cx + 4, cy - 7, cx + 18, cy - 12, gp);
        fb.drawLine(cx - 18, cy - 12, cx - 16, cy - 20, gp);
        fb.drawLine(cx + 18, cy - 12, cx + 16, cy - 20, gp);
        break;
    default:                                         // bow
        fb.drawLine(cx - 4, cy - 4, cx - 10, cy + 6, g);
        fb.drawLine(cx + 4, cy - 4, cx + 10, cy + 6, g);
        break;
    }

    // walking legs
    fb.drawLine(cx - 5, cy + 6,  cx - 12, cy + 14, gd);
    fb.drawLine(cx + 5, cy + 6,  cx + 12, cy + 14, gd);
    fb.drawLine(cx - 5, cy + 14, cx - 11, cy + 22, gd);
    fb.drawLine(cx + 5, cy + 14, cx + 11, cy + 22, gd);
}

// ── word-wrapped patter ──────────────────────────────────────────────────────
static void wrapText(const char* txt, int x, int y, int maxW, int lineH, int font){
    char line[42]; line[0] = '\0';
    int li = 0, cy = y;
    const char* p = txt;
    while (*p){
        const char* sp = strchr(p, ' ');
        int wl = sp ? (int)(sp - p) : (int)strlen(p);
        if (wl > 40) wl = 40;
        char word[42]; memcpy(word, p, wl); word[wl] = '\0';
        char test[84];
        snprintf(test, sizeof(test), "%s%s%s", line, li ? " " : "", word);
        if (fb.textWidth(test, font) > maxW && li > 0){
            fb.drawString(line, x, cy, font);
            cy += lineH;
            snprintf(line, sizeof(line), "%s", word);
            li = (int)strlen(line);
        } else {
            snprintf(line, sizeof(line), "%s", test);
            li = (int)strlen(line);
        }
        if (!sp) break;
        p = sp + 1;
    }
    if (li) fb.drawString(line, x, cy, font);
}

// ── screens ──────────────────────────────────────────────────────────────────
static void drawStart(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(PURPLE_LIGHT); fb.drawString("MANTIS",W/2,14,4);
    fb.setTextColor(LIME_PALE);    fb.drawString("PSYCHIC",W/2,44,4);
    drawMantis(W/2, 130, (frameNo/60)%2 ? 0 : 2, frameNo);
    const char* opt[2]  = {"AMAZE ME", "TEACH ME A TRICK"};
    const char* note[2] = {"the mantis picks one", "tricks to perform on people"};
    for (int i = 0; i < 2; i++){
        int y = 196 + i*52; bool s = (i == modeSel);
        fb.fillRoundRect(12, y, W-24, 44, 5, s ? PURPLE_DARK : TEAL_DARK);
        fb.drawRoundRect(12, y, W-24, 44, 5, s ? LIME : TEAL);
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(s ? LIME_PALE : TEAL_LIGHT);
        fb.drawString(opt[i], W/2, y + 8, 2);
        fb.setTextColor(s ? TEAL_LIGHT : TEAL);
        fb.drawString(note[i], W/2, y + 28, 1);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("L: switch   R: begin", W/2, H-4, 1);
}

static void drawList(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("TRICKS TO LEARN", W/2, 8, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("self-working - they always land", W/2, 28, 1);
    int row = 0, seen = -1;
    for (int i = 0; i < fxCount && row < 6; i++){
        if (fxIdx[i].performer != (listPerformer?1:0)) continue;
        seen++;
        if (seen < listTop) continue;
        int y = 50 + row*38; bool s = (seen == listSel);
        fb.fillRoundRect(10, y, W-20, 32, 4, s ? PURPLE_DARK : TEAL_DARK);
        if (s) fb.drawRoundRect(10, y, W-20, 32, 4, LIME);
        fb.setTextDatum(ML_DATUM);
        fb.setTextColor(s ? LIME_PALE : TEAL_LIGHT);
        fb.drawString(fxIdx[i].title, 18, y + 16, 2);
        row++;
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("L: next   R: learn it", W/2, H-4, 1);
}

static void drawChart(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("find your number", W/2, 2, 1);
    for (int v = 0; v < 100; v++){
        int col = v % 10, row = v / 10;
        int x = col * 17, y = 14 + row * 29;
        char b[4]; snprintf(b, sizeof(b), "%d", v);
        fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString(b, x + 8, y, 1);
        drawSymbol(chart[v], x + 8, y + 18, 5, (v % 2) ? LIME : PURPLE_LIGHT);
    }
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL);
    fb.drawString(promptTxt, W/2, H-2, 1);
}

static void drawCountedText(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString("the mantis's page", W/2, 6, 1);
    fb.drawFastHLine(10, 18, W - 20, TEAL_DARK);
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    wrapText(qText, 8, 28, W - 16, 22, 2);
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL);
    fb.drawString(promptTxt, W/2, H-2, 1);
}

static void drawPlay(){
    if (chartOn) { drawChart(); return; }
    if (textOn)  { drawCountedText(); return; }
    fb.fillSprite(TEAL_DEEP);
    // a faint pulse behind the mantis on the reveal
    if (beats[beatIdx].kind == 'R'){
        uint16_t c = ((frameNo/5)%2) ? TEAL_DARK : TEAL_DEEP;
        fb.fillRect(0, 0, W, 150, c);
    }
    drawMantis(W/2, 84, pose, frameNo);

    // patter
    fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_PALE);
    if (sayLine[0]) wrapText(sayLine, 8, 132, W-16, 16, 2);

    // the reveal, big
    if (revealTxt){
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(((frameNo/6)%2) ? LIME : LIME_PALE);
        int font = (fb.textWidth(revealTxt, 4) <= W-12) ? 4 : 2;
        wrapText(revealTxt, 6, 210, W-12, 22, font);
        if (revIsSymbol) drawSymbol(chartTarget, W/2, 268, 18, ((frameNo/6)%2) ? LIME : LIME_PALE);
    }

    // value ask
    if (asking && askDigits){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(askBad ? PURPLE_LIGHT : TEAL_LIGHT);
        fb.drawString(askBad ? "that can't be right - check it" : promptTxt, W/2, 200, 1);
        int bw = 26, x0 = W/2 - (askNd*bw)/2;
        for (int i = 0; i < askNd; i++){
            bool s = (i == askPos);
            fb.fillRoundRect(x0 + i*bw + 1, 214, bw - 2, 36, 4, s ? PURPLE_DARK : TEAL_DARK);
            if (s) fb.drawRoundRect(x0 + i*bw + 1, 214, bw - 2, 36, 4, LIME);
            char d[2] = { (char)('0' + askD[i]), 0 };
            fb.setTextDatum(MC_DATUM); fb.setTextColor(s ? LIME_PALE : TEAL_LIGHT);
            fb.drawString(d, x0 + i*bw + bw/2, 232, 4);
        }
    } else if (asking){
        fb.fillRoundRect(20, 206, W-40, 46, 5, TEAL_DARK);
        fb.drawRoundRect(20, 206, W-40, 46, 5, LIME);
        fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString(promptTxt, W/2, 210, 1);
        fb.setTextColor(LIME_PALE);
        char b[16]; snprintf(b, sizeof(b), "%ld", askVal);
        fb.drawString(b, W/2, 224, 4);
    }
    // choice
    if (choosing){
        fb.setTextDatum(TC_DATUM); fb.setTextColor(TEAL_LIGHT);
        fb.drawString(promptTxt, W/2, 200, 1);
        for (int i = 0; i < choiceN; i++){
            int bw = (W - 24) / (choiceN ? choiceN : 1);
            int x = 12 + i*bw; bool s = (i == choiceSel);
            fb.fillRoundRect(x+2, 216, bw-4, 30, 4, s ? PURPLE_DARK : TEAL_DARK);
            if (s) fb.drawRoundRect(x+2, 216, bw-4, 30, 4, LIME);
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(s ? LIME_PALE : TEAL_LIGHT);
            fb.drawString(choiceLbl[i] ? choiceLbl[i] : "", x + bw/2, 231, 1);
        }
    }

    // footer
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    if (awaitPress)      fb.drawString(promptTxt, W/2, H-4, 1);
    else if (asking && askDigits) fb.drawString("L: change digit   R: next digit", W/2, H-4, 1);
    else if (asking)     fb.drawString("L: scroll (hold to run)   R: that's it", W/2, H-4, 1);
    else if (choosing)   fb.drawString("L: move   R: touch it", W/2, H-4, 1);
    else if (revealTxt)  fb.drawString("press to go on", W/2, H-4, 1);
}

static void drawDone(){
    fb.fillSprite(TEAL_DEEP);
    drawMantis(W/2, 110, 5, frameNo);                 // bow
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(PURPLE_LIGHT); fb.drawString("THE MANTIS BOWS", W/2, 168, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("it has already forgotten", W/2, 194, 1);
    fb.drawString("everything about you", W/2, 208, 1);
    fb.setTextDatum(BC_DATUM); fb.setTextColor(TEAL_DARK);
    fb.drawString("press for another", W/2, H-4, 1);
}

void mantisPsychicDraw(){
    switch (scr){
    case SC_START: drawStart(); break;
    case SC_LIST:  drawList();  break;
    case SC_PLAY:  drawPlay();  break;
    default:       drawDone();  break;
    }
    fb.pushSprite(0, 0);
}
