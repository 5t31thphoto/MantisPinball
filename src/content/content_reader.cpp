// content_reader.cpp - Generic offline article/rabbit-hole reader engine.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
#include "content_reader.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;

static constexpr int W = 170;
static constexpr int H = 320;

// ---- Loaded pack state -------------------------------------------------------
struct CREntry {
    char     id[CR_ID_LEN];
    char     title[CR_TITLE_LEN];
    uint32_t bodyOffset;   // byte offset into the pack file where body begins
    uint16_t bodyLen;      // length of body text
    // Branch choices (CYOA) / index links
    uint8_t  choiceCount;
    char     choiceLabel[CR_MAX_CHOICES][CR_TITLE_LEN];
    char     choiceGoto[CR_MAX_CHOICES][CR_ID_LEN];
    uint8_t  choiceBtn[CR_MAX_CHOICES];   // 0 = L, 1 = R
    // Sequence steps
    uint8_t  stepCount;
    uint32_t stepOffset[16];   // offsets of each @STEP line text
    uint16_t stepLen[16];
};

static const ContentMode* crMode = nullptr;
static fs::File    crFile;
static bool        crFsReady = false;
static bool        crReady   = false;

static CREntry     crEntries[CR_MAX_ENTRIES];
static int         crEntryCount = 0;

static int         crCurrent   = 0;   // current entry index
static int         crStepIdx   = 0;   // current step (sequence mode)

// Index list scrolling (topic mode)
static int         crListTop   = 0;   // first visible entry in the index
static int         crListSel   = 0;   // highlighted entry in the index
static bool         crInIndex  = true; // topic mode: showing index vs an entry

// Body text scrolling
static char        crBody[CR_MAX_BODY];
static int         crBodyLen   = 0;
// Wrapped lines
static constexpr int CR_MAXLINES = 220;
static char        crLines[CR_MAXLINES][32];
static int         crLineCount = 0;
static int         crScroll    = 0;
static constexpr int CR_VISIBLE = 20;   // body lines visible at once
static uint32_t    crScrollAt   = 0;

// ---- Small helpers -----------------------------------------------------------
static void crWrapBody() {
    crLineCount = 0;
    int i = 0;
    while (i < crBodyLen && crLineCount < CR_MAXLINES) {
        // Handle explicit newlines
        if (crBody[i] == '\n') { crLines[crLineCount][0] = '\0'; crLineCount++; i++; continue; }
        // Greedy word wrap. Font-1 glyphs are ~6px wide; the panel is 170px
        // with a ~4px left margin, so 27 chars (~162px) is the safe maximum.
        // (Rabbit Hole uses 27 and never clips; the old value of 30 overran the
        // right edge and cut words off.)
        int lineStart = i, lastSpace = -1, col = 0;
        while (i < crBodyLen && crBody[i] != '\n' && col < 27) {
            if (crBody[i] == ' ') lastSpace = i;
            i++; col++;
        }
        int end = i;
        if (i < crBodyLen && crBody[i] != '\n' && lastSpace > lineStart) {
            end = lastSpace;
            i = lastSpace + 1;
        }
        int n = end - lineStart;
        if (n > 27) n = 27;
        memcpy(crLines[crLineCount], crBody + lineStart, n);
        crLines[crLineCount][n] = '\0';
        crLineCount++;
    }
    crScroll = 0;
}

// Read a span of bytes from the pack file into crBody
static void crLoadBody(uint32_t off, uint16_t len) {
    if (!crFsReady) { crBody[0] = '\0'; crBodyLen = 0; return; }
    if (len >= CR_MAX_BODY) len = CR_MAX_BODY - 1;
    crFile.seek(off);
    crBodyLen = crFile.read((uint8_t*)crBody, len);
    if (crBodyLen < 0) crBodyLen = 0;
    crBody[crBodyLen] = '\0';
    crWrapBody();
}

