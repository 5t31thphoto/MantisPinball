// cyoa_mode.cpp - Choose-Your-Own-Adventure + collectible-dice skill checks.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <FS.h>
#include <string.h>
#include "cyoa_mode.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
#include "../chord_guard.h"
static constexpr int W = 170;
static constexpr int H = 320;

// ── Story model ───────────────────────────────────────────────────────────────
// The story is loaded from /content/cyoa.pak. Format (one page per @PAGE):
//
//   @PAGE <id>
//   <body text, any number of lines>
//   @GO <L|R>|<label>|<goto-id>                 plain choice
//   @ROLL <L|R>|<label>|<target>|<tag>|<win-id>|<lose-id>   skill check
//   @XP <amount>                                 grant XP when page is reached
//   @UNLOCK <flagname>                           grant a reward when reached
//   @END                                         terminal page (offers restart)
//
// tag is one of: none, agility, courage, wits  (matches special dice bonuses)
static constexpr int CY_MAX_PAGES   = 120;
static constexpr int CY_ID_LEN      = 20;
static constexpr int CY_LABEL_LEN   = 40;
static constexpr int CY_MAX_CHOICES = 2;
static constexpr int CY_MAX_LINES   = 24;
static constexpr int CY_LINE_LEN    = 28;   // chars per wrapped line (4px margin + 27 glyphs ~= 166px, fits 170)

struct CyChoice {
    uint8_t btn;        // 0 = L, 1 = R
    uint8_t isRoll;     // 0 = plain @GO, 1 = @ROLL skill check
    char    label[CY_LABEL_LEN];
    char    gotoId[CY_ID_LEN];   // plain target (or win target for rolls)
    char    failId[CY_ID_LEN];   // roll fail target
    uint8_t target;              // roll target number
    uint8_t tag;                 // 0 none, 1 agility, 2 courage, 3 wits
    int8_t  gateItem;            // -1 none, else ItemId required+consumed to take this choice
};

struct CyPage {
    char     id[CY_ID_LEN];
    uint32_t bodyOffset;
    uint32_t bodyLen;
    uint8_t  choiceCount;
    CyChoice choices[CY_MAX_CHOICES];
    int32_t  grantXp;
    int8_t   grantItem;     // ItemId to add, or -1
    uint8_t  grantDieSides;  // 0 = none
    uint8_t  grantDieAttr;
    int8_t   hpDelta;        // change to story HP on reaching the page
    uint8_t  isEnd;
};

static CyPage  cyPages[CY_MAX_PAGES];
static int     cyPageCount = 0;
static bool    cyReady     = false;
static int     cyCurrent   = 0;
static char    cyPakPath[40] = "/content/cyoa.pak";

// The story library. Each entry maps a CYOA pack to its unlock flag. The first
// (Hollow Road) is always available; the rest unlock via rewards.
struct CyStory { uint32_t flag; const char* path; const char* title; const char* how; };
static const CyStory CY_STORIES[] = {
    { STORY_HOLLOW,   "/content/cyoa.pak",     "The Hollow Road",       "" },
    { STORY_SIGNAL,   "/content/signal.pak",   "Signal from the Ridge", "reach level 10" },
    { STORY_DERELICT, "/content/derelict.pak", "The Derelict Array",    "7-day quest streak" },
    { STORY_TIDE,     "/content/tide.pak",     "The Drowned Reliquary", "unlock all fish" },
};
static const int CY_STORY_COUNT = sizeof(CY_STORIES)/sizeof(CY_STORIES[0]);
static int cySelIdx = 0;   // story menu cursor

// Wrapped body lines for the current page
static char    cyLines[CY_MAX_LINES][CY_LINE_LEN];
static int     cyLineCount = 0;
static int     cyScroll    = 0;

// Roll overlay state
enum CyMode { CY_SELECT, CY_PAGE, CY_ROLLING, CY_RESULT };
static CyMode  cyMode      = CY_PAGE;
static int     cyRollTotal = 0;
static char    cyAssist[28] = "";     // "LUCK CHARM +2" / "REROLL!" / heal note
static uint32_t cyAssistAt  = 0;
static int     cyRollTarget= 0;
static bool    cyRollWin   = false;
static char    cyRollBreak[40] = "";
static char    cyResultGoto[CY_ID_LEN] = "";
static uint32_t cyRollAnimAt = 0;
static int     cyRollAnimFace = 0;