// ---- Pack parsing ------------------------------------------------------------
// Single pass: index entry headers, choices, steps, body spans.
static bool crParsePack() {
    crEntryCount = 0;
    if (!crFsReady) return false;
    crFile.seek(0);

    String line;
    int    curEntry = -1;
    long   bodyStart = -1;

    auto finalizeBody = [&](long endPos) {
        if (curEntry >= 0 && bodyStart >= 0) {
            long len = endPos - bodyStart;
            if (len < 0) len = 0;
            if (len > 65000) len = 65000;
            crEntries[curEntry].bodyOffset = (uint32_t)bodyStart;
            crEntries[curEntry].bodyLen    = (uint16_t)len;
        }
        bodyStart = -1;
    };

    while (crFile.available()) {
        long lineStart = crFile.position();
        line = crFile.readStringUntil('\n');
        // Strip trailing CR
        if (line.endsWith("\r")) line.remove(line.length() - 1);

        if (line.startsWith("@PACK")) {
            continue;
        } else if (line.startsWith("@ENTRY")) {
            finalizeBody(lineStart);
            if (crEntryCount >= CR_MAX_ENTRIES) break;
            curEntry = crEntryCount++;
            CREntry& e = crEntries[curEntry];
            e.id[0] = e.title[0] = '\0';
            e.choiceCount = e.stepCount = 0;
            e.bodyOffset = 0; e.bodyLen = 0;
            // Parse "@ENTRY id|title"
            int sp = line.indexOf(' ');
            String rest = (sp >= 0) ? line.substring(sp + 1) : "";
            int bar = rest.indexOf('|');
            String id    = (bar >= 0) ? rest.substring(0, bar) : rest;
            String title = (bar >= 0) ? rest.substring(bar + 1) : rest;
            id.trim(); title.trim();
            strncpy(e.id, id.c_str(), CR_ID_LEN - 1);
            strncpy(e.title, title.c_str(), CR_TITLE_LEN - 1);
            bodyStart = crFile.position();   // body begins after this line
        } else if (line.startsWith("@CHOICE") && curEntry >= 0) {
            // body ends where the first @CHOICE/@STEP/@END begins
            if (bodyStart >= 0) { finalizeBody(lineStart); }
            CREntry& e = crEntries[curEntry];
            if (e.choiceCount < CR_MAX_CHOICES) {
                // "@CHOICE L|label|goto"
                int sp = line.indexOf(' ');
                String rest = (sp >= 0) ? line.substring(sp + 1) : "";
                int b1 = rest.indexOf('|');
                int b2 = rest.indexOf('|', b1 + 1);
                String btn  = rest.substring(0, b1);
                String lbl  = rest.substring(b1 + 1, b2);
                String go   = rest.substring(b2 + 1);
                btn.trim(); lbl.trim(); go.trim();
                int c = e.choiceCount++;
                e.choiceBtn[c] = (btn == "R" || btn == "r") ? 1 : 0;
                strncpy(e.choiceLabel[c], lbl.c_str(), CR_TITLE_LEN - 1);
                strncpy(e.choiceGoto[c],  go.c_str(),  CR_ID_LEN - 1);
            }
        } else if (line.startsWith("@STEP") && curEntry >= 0) {
            if (bodyStart >= 0) { finalizeBody(lineStart); }
            CREntry& e = crEntries[curEntry];
            if (e.stepCount < 16) {
                int sp = line.indexOf(' ');
                long textOff = lineStart + (sp >= 0 ? sp + 1 : 5);
                e.stepOffset[e.stepCount] = (uint32_t)textOff;
                e.stepLen[e.stepCount] = (uint16_t)(line.length() - (sp >= 0 ? sp + 1 : 5));
                e.stepCount++;
            }
        } else if (line.startsWith("@END")) {
            finalizeBody(lineStart);
            curEntry = -1;
        }
    }
    finalizeBody(crFile.position());
    return crEntryCount > 0;
}

static int crFindEntry(const char* id) {
    for (int i = 0; i < crEntryCount; i++)
        if (strcmp(crEntries[i].id, id) == 0) return i;
    return -1;
}

// ---- Public API --------------------------------------------------------------
bool contentReady() { return crReady; }

void contentEnter(const ContentMode* mode) {
    crMode = mode;
    crReady = false;
    crEntryCount = 0;
    crCurrent = 0; crStepIdx = 0;
    crListTop = crListSel = 0;
    crInIndex = (mode->kind == CK_TOPICS || mode->kind == CK_SEQUENCE);
    crScroll = 0;

    if (!crFsReady) {
        // Our LittleFS data partition is labeled "content" (see the custom
        // partition table). Mount it explicitly; format on first boot.
        crFsReady = LittleFS.begin(true, "/littlefs", 10, "content");
    }
    if (crFsReady) {
        if (crFile) crFile.close();
        crFile = LittleFS.open(mode->packPath, "r");
        if (crFile && crFile.size() > 0) {
            if (crParsePack()) {
                crReady = true;
                crCurrent = 0;
                if (mode->kind == CK_BRANCH) {
                    crLoadBody(crEntries[0].bodyOffset, crEntries[0].bodyLen);
                }
            }
        }
    }
}

void contentShutdown() {
    if (crFile) crFile.close();
    crReady = false;
    crMode = nullptr;
}

// Navigation for topic modes (index list + entry view).
// Matches the main menu convention: L steps the selection, R opens/acts.
static void crTickTopics() {
    bool rEdge = btnR && !btnR_prev;
    bool lEdge = btnL && !btnL_prev;

    if (crInIndex) {
        // L steps the highlight down the list (wraps). R opens the entry.
        if (lEdge) {
            crListSel = (crListSel + 1) % crEntryCount;
            if (crListSel < crListTop) crListTop = crListSel;
            if (crListSel >= crListTop + CR_VISIBLE) crListTop = crListSel - CR_VISIBLE + 1;
        }
        if (rEdge) {
            crCurrent = crListSel;
            crLoadBody(crEntries[crCurrent].bodyOffset, crEntries[crCurrent].bodyLen);
            crInIndex = false;
        }
    } else {
        // In an entry: R scrolls body down (wraps), L returns to index.
        if (rEdge) {
            if (crScroll + CR_VISIBLE < crLineCount) crScroll++;
            else crScroll = 0;   // wrap to top
        }
        if (lEdge) {
            crInIndex = true;
        }
    }
}

// Navigation for sequence modes (index -> step-by-step).
// L steps the index selection / goes back a step; R opens / advances a step.
static void crTickSequence() {
    bool rEdge = btnR && !btnR_prev;
    bool lEdge = btnL && !btnL_prev;

    if (crInIndex) {
        if (lEdge) {
            crListSel = (crListSel + 1) % crEntryCount;
            if (crListSel < crListTop) crListTop = crListSel;
            if (crListSel >= crListTop + CR_VISIBLE) crListTop = crListSel - CR_VISIBLE + 1;
        }
        if (rEdge) {
            crCurrent = crListSel;
            crStepIdx = 0;
            crInIndex = false;
        }
    } else {
        CREntry& e = crEntries[crCurrent];
        if (rEdge) {
            if (crStepIdx + 1 < e.stepCount) crStepIdx++;
        }
        if (lEdge) {
            if (crStepIdx > 0) crStepIdx--;
            else crInIndex = true;   // back past step 1 -> index
        }
    }
}

// Navigation for branch modes (L/R choices)
static void crTickBranch() {
    bool rEdge = btnR && !btnR_prev;
    bool lEdge = btnL && !btnL_prev;
    CREntry& e = crEntries[crCurrent];

    // Find which choice each button maps to
    auto take = [&](int wantBtn) {
        for (int i = 0; i < e.choiceCount; i++) {
            if (e.choiceBtn[i] == wantBtn) {
                int dst = crFindEntry(e.choiceGoto[i]);
                if (dst >= 0) {
                    crCurrent = dst;
                    crLoadBody(crEntries[dst].bodyOffset, crEntries[dst].bodyLen);
                }
                return;
            }
        }
        // No choice on this button: if it's a terminal page, restart story
        if (e.choiceCount == 0) {
            crCurrent = 0;
            crLoadBody(crEntries[0].bodyOffset, crEntries[0].bodyLen);
        }
    };
    if (lEdge) take(0);
    if (rEdge) take(1);
}

void contentTick() {
    if (!crReady || !crMode) return;
    switch (crMode->kind) {
        case CK_TOPICS:   crTickTopics();   break;
        case CK_SEQUENCE: crTickSequence(); break;
        case CK_BRANCH:   crTickBranch();   break;
    }
}