// XP/unlock awards are applied once per page visit; track to avoid re-award on
// scroll/redraw.
static bool    cyAwarded = false;

// Story survival: the player has HP within a story run (separate from profile
// level). Rations/medkits restore it; hazards reduce it; 0 HP ends the run.
static int     cyHp     = 10;
static const int CY_HP_MAX = 10;
static bool    cyDead   = false;

// ── Tag helpers ───────────────────────────────────────────────────────────────
static uint8_t tagFromName(const char* s) {
    if (!strcmp(s, "agility")) return 1;
    if (!strcmp(s, "courage")) return 2;
    if (!strcmp(s, "wits"))    return 3;
    return 0;
}
static const char* tagToName(uint8_t t) {
    switch (t) { case 1: return "agility"; case 2: return "courage";
                 case 3: return "wits"; default: return ""; }
}
// Map an item name (used in @ITEM lines) to an ItemId in the shared inventory.
static int itemFromName(const char* s) {
    if (!strcmp(s, "torch"))     return ITEM_CY_TORCH;
    if (!strcmp(s, "rope"))      return ITEM_CY_ROPE;
    if (!strcmp(s, "ration"))    return ITEM_CY_RATION;
    if (!strcmp(s, "medkit"))    return ITEM_CY_MEDKIT;
    if (!strcmp(s, "luckcharm")) return ITEM_CY_LUCKCHARM;
    if (!strcmp(s, "multitool")) return ITEM_CY_MULTITOOL;
    if (!strcmp(s, "reroll"))    return ITEM_CY_REROLL;
    return -1;
}
// Map a die-attribute name to its DieAttr.
static uint8_t dieAttrFromName(const char* s) {
    if (!strcmp(s, "lucky")) return DA_LUCKY;
    if (!strcmp(s, "keen"))  return DA_KEEN;
    if (!strcmp(s, "heavy")) return DA_HEAVY;
    if (!strcmp(s, "wild"))  return DA_WILD;
    if (!strcmp(s, "swift")) return DA_SWIFT;
    if (!strcmp(s, "brave")) return DA_BRAVE;
    if (!strcmp(s, "wise"))  return DA_WISE;
    return DA_NONE;
}

static int cyFindPage(const char* id) {
    for (int i = 0; i < cyPageCount; i++)
        if (strncmp(cyPages[i].id, id, CY_ID_LEN) == 0) return i;
    return -1;
}

// Split a "a|b|c" field line into up to n parts (in place into out pointers).
// Returns count. Modifies buf.
static int splitPipe(char* buf, char** out, int n) {
    int c = 0; out[c++] = buf;
    for (char* p = buf; *p && c < n; p++) {
        if (*p == '|') { *p = '\0'; out[c++] = p + 1; }
    }
    return c;
}

// ── Pack parsing ──────────────────────────────────────────────────────────────
// First pass: read the whole file into a heap buffer, index pages and choices,
// record body spans (offset/len into the buffer). The buffer stays resident so
// body text can be re-wrapped on demand.
static char*   cyBuf    = nullptr;
static size_t  cyBufLen = 0;

static void cyFreeBuf() {
    if (cyBuf) { free(cyBuf); cyBuf = nullptr; cyBufLen = 0; }
}

static bool cyFsReady = false;

static bool cyParse() {
    cyPageCount = 0;
    cyReady = false;
    cyFreeBuf();

    // The content pack lives on the LittleFS partition labeled "content" (see
    // the custom partition table). It MUST be mounted explicitly with that
    // label before opening files — otherwise the open fails and the story shows
    // as "not installed" even when the pack was flashed. This mirrors how the
    // content reader (scout/survival/first-aid) mounts it.
    if (!cyFsReady) {
        cyFsReady = LittleFS.begin(true, "/littlefs", 10, "content");
    }
    if (!cyFsReady) return false;

    fs::File f = LittleFS.open(cyPakPath, "r");
    if (!f) return false;
    size_t sz = f.size();
    if (sz == 0 || sz > 60000) { f.close(); return false; }
    cyBuf = (char*)malloc(sz + 1);
    if (!cyBuf) { f.close(); return false; }
    size_t rd = f.read((uint8_t*)cyBuf, sz);
    f.close();
    cyBuf[rd] = '\0';
    cyBufLen = rd;

    int cur = -1;
    size_t i = 0;
    bool inBody = false;
    while (i < rd) {
        // line = [lineStart, lineEnd)
        size_t lineStart = i;
        while (i < rd && cyBuf[i] != '\n') i++;
        size_t lineEnd = i;            // points at '\n' or end
        size_t nextLine = (i < rd) ? i + 1 : rd;
        // Make a temporary NUL-terminated view by remembering the char
        char saved = cyBuf[lineEnd];
        cyBuf[lineEnd] = '\0';
        char* line = &cyBuf[lineStart];
        // strip trailing CR
        size_t ll = strlen(line);
        if (ll && line[ll-1] == '\r') { line[ll-1] = '\0'; ll--; }

        if (strncmp(line, "@PAGE", 5) == 0) {
            if (cyPageCount >= CY_MAX_PAGES) { cyBuf[lineEnd] = saved; break; }
            cur = cyPageCount++;
            CyPage& pg = cyPages[cur];
            memset(&pg, 0, sizeof(pg));
            pg.grantItem = -1;   // 0 is a valid ItemId, so -1 means "none"
            const char* idp = line + 5; while (*idp == ' ') idp++;
            strncpy(pg.id, idp, CY_ID_LEN - 1);
            pg.bodyOffset = nextLine;   // body starts on the next line
            pg.bodyLen = 0;
            inBody = true;
        } else if (cur >= 0 && strncmp(line, "@GO", 3) == 0) {
            inBody = false;
            CyPage& pg = cyPages[cur];
            if (pg.choiceCount < CY_MAX_CHOICES) {
                char tmp[80]; strncpy(tmp, line + 3, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
                char* parts[4]; char* s = tmp; while (*s==' ') s++;
                int n = splitPipe(s, parts, 4);
                if (n >= 3) {
                    CyChoice& c = pg.choices[pg.choiceCount++];
                    c.btn = (parts[0][0] == 'R' || parts[0][0] == 'r') ? 1 : 0;
                    c.isRoll = 0;
                    c.gateItem = -1;
                    strncpy(c.label, parts[1], CY_LABEL_LEN-1);
                    strncpy(c.gotoId, parts[2], CY_ID_LEN-1);
                }
            }
        } else if (cur >= 0 && strncmp(line, "@USE", 4) == 0) {
            // @USE <L|R>|<item>|<label>|<have-id>|<lack-id>
            // A hidden inventory check woven into the choices. The player always
            // sees a normal-looking choice; if they hold <item> it is consumed
            // and they go to <have-id>; if not, they're dumped to <lack-id>
            // (a real consequence, like a failed roll). Never greys out.
            inBody = false;
            CyPage& pg = cyPages[cur];
            if (pg.choiceCount < CY_MAX_CHOICES) {
                char tmp[120]; strncpy(tmp, line + 4, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
                char* parts[5]; char* s = tmp; while (*s==' ') s++;
                int n = splitPipe(s, parts, 5);
                if (n >= 5) {
                    CyChoice& c = pg.choices[pg.choiceCount++];
                    c.btn = (parts[0][0] == 'R' || parts[0][0] == 'r') ? 1 : 0;
                    c.isRoll = 0;
                    c.gateItem = (int8_t)itemFromName(parts[1]);
                    strncpy(c.label,  parts[2], CY_LABEL_LEN-1);
                    strncpy(c.gotoId, parts[3], CY_ID_LEN-1);   // have item -> here
                    strncpy(c.failId, parts[4], CY_ID_LEN-1);   // lack item -> here
                }
            }
        } else if (cur >= 0 && strncmp(line, "@ROLL", 5) == 0) {
            inBody = false;
            CyPage& pg = cyPages[cur];
            if (pg.choiceCount < CY_MAX_CHOICES) {
                char tmp[120]; strncpy(tmp, line + 5, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
                char* parts[6]; char* s = tmp; while (*s==' ') s++;
                int n = splitPipe(s, parts, 6);
                if (n >= 6) {
                    CyChoice& c = pg.choices[pg.choiceCount++];
                    c.btn = (parts[0][0] == 'R' || parts[0][0] == 'r') ? 1 : 0;
                    c.isRoll = 1;
                    c.gateItem = -1;
                    strncpy(c.label, parts[1], CY_LABEL_LEN-1);
                    c.target = (uint8_t)atoi(parts[2]);
                    c.tag = tagFromName(parts[3]);
                    strncpy(c.gotoId, parts[4], CY_ID_LEN-1);
                    strncpy(c.failId, parts[5], CY_ID_LEN-1);
                }
            }
        } else if (cur >= 0 && strncmp(line, "@XP", 3) == 0) {
            inBody = false;
            cyPages[cur].grantXp = atoi(line + 3);
        } else if (cur >= 0 && strncmp(line, "@ITEM", 5) == 0) {
            inBody = false;
            const char* fp = line + 5; while (*fp == ' ') fp++;
            int id = itemFromName(fp);
            if (id >= 0) cyPages[cur].grantItem = (int8_t)id;
        } else if (cur >= 0 && strncmp(line, "@DIE", 4) == 0) {
            inBody = false;
            // @DIE <sides> <attrname>
            char tmp[32]; strncpy(tmp, line + 4, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
            char* s = tmp; while (*s == ' ') s++;
            int sides = atoi(s);
            char* sp = strchr(s, ' ');
            uint8_t attr = DA_NONE;
            if (sp) { while (*sp == ' ') sp++; attr = dieAttrFromName(sp); }
            if (sides >= 2) { cyPages[cur].grantDieSides = (uint8_t)sides;
                              cyPages[cur].grantDieAttr = attr; }
        } else if (cur >= 0 && strncmp(line, "@HP", 3) == 0) {
            inBody = false;
            cyPages[cur].hpDelta = (int8_t)atoi(line + 3);
        } else if (cur >= 0 && strncmp(line, "@END", 4) == 0) {
            inBody = false;
            cyPages[cur].isEnd = 1;
        } else if (cur >= 0 && inBody) {
            // accumulate into body span
            CyPage& pg = cyPages[cur];
            pg.bodyLen = (nextLine > pg.bodyOffset) ? (nextLine - pg.bodyOffset) : 0;
        }
        cyBuf[lineEnd] = saved;
        i = nextLine;
    }
    cyReady = (cyPageCount > 0);
    return cyReady;
}

// ── Body wrapping ─────────────────────────────────────────────────────────────
static void cyWrapBody(int pageIdx) {
    cyLineCount = 0;
    cyScroll = 0;
    if (pageIdx < 0 || pageIdx >= cyPageCount || !cyBuf) return;
    CyPage& pg = cyPages[pageIdx];
    size_t start = pg.bodyOffset;
    size_t end   = pg.bodyOffset + pg.bodyLen;
    if (end > cyBufLen) end = cyBufLen;

    char word[CY_LINE_LEN];
    char cur[CY_LINE_LEN]; cur[0] = '\0';
    int wl = 0;

    auto pushLine = [&](const char* s) {
        if (cyLineCount < CY_MAX_LINES) {
            strncpy(cyLines[cyLineCount], s, CY_LINE_LEN-1);
            cyLines[cyLineCount][CY_LINE_LEN-1] = '\0';
            cyLineCount++;
        }
    };
    auto flushWord = [&]() {
        if (wl == 0) return;
        word[wl] = '\0';
        if (cur[0] == '\0') {
            strncpy(cur, word, CY_LINE_LEN-1); cur[CY_LINE_LEN-1]='\0';
        } else if ((int)strlen(cur) + 1 + wl < CY_LINE_LEN) {
            strcat(cur, " "); strcat(cur, word);
        } else {
            pushLine(cur);
            strncpy(cur, word, CY_LINE_LEN-1); cur[CY_LINE_LEN-1]='\0';
        }
        wl = 0;
    };

    for (size_t k = start; k < end; k++) {
        char ch = cyBuf[k];
        if (ch == '\r') continue;
        if (ch == '\n') {
            flushWord();
            if (cur[0]) { pushLine(cur); cur[0] = '\0'; }
            else        { pushLine(""); }   // blank line = paragraph gap
        } else if (ch == ' ') {
            flushWord();
        } else {
            if (wl < CY_LINE_LEN-1) word[wl++] = ch;
        }
    }
    flushWord();
    if (cur[0]) pushLine(cur);
}

// Apply XP/unlock awards for the page we just landed on (once).
static void cyApplyAwards(int pageIdx) {
    if (cyAwarded) return;
    CyPage& pg = cyPages[pageIdx];
    if (pg.grantXp > 0) {
        qpAddXp((uint32_t)pg.grantXp);
        // The mantis shares a little in the adventure's glory.
        qpGrantPetXp((uint32_t)pg.grantXp / 4);
    }
    if (pg.grantItem >= 0)     qpAddItem((uint8_t)pg.grantItem, 1);
    if (pg.grantDieSides >= 2) qpAddDie(pg.grantDieSides, pg.grantDieAttr);
    if (pg.hpDelta != 0) {
        cyHp += pg.hpDelta;
        if (cyHp > CY_HP_MAX) cyHp = CY_HP_MAX;
        if (cyHp < 0) cyHp = 0;
        // ARMED supplies kick in exactly when the story turns dire: a Medkit
        // saves you from the brink, a Ration steadies you when running low.
        if (cyHp <= 2 && qpTakeOnePending(ITEM_CY_MEDKIT)) {
            cyHp += 6; if (cyHp > CY_HP_MAX) cyHp = CY_HP_MAX;
            strncpy(cyAssist, "MEDKIT +6 HP", sizeof(cyAssist)-1);
            cyAssistAt = millis();
        } else if (cyHp <= 4 && qpTakeOnePending(ITEM_CY_RATION)) {
            cyHp += 3; if (cyHp > CY_HP_MAX) cyHp = CY_HP_MAX;
            strncpy(cyAssist, "RATION +3 HP", sizeof(cyAssist)-1);
            cyAssistAt = millis();
        }
        if (cyHp == 0) cyDead = true;
    }
    cyAwarded = true;
}

static void cyGoto(int pageIdx) {
    if (pageIdx < 0 || pageIdx >= cyPageCount) return;
    if (pageIdx == 0) { cyHp = CY_HP_MAX; cyDead = false; }  // restart resets HP
    cyCurrent = pageIdx;
    cyMode = CY_PAGE;
    cyAwarded = false;
    cyWrapBody(cyCurrent);
    cyApplyAwards(cyCurrent);
}

// Auto-check level-gated unlocks, then open the story-select menu.
void cyoaEnter() {
    chordGuardReset();
    fb.setRotation(0);
    qpCheckStoryUnlocks();        // unlock level-10 story if earned
    cyMode = CY_SELECT;
    cySelIdx = 0;
    cyReady = false;
}

// Load the story at story-table index i (must be unlocked). Returns success.
static bool cyLoadStory(int i) {
    if (i < 0 || i >= CY_STORY_COUNT) return false;
    if (!qpStoryUnlocked(CY_STORIES[i].flag)) return false;
    strncpy(cyPakPath, CY_STORIES[i].path, sizeof(cyPakPath) - 1);
    cyPakPath[sizeof(cyPakPath) - 1] = 0;
    if (!cyParse()) { cyReady = false; return false; }
    cyCurrent = 0;
    cyMode = CY_PAGE;
    cyAwarded = false;
    cyHp = CY_HP_MAX;
    cyDead = false;
    cyWrapBody(0);
    cyApplyAwards(0);
    return true;
}

void cyoaShutdown() {
    cyFreeBuf();
    cyReady = false;
}

// ── Input ─────────────────────────────────────────────────────────────────────
static void cyBeginRoll(const CyChoice& c) {
    cyMode = CY_ROLLING;
    cyRollTarget = c.target;
    cyAssist[0] = '\0';
    cyRollTotal = qpRollEquipped(tagToName(c.tag), cyRollBreak, sizeof(cyRollBreak));
    // ARMED Luck Charm: +2 to this check (one charm per check).
    if (cyRollTotal < c.target && qpTakeOnePending(ITEM_CY_LUCKCHARM)) {
        cyRollTotal += 2;
        strncpy(cyAssist, "LUCK CHARM +2", sizeof(cyAssist)-1);
        cyAssistAt = millis();
    }
    // ARMED Reroll: if the check still fails, roll it once more automatically.
    if (cyRollTotal < c.target && qpTakeOnePending(ITEM_CY_REROLL)) {
        int second = qpRollEquipped(tagToName(c.tag), cyRollBreak, sizeof(cyRollBreak));
        if (second > cyRollTotal) cyRollTotal = second;
        strncpy(cyAssist, "REROLL!", sizeof(cyAssist)-1);
        cyAssistAt = millis();
    }
    cyRollWin = (cyRollTotal >= cyRollTarget);
    strncpy(cyResultGoto, cyRollWin ? c.gotoId : c.failId, CY_ID_LEN-1);
    cyResultGoto[CY_ID_LEN-1] = '\0';
    cyRollAnimAt = millis();
    cyRollAnimFace = 1;
}

void cyoaTick() {
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();

    // Story-select menu runs before a story is loaded (cyReady is false here).
    if (cyMode == CY_SELECT) {
        if (lEdge) cySelIdx = (cySelIdx + 1) % CY_STORY_COUNT;
        if (rEdge) {
            if (qpStoryUnlocked(CY_STORIES[cySelIdx].flag)) cyLoadStory(cySelIdx);
            // locked entries do nothing (stay on the menu)
        }
        return;
    }

    if (!cyReady) return;

    // Death overlay: HP reached 0. Either button restarts from the beginning.
    if (cyDead) {
        if (lEdge || rEdge) cyGoto(0);
        return;
    }

    if (cyMode == CY_PAGE) {
        CyPage& pg = cyPages[cyCurrent];
        if (pg.isEnd && pg.choiceCount == 0) {
            // Terminal: either button restarts the story.
            if (lEdge || rEdge) cyGoto(0);
            return;
        }
        // Scroll long bodies: if there's more text than fits, L scrolls when
        // there is no L-choice. To keep it simple and match the device, a long
        // page uses R to advance text until the last screen, then choices act.
        int avail = (pg.choiceCount > 0) ? 8 : 16;
        bool moreText = (cyScroll + avail < cyLineCount);
        if (moreText) {
            // While more text remains, L scrolls up a little, R scrolls down.
            if (rEdge) { cyScroll += avail; if (cyScroll > cyLineCount-1) cyScroll = cyLineCount-1; return; }
            if (lEdge) { cyScroll -= avail; if (cyScroll < 0) cyScroll = 0; return; }
            return;
        }
        // At the end of the text: buttons map to choices.
        auto take = [&](int wantBtn) {
            for (int i = 0; i < pg.choiceCount; i++) {
                CyChoice& ch = pg.choices[i];
                if (ch.btn != wantBtn) continue;
                if (ch.isRoll) { cyBeginRoll(ch); return; }
                // Hidden inventory check: an @USE choice routes by whether the
                // player holds (and spends) the gating item.
                if (ch.gateItem >= 0) {
                    const char* dest;
                    if (qpUseItem((uint8_t)ch.gateItem)) dest = ch.gotoId;  // had it, consumed
                    else                                  dest = ch.failId;  // lacked it -> consequence
                    int d = cyFindPage(dest); if (d >= 0) cyGoto(d);
                    return;
                }
                // Plain choice.
                int d = cyFindPage(ch.gotoId); if (d >= 0) cyGoto(d);
                return;
            }
        };
        if (lEdge) take(0);
        if (rEdge) take(1);
    }
    else if (cyMode == CY_ROLLING) {
        // brief animation, then show result; any press skips to result
        if (millis() - cyRollAnimAt > 1100 || lEdge || rEdge) {
            cyMode = CY_RESULT;
        }
    }
    else if (cyMode == CY_RESULT) {
        if (lEdge || rEdge) {
            int d = cyFindPage(cyResultGoto);
            if (d >= 0) cyGoto(d); else cyGoto(0);
        }
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────
static void cyDrawHud() {
    // Top banner: level + HP + XP bar so progression is always visible.
    fb.fillRect(0, 0, W, 16, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    char hud[24]; snprintf(hud, sizeof(hud), "LVL %u", (unsigned)qpLevel());
    fb.drawString(hud, 4, 8, 1);
    // HP readout
    fb.setTextColor(cyHp <= 3 ? PURPLE_LIGHT : LIME);
    char hp[16]; snprintf(hp, sizeof(hp), "HP %d", cyHp);
    fb.drawString(hp, 44, 8, 1);
    // mini XP bar
    uint32_t into = qpXpIntoLevel(), span = qpXpForNextLevel();
    int bw = 50; int fillw = span ? (int)((uint64_t)into * bw / span) : 0;
    fb.drawRect(W-bw-4, 5, bw, 6, TEAL_DARK);
    fb.fillRect(W-bw-4, 5, fillw, 6, TEAL_BRIGHT);
    // Transient banner when an ARMED supply just kicked in (charm/reroll/heal).
    if (cyAssist[0] && millis() - cyAssistAt < 2600) {
        fb.fillRect(0, 16, W, 12, LIME_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(cyAssist, W/2, 22, 1);
    }
}

static void cyDrawPage() {
    cyDrawHud();
    CyPage& pg = cyPages[cyCurrent];
    int avail = (pg.choiceCount > 0) ? 8 : 16;
    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_LIGHT);
    int y = 22;
    for (int i = cyScroll; i < cyLineCount && i < cyScroll + avail; i++) {
        fb.drawString(cyLines[i], 4, y, 1); y += 13;
    }
    bool moreText = (cyScroll + avail < cyLineCount);
    if (moreText) {
        fb.setTextColor(LIME); fb.setTextDatum(MC_DATUM);
        fb.drawString("R: more   L: back", W/2, H-10, 1);
        return;
    }
    // choices
    if (pg.isEnd && pg.choiceCount == 0) {
        fb.setTextColor(LIME); fb.setTextDatum(MC_DATUM);
        fb.drawString("THE END", W/2, H-40, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("press to restart", W/2, H-18, 1);
        return;
    }
    int cy = H - 56;
    fb.drawFastHLine(0, cy - 4, W, TEAL_DARK);
    for (int i = 0; i < pg.choiceCount; i++) {
        const CyChoice& c = pg.choices[i];
        const char* tag = (c.btn == 0) ? "L:" : "R:";
        uint16_t col = (c.btn == 0) ? TEAL_BRIGHT : PURPLE_LIGHT;
        fb.setTextColor(col); fb.setTextDatum(TL_DATUM);
        char row[48];
        if (c.isRoll)
            snprintf(row, sizeof(row), "%s %s [roll %d]", tag, c.label, c.target);
        else
            snprintf(row, sizeof(row), "%s %s", tag, c.label);
        fb.drawString(row, 4, cy + i * 22, 1);
        if (c.isRoll && tagToName(c.tag)[0]) {
            fb.setTextColor(TEAL_DARK);
            fb.drawString(tagToName(c.tag), 14, cy + i * 22 + 11, 1);
        }
    }
}

static void cyDrawRolling() {
    cyDrawHud();
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("SKILL CHECK", W/2, 40, 2);
    fb.setTextColor(TEAL_DARK);
    char t[24]; snprintf(t, sizeof(t), "need %d", cyRollTarget);
    fb.drawString(t, W/2, 62, 1);

    // Animated die face cycling
    int cx = W/2, cyy = 150, s = 56;
    fb.fillRoundRect(cx - s/2, cyy - s/2, s, s, 8, LIME_PALE);
    fb.drawRoundRect(cx - s/2, cyy - s/2, s, s, 8, PURPLE_DARK);
    int face = ((millis() / 90) % 6) + 1;
    fb.setTextColor(PURPLE_DARK); fb.setTextDatum(MC_DATUM);
    char fc[4]; snprintf(fc, sizeof(fc), "%d", face);
    fb.drawString(fc, cx, cyy, 4);

    fb.setTextColor(TEAL_LIGHT);
    fb.drawString("rolling...", W/2, 220, 1);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("press to skip", W/2, H-14, 1);
}

static void cyDrawResult() {
    cyDrawHud();
    fb.setTextDatum(MC_DATUM);
    if (cyRollWin) { fb.setTextColor(LIME); fb.drawString("SUCCESS!", W/2, 50, 2); }
    else           { fb.setTextColor(PURPLE_LIGHT); fb.drawString("FAILED", W/2, 50, 2); }

    // Result total vs target
    fb.setTextColor(LIME_PALE);
    char res[32]; snprintf(res, sizeof(res), "%d vs %d", cyRollTotal, cyRollTarget);
    fb.drawString(res, W/2, 120, 4);

    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(cyRollBreak, W/2, 156, 1);

    // big die showing nothing fancy, just a checkmark/x feel via color block
    int cx = W/2, cyy = 210, s = 50;
    uint16_t bg = cyRollWin ? LIME_DARK : PURPLE_DARK;
    fb.fillRoundRect(cx - s/2, cyy - s/2, s, s, 8, bg);
    fb.setTextColor(LIME_PALE); fb.setTextDatum(MC_DATUM);
    fb.drawString(cyRollWin ? "OK" : "X", cx, cyy, 4);

    fb.setTextColor(TEAL_DARK);
    fb.drawString("press to continue", W/2, H-14, 1);
}

void cyoaDraw() {
    fb.fillSprite(TEAL_DEEP);

    // Story-select menu.
    if (cyMode == CY_SELECT) {
        fb.fillRect(0, 0, W, 18, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("MANTIS TALE", 4, 9, 1);
        fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT);
        char lv[12]; snprintf(lv, sizeof(lv), "Lv %lu", (unsigned long)qpLevel());
        fb.drawString(lv, W - 4, 9, 1);

        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("CHOOSE YOUR TALE", W/2, 30, 1);

        int y = 48;
        for (int i = 0; i < CY_STORY_COUNT; i++) {
            bool unlocked = qpStoryUnlocked(CY_STORIES[i].flag);
            bool sel = (i == cySelIdx);
            if (sel) fb.fillRoundRect(6, y - 2, W - 12, 50, 4, PURPLE_DARK);

            fb.setTextDatum(ML_DATUM);
            fb.setTextColor(unlocked ? (sel ? LIME_PALE : LIME) : TEAL_DARK);
            // title (truncate to fit)
            char t[26]; strncpy(t, CY_STORIES[i].title, sizeof(t)-1); t[sizeof(t)-1]=0;
            fb.drawString(t, 12, y + 12, 1);

            if (unlocked) {
                fb.setTextColor(sel ? LIME : TEAL_LIGHT);
                fb.drawString("> play", 12, y + 30, 1);
            } else {
                fb.setTextColor(PURPLE_LIGHT);
                fb.drawString("LOCKED", 12, y + 30, 1);
                fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_DARK);
                fb.drawString(CY_STORIES[i].how, W - 12, y + 30, 1);
            }
            y += 56;
        }

        fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("L: next", 4, H - 7, 1);
        fb.setTextDatum(MR_DATUM); fb.drawString("R: play", W - 4, H - 7, 1);
        fb.pushSprite(0, 0);
        return;
    }

    if (!cyReady) {
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("story not", W/2, H/2 - 10, 2);
        fb.drawString("installed", W/2, H/2 + 10, 2);
        fb.setTextColor(TEAL_DARK);
        fb.drawString("flash content pack", W/2, H/2 + 36, 1);
        fb.pushSprite(0, 0);
        return;
    }
    if (cyDead) {
        cyDrawHud();
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(PURPLE_LIGHT);
        fb.drawString("YOU HAVE FALLEN", W/2, H/2 - 24, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("Your adventure ends here.", W/2, H/2 + 2, 1);
        fb.drawString("But the dice you earned", W/2, H/2 + 18, 1);
        fb.drawString("are yours to keep.", W/2, H/2 + 32, 1);
        fb.setTextColor(LIME);
        fb.drawString("press to try again", W/2, H - 24, 1);
        fb.pushSprite(0, 0);
        return;
    }
    switch (cyMode) {
        case CY_SELECT:  break;   // handled above with early return
        case CY_PAGE:    cyDrawPage();    break;
        case CY_ROLLING: cyDrawRolling(); break;
        case CY_RESULT:  cyDrawResult();  break;
    }
    fb.pushSprite(0, 0);
}