// ---- Rendering ---------------------------------------------------------------
static void crDrawHeader(const char* txt) {
    fb.fillRect(0, 0, W, 16, thc(crMode->accent));
    fb.setTextDatum(ML_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString(txt, 4, 8, 1);
}

static void crDrawFooter(const char* lhint, const char* rhint) {
    fb.fillRect(0, H - 14, W, 14, TEAL_DARK);
    fb.setTextColor(LIME);
    fb.setTextDatum(ML_DATUM); fb.drawString(lhint, 3, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString(rhint, W - 3, H - 7, 1);
}

static void crDrawIndex() {
    crDrawHeader(crMode->title);
    fb.setTextDatum(TL_DATUM);
    int y = 20;
    for (int i = crListTop; i < crEntryCount && i < crListTop + CR_VISIBLE; i++) {
        bool sel = (i == crListSel);
        if (sel) {
            fb.fillRect(2, y - 1, W - 4, 13, PURPLE);
            fb.setTextColor(LIME_PALE);
        } else {
            fb.setTextColor(TEAL_LIGHT);
        }
        char row[34];
        snprintf(row, sizeof(row), "%s", crEntries[i].title);
        fb.drawString(row, 5, y, 1);
        y += 14;
    }
    // Scroll indicator
    if (crEntryCount > CR_VISIBLE) {
        int barH = (H - 40) * CR_VISIBLE / crEntryCount;
        int barY = 20 + (H - 40) * crListTop / crEntryCount;
        fb.fillRect(W - 3, barY, 2, barH, TEAL_BRIGHT);
    }
    crDrawFooter("L: scroll", "R: open");
}

static void crDrawBody() {
    crDrawHeader(crEntries[crCurrent].title);
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(TEAL_LIGHT);
    int y = 20;
    for (int i = crScroll; i < crLineCount && i < crScroll + CR_VISIBLE; i++) {
        fb.drawString(crLines[i], 4, y, 1);
        y += 14;
    }
    if (crLineCount > CR_VISIBLE) {
        int barH = (H - 40) * CR_VISIBLE / crLineCount;
        int barY = 20 + (H - 40) * crScroll / crLineCount;
        fb.fillRect(W - 3, barY, 2, barH, TEAL_BRIGHT);
    }
    crDrawFooter("L: back", "R: scroll");
}

static void crDrawStep() {
    CREntry& e = crEntries[crCurrent];
    crDrawHeader(e.title);
    // Step number
    char sn[24]; snprintf(sn, sizeof(sn), "STEP %d / %d", crStepIdx + 1, e.stepCount);
    fb.setTextDatum(TC_DATUM); fb.setTextColor(LIME);
    fb.drawString(sn, W / 2, 22, 2);
    // Step text (load + wrap on the fly)
    if (e.stepCount > 0) {
        crLoadBody(e.stepOffset[crStepIdx], e.stepLen[crStepIdx]);
        fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
        int y = 48;
        for (int i = 0; i < crLineCount && i < CR_VISIBLE; i++) {
            fb.drawString(crLines[i], 6, y, 1);
            y += 15;
        }
    }
    crDrawFooter(crStepIdx > 0 ? "L: prev" : "L: list",
                 crStepIdx + 1 < e.stepCount ? "R: next" : "(end)");
}

static void crDrawBranch() {
    CREntry& e = crEntries[crCurrent];
    fb.fillRect(0, 0, W, 16, thc(crMode->accent));
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(crMode->title, 4, 8, 1);
    // Story body
    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
    int y = 22;
    int avail = (e.choiceCount > 0) ? 14 : CR_VISIBLE;  // leave room for choices
    for (int i = crScroll; i < crLineCount && i < crScroll + avail; i++) {
        fb.drawString(crLines[i], 4, y, 1);
        y += 14;
    }
    // Choices
    int cy = H - 50;
    fb.drawFastHLine(0, cy - 4, W, TEAL_DARK);
    for (int i = 0; i < e.choiceCount; i++) {
        const char* tag = (e.choiceBtn[i] == 0) ? "L:" : "R:";
        uint16_t col = (e.choiceBtn[i] == 0) ? TEAL_BRIGHT : PURPLE_LIGHT;
        fb.setTextColor(col); fb.setTextDatum(TL_DATUM);
        char row[40]; snprintf(row, sizeof(row), "%s %s", tag, e.choiceLabel[i]);
        fb.drawString(row, 4, cy + i * 16, 1);
    }
    if (e.choiceCount == 0) {
        fb.setTextColor(LIME); fb.setTextDatum(MC_DATUM);
        fb.drawString("THE END - press to restart", W / 2, H - 20, 1);
    }
}

void contentDraw() {
    fb.fillSprite(TEAL_DEEP);
    if (!crReady) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("content not", W / 2, H / 2 - 10, 2);
        fb.drawString("installed", W / 2, H / 2 + 10, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("flash content pack", W / 2, H / 2 + 36, 1);
        fb.pushSprite(0, 0);
        return;
    }
    switch (crMode->kind) {
        case CK_TOPICS:
            if (crInIndex) crDrawIndex(); else crDrawBody();
            break;
        case CK_SEQUENCE:
            if (crInIndex) crDrawIndex(); else crDrawStep();
            break;
        case CK_BRANCH:
            crDrawBranch();
            break;
    }
    fb.pushSprite(0, 0);
}
