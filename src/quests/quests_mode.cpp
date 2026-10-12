// quests_mode.cpp - daily gamified to-do; owns the RPG profile + reward economy.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <string.h>
#include "quests_mode.h"
#include "quest_profile.h"
#include "reward_config.h"
#include "../sysclock/sysclock.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
#include "../chord_guard.h"
static constexpr int W = 170;
static constexpr int H = 320;

// ── AP setup portal (same proven pattern as the miner/weather portals) ────────
static constexpr const char* Q_AP = "pinball-quests";
static WebServer* qSrv = nullptr;
static DNSServer* qDns = nullptr;
static bool       q_portalOn = false;
static bool       q_enterHoldGuard = false;   // ignore buttons held over from menu select
static bool       q_confirmDate = false;   // showing "is this today?" overlay
enum QTab { QTAB_SCHEDULE, QTAB_INVENTORY, QTAB_PROFILE, QTAB_COUNT };
static int        q_tab = QTAB_SCHEDULE;
static int        q_invSel = 0;            // inventory cursor
static char     q_invMsg[36] = "";
static uint32_t q_invMsgAt   = 0;

static const uint32_t QSCHED_VERSION = 1;
static QuestSchedule g_sched;

// Daily progress: which task slots are completed today, plus the day index it
// applies to so it resets when the day rolls over.
static uint16_t q_doneMask = 0;
static uint32_t q_dayCounter = 0;     // monotonically increasing day number
static uint16_t q_streak = 0;         // consecutive fully-completed days
static uint64_t q_lastFullDay = 0;    // epoch-day (or counter) of last full completion
static int      q_todayIdx   = 0;     // 0..6 weekday shown
static int      q_sel        = 0;

// Reward popup state
static bool     q_showReward = false;
static uint32_t q_rewardAt   = 0;
static char     q_rewardMsg[40] = "";
static bool     q_levelUp    = false;

static const char* DAY_NAMES[7] =
    { "SUNDAY","MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY" };

// ── Offline day resolution ────────────────────────────────────────────────────
// We persist a "day counter" and the millis-derived day at last save, so that
// across reboots we can advance the counter by however many 24h periods have
// elapsed. If the RTC is valid we trust its weekday directly; otherwise we map
// the counter onto the AP start weekday. Manual advance bumps the counter.
struct QDayState { uint32_t counter; uint64_t lastEpochDays; uint16_t doneMask; uint32_t version;
                   uint16_t streak; uint64_t lastFullDay; };

static uint64_t nowEpochDays() {
    if (sysclockValid()) return sysclockEpochDays();   // shared clock: days since 1970
    // No real clock: use the day counter (handled elsewhere).
    return 0;
}
static bool clockValid() { return sysclockValid(); }

static void loadDayState() {
    Preferences p;
    QDayState st; memset(&st,0,sizeof(st));
    if (p.begin("qday", true)) {
        size_t n = p.getBytesLength("st");
        if (n == sizeof(QDayState)) p.getBytes("st",&st,sizeof(st));
        p.end();
    }
    q_dayCounter = st.counter;
    q_doneMask   = st.doneMask;
    q_streak     = st.streak;
    q_lastFullDay= st.lastFullDay;

    if (clockValid()) {
        // Trust the shared clock's LOCAL weekday (tz + DST aware). Using
        // localtime_r here would return UTC (no TZ env set) and could report
        // tomorrow's weekday in the evening.
        q_todayIdx = sysclockWeekday();
        uint64_t ed = nowEpochDays();
        if (st.lastEpochDays != 0 && ed != st.lastEpochDays) {
            // day changed since last run -> reset completion
            q_doneMask = 0;
        }
    } else {
        // No clock: weekday = (startDow + counter) mod 7
        q_todayIdx = (g_sched.startDow + q_dayCounter) % 7;
    }
}

static void saveDayState() {
    Preferences p;
    if (p.begin("qday", false)) {
        QDayState st;
        st.version = 2;
        st.counter = q_dayCounter;
        st.lastEpochDays = nowEpochDays();
        st.doneMask = q_doneMask;
        st.streak = q_streak;
        st.lastFullDay = q_lastFullDay;
        p.putBytes("st",&st,sizeof(st));
        p.end();
    }
}

// Manually advance to the next day (failsafe / no-clock usage).
static void advanceDayManual() __attribute__((unused));
static void advanceDayManual() {
    q_dayCounter++;
    q_doneMask = 0;
    if (!clockValid()) q_todayIdx = (g_sched.startDow + q_dayCounter) % 7;
    else { // also nudge weekday view forward when user forces it
        q_todayIdx = (q_todayIdx + 1) % 7;
    }
    q_sel = 0;
    saveDayState();
}

// ── Schedule config ───────────────────────────────────────────────────────────
void questsSeedDefaultConfig() {
    memset(&g_sched,0,sizeof(g_sched));
    g_sched.version = QSCHED_VERSION;
    g_sched.startDow = 1;   // assume setup on a Monday until AP says otherwise
    struct Seed { const char* l; uint8_t cat; uint8_t diff; };
    // Weekday routine
    const Seed wk[] = {
        {"Make your bed",     CAT_GENERAL,  1},
        {"Brush teeth",       CAT_GENERAL,  1},
        {"Homework",          CAT_MENTAL,   3},
        {"Read 20 minutes",   CAT_MENTAL,   2},
        {"Help with chores",  CAT_KINDNESS, 2},
    };
    for (int d=1; d<=5; d++)
        for (int t=0; t<5; t++){
            QuestTask& q=g_sched.days[d][t];
            strncpy(q.label, wk[t].l, Q_LABEL_LEN-1);
            q.category=wk[t].cat; q.difficulty=wk[t].diff; q.active=1;
        }
    // Weekend
    const Seed we[] = {
        {"Tidy your room",    CAT_PHYSICAL, 2},
        {"Outside 30 min",    CAT_PHYSICAL, 2},
        {"Create something",  CAT_CREATIVE, 3},
        {"Family time",       CAT_KINDNESS, 2},
    };
    for (int d=0; d<=6; d+=6)
        for (int t=0; t<4; t++){
            QuestTask& q=g_sched.days[d][t];
            strncpy(q.label, we[t].l, Q_LABEL_LEN-1);
            q.category=we[t].cat; q.difficulty=we[t].diff; q.active=1;
        }
}

// Import a schedule written by the installer's Quest Rewards tab. The file uses
// EXACTLY the same line encoding the captive portal POSTs, so this reuses
// qParsePost and the portal keeps working untouched. The file is consumed once
// and then renamed, so a re-flash of content doesn't keep overwriting edits the
// user later makes in the portal.
static void qParsePost(const String& body);   // defined with the portal below
static void questsImportFileIfPresent() {
    if (!LittleFS.begin(false)) return;
    if (!LittleFS.exists("/content/todos.txt")) return;
    fs::File f = LittleFS.open("/content/todos.txt", "r");
    if (!f) return;
    String body;
    while (f.available()) body += (char)f.read();
    f.close();
    if (body.length() > 2) {
        qParsePost(body);                     // writes + saves the schedule
        LittleFS.remove("/content/todos.txt"); // consume it once
    }
}

void questsConfigLoad() {
    questsImportFileIfPresent();
    Preferences p; bool ok=false;
    if (p.begin("qsched", true)) {
        size_t n=p.getBytesLength("s");
        if (n==sizeof(QuestSchedule)){ p.getBytes("s",&g_sched,sizeof(QuestSchedule));
            if (g_sched.version==QSCHED_VERSION) ok=true; }
        p.end();
    }
    if (!ok){ questsSeedDefaultConfig(); questsConfigSave(); }
}
void questsConfigSave() {
    Preferences p;
    if (p.begin("qsched", false)){ p.putBytes("s",&g_sched,sizeof(QuestSchedule)); p.end(); }
}
QuestSchedule& questsConfig() { return g_sched; }

// ── Today's task helpers ──────────────────────────────────────────────────────
static int todayActive() {
    int n=0; for(int t=0;t<Q_MAX_TASKS;t++) if(g_sched.days[q_todayIdx][t].active) n++; return n;
}
static int slotForOrd(int ord) {
    int n=0; for(int t=0;t<Q_MAX_TASKS;t++){
        if(g_sched.days[q_todayIdx][t].active){ if(n==ord)return t; n++; } }
    return -1;
}

// ── AP portal HTML ────────────────────────────────────────────────────────────
// The form builds a compact textarea-free UI: a start-day selector plus, for
// each weekday, rows of "label | category | difficulty". On submit it POSTs a
// line-encoded payload to /save which the device parses into the schedule.
static const char Q_HTML[] PROGMEM = R"RAW(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{background:#001111;color:#00e7e7;font-family:monospace;padding:14px;max-width:520px;margin:0 auto}
h2{color:#00ff00}h3{color:#00e7e7;border-bottom:1px solid #007373;margin-top:18px}
.task{display:flex;gap:6px;margin-bottom:6px}
input.l{flex:3;background:#001a1a;border:1px solid #007373;color:#00e7e7;padding:6px;font-family:monospace}
select{flex:1;background:#001a1a;border:1px solid #007373;color:#00e7e7;padding:6px;font-family:monospace}
label{font-size:13px;color:#007373}
button{background:#003c3c;border:1px solid #00e7e7;color:#00e7e7;padding:10px 24px;
       font-family:monospace;font-size:15px;border-radius:4px;cursor:pointer;width:100%;margin-top:16px}
button:hover{background:#007373}.note{font-size:11px;color:#005a5a}
</style></head><body>
<h2>&#127942; QUEST SETUP</h2>
<p class='note'>Set up a recurring weekly to-do list. Each task has a category
and difficulty (1-5). Harder tasks pay more XP and better rewards.</p>
<label>Today is:</label>
<select id='dow'>
<option value='0'>Sunday</option><option value='1'>Monday</option>
<option value='2'>Tuesday</option><option value='3'>Wednesday</option>
<option value='4'>Thursday</option><option value='5'>Friday</option>
<option value='6'>Saturday</option></select>
<div id='days'></div>
<button onclick='save()'>SAVE WEEK</button>
<script>
var DN=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var CATS=['General','Physical','Mental','Creative','Kindness'];
var root=document.getElementById('days');
for(var d=0;d<7;d++){
  var h=document.createElement('h3');h.textContent=DN[d];root.appendChild(h);
  for(var t=0;t<6;t++){
    var row=document.createElement('div');row.className='task';
    var li=document.createElement('input');li.className='l';li.placeholder='task '+(t+1)+' (blank=skip)';
    li.id='l'+d+'_'+t;row.appendChild(li);
    var cs=document.createElement('select');cs.id='c'+d+'_'+t;
    for(var c=0;c<CATS.length;c++){var o=document.createElement('option');o.value=c;o.textContent=CATS[c];cs.appendChild(o);}
    row.appendChild(cs);
    var ds=document.createElement('select');ds.id='d'+d+'_'+t;
    for(var n=1;n<=5;n++){var o2=document.createElement('option');o2.value=n;o2.textContent=n;ds.appendChild(o2);}
    row.appendChild(ds);
    root.appendChild(row);
  }
}
function save(){
  var out=document.getElementById('dow').value+'\n';
  for(var d=0;d<7;d++)for(var t=0;t<6;t++){
    var l=document.getElementById('l'+d+'_'+t).value.trim();
    if(!l)continue;
    var c=document.getElementById('c'+d+'_'+t).value;
    var n=document.getElementById('d'+d+'_'+t).value;
    out+=d+'|'+t+'|'+c+'|'+n+'|'+l+'\n';
  }
  var x=new XMLHttpRequest();x.open('POST','/save',true);
  x.setRequestHeader('Content-Type','text/plain');
  x.onload=function(){document.body.innerHTML='<h2 style="color:#0f0">SAVED!</h2><p>You can close this page.</p>';};
  x.send(out);
}
</script></body></html>)RAW";

// Parse the posted payload into g_sched. Format: first line = startDow; then
// lines "day|slot|category|difficulty|label".
static void qParsePost(const String& body) {
    // wipe existing schedule (keep version/startDow set below)
    memset(g_sched.days, 0, sizeof(g_sched.days));
    int i = 0, n = body.length();
    // first line: startDow
    int lineEnd = body.indexOf('\n');
    if (lineEnd < 0) return;
    g_sched.startDow = (uint8_t)(body.substring(0, lineEnd).toInt() % 7);
    i = lineEnd + 1;
    while (i < n) {
        int e = body.indexOf('\n', i);
        if (e < 0) e = n;
        String line = body.substring(i, e);
        i = e + 1;
        if (line.length() < 7) continue;
        // split on '|' into 5 fields (label may contain no '|')
        int p1=line.indexOf('|'); int p2=line.indexOf('|',p1+1);
        int p3=line.indexOf('|',p2+1); int p4=line.indexOf('|',p3+1);
        if (p1<0||p2<0||p3<0||p4<0) continue;
        int d   = line.substring(0,p1).toInt();
        int slot= line.substring(p1+1,p2).toInt();
        int cat = line.substring(p2+1,p3).toInt();
        int diff= line.substring(p3+1,p4).toInt();
        String lbl = line.substring(p4+1);
        if (d<0||d>6||slot<0||slot>=Q_MAX_TASKS) continue;
        QuestTask& q = g_sched.days[d][slot];
        strncpy(q.label, lbl.c_str(), Q_LABEL_LEN-1); q.label[Q_LABEL_LEN-1]='\0';
        q.category = (uint8_t)(cat<0?0:(cat>=CAT_COUNT?0:cat));
        q.difficulty = (uint8_t)(diff<1?1:(diff>5?5:diff));
        q.active = 1;
    }
    g_sched.version = QSCHED_VERSION;
    questsConfigSave();
}

static void qStartPortal() {
    WiFi.disconnect(true); delay(100);
    WiFi.mode(WIFI_AP); WiFi.softAP(Q_AP); delay(100);
    qDns = new DNSServer();
    qDns->setErrorReplyCode(DNSReplyCode::NoError);
    qDns->start(53, "*", WiFi.softAPIP());
    qSrv = new WebServer(80);
    qSrv->on("/", []{
        String pg = FPSTR(Q_HTML);
        qSrv->send(200, "text/html", pg);
    });
    qSrv->onNotFound([]{
        qSrv->sendHeader("Location", "http://192.168.4.1/"); qSrv->send(302, "", "");
    });
    qSrv->on("/save", HTTP_POST, []{
        String body = qSrv->arg("plain");
        qParsePost(body);
        qSrv->send(200, "text/plain", "OK");
    });
    qSrv->begin();
    q_portalOn = true;
}

static void qStopPortal() {
    if (qSrv) { qSrv->stop(); delete qSrv; qSrv = nullptr; }
    if (qDns) { qDns->stop(); delete qDns; qDns = nullptr; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    q_portalOn = false;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void questsEnter() {
    chordGuardReset();
    rcLoad();          // pick up installer-tuned reward rules, if any
    fb.setRotation(0);
    questsConfigLoad();
    loadDayState();
    q_sel = 0;
    q_showReward = false;
    q_portalOn = false;
    q_enterHoldGuard = true;   // ignore any button still held from menu selection
    // If the clock came back from a power loss unsure of the date, ask the user
    // to confirm — pre-filled with our best estimate, so it's a click or two.
    q_confirmDate = sysclockNeedsConfirm();
}
void questsShutdown() {
    if (q_portalOn) qStopPortal();
    saveDayState();
}

// ── Complete a task -> generate + apply a reward ──────────────────────────────
static void completeTask(int slot) {
    if (slot < 0) return;
    if (q_doneMask & (1u<<slot)) return;
    QuestTask& q = g_sched.days[q_todayIdx][slot];
    if (!q.active) return;
    q_doneMask |= (1u<<slot);

    uint32_t before = qpLevel();
    RewardResult r = qpGenerateReward(q.category, q.difficulty);
    qpApplyReward(r);
    qpCheckStoryUnlocks();   // unlock the level-10 story the moment it's earned
    uint32_t after = qpLevel();

    strncpy(q_rewardMsg, r.summary, sizeof(q_rewardMsg)-1);
    q_rewardMsg[sizeof(q_rewardMsg)-1]='\0';
    q_levelUp = (after > before);
    q_showReward = true;
    q_rewardAt = millis();

    // If every active task for today is now done, update the daily streak.
    uint16_t activeMask = 0;
    for (int s = 0; s < Q_MAX_TASKS; s++)
        if (g_sched.days[q_todayIdx][s].active) activeMask |= (1u<<s);
    if (activeMask && (q_doneMask & activeMask) == activeMask) {
        uint64_t today = clockValid() ? nowEpochDays() : (uint64_t)q_dayCounter + 1;
        if (today != q_lastFullDay) {           // don't double-count the same day
            if (q_lastFullDay != 0 && today == q_lastFullDay + 1) q_streak++;  // consecutive
            else q_streak = 1;                   // streak broken or first ever
            q_lastFullDay = today;
            if (q_streak >= 7 && !qpStoryUnlocked(STORY_DERELICT)) {
                qpUnlockStory(STORY_DERELICT);
                q_levelUp = true;                // reuse the celebratory banner
                strncpy(q_rewardMsg, "7-DAY STREAK! Story unlocked", sizeof(q_rewardMsg)-1);
                q_rewardMsg[sizeof(q_rewardMsg)-1]='\0';
            }
        }
    }
    saveDayState();
}

void questsTick() {
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();

    // "Is this today?" overlay after a power-loss reboot. Pre-filled with the
    // estimate, so the user is only a click or two from correct.
    //   tap-R  = yes, that's today (confirm and dismiss)
    //   tap-L  = +1 day (nudge the estimate forward)
    //   hold-L = -1 day (nudge back)
    if (q_confirmDate) {
        static uint32_t cdHoldStart = 0; static bool cdHolding = false;
        if (btnL && !cdHolding) { cdHolding = true; cdHoldStart = millis(); }
        if (!btnL && cdHolding) {
            cdHolding = false;
            if (millis() - cdHoldStart >= 600) sysclockAdjustDays(-1);  // hold-L = back a day
            else                                sysclockAdjustDays(+1);  // tap-L  = forward a day
        }
        if (rEdge) { sysclockConfirm(); q_confirmDate = false; loadDayState(); }
        return;
    }

    // While the AP config portal is open, service it; right button closes it.
    if (q_portalOn) {
        if (qDns) qDns->processNextRequest();
        if (qSrv) qSrv->handleClient();
        if (rEdge) {            // close portal, reload the (possibly new) schedule
            qStopPortal();
            questsConfigLoad();
            loadDayState();
            q_sel = 0;
        }
        return;
    }

    // Dismiss reward popup on any press.
    if (q_showReward) {
        if (lEdge || rEdge || millis()-q_rewardAt > 2200) q_showReward=false;
        return;
    }

    // Hold LEFT ~1s cycles tabs (Schedule -> Inventory -> Profile).
    // Hold RIGHT ~1.5s opens the AP setup portal.
    // Both-button gestures are RESERVED globally (sleep / menu) — never used here.
    static uint32_t lHoldStart=0;
    static uint32_t rHoldStart=0;
    static bool     lHoldFired=false;
    // Ignore any button that was already held when we entered (e.g. the R press
    // used to select QUESTS from the menu) until it's released once. This stops
    // the setup portal from opening unprovoked on entry.
    if (q_enterHoldGuard) {
        lHoldStart=0; rHoldStart=0; lHoldFired=false;
        if (!btnL && !btnR) q_enterHoldGuard = false;
        return;
    }
    if (btnL && !btnR) {
        rHoldStart=0;
        if (lHoldStart==0){ lHoldStart=millis(); lHoldFired=false; }
        else if (!lHoldFired && millis()-lHoldStart > 1000) {
            q_tab = (q_tab+1) % QTAB_COUNT; q_sel=0; q_invSel=0;
            lHoldFired=true;   // one tab change per hold
        }
    } else if (btnR && !btnL) {
        lHoldStart=0; lHoldFired=false;
        if (rHoldStart==0) rHoldStart=millis();
        else if (millis()-rHoldStart > 1500) { qStartPortal(); rHoldStart=0; return; }
    } else {
        lHoldStart=0; rHoldStart=0; lHoldFired=false;
    }
    // Suppress the tap action on the release that ended a hold-to-cycle.
    bool lTap = lEdge && !lHoldFired;

    if (q_tab == QTAB_INVENTORY) {
        int n = qpInventoryCount();
        if (n > 0) {
            if (lTap) q_invSel = (q_invSel+1) % n;
            if (rEdge) {
                const char* m = qpUseInventorySlot(q_invSel);  // arm / apply
                if (m && m[0]) { strncpy(q_invMsg, m, sizeof(q_invMsg)-1);
                    q_invMsg[sizeof(q_invMsg)-1]='\0'; q_invMsgAt = millis(); }
            }
        }
        return;
    }
    if (q_tab == QTAB_PROFILE) {
        return;   // profile is read-only
    }

    // ---- Schedule tab ----
    int active = todayActive();
    if (active <= 0) return;   // no tasks; tabs/portal still work via holds above
    if (lTap) q_sel = (q_sel+1) % active;
    if (rEdge) completeTask(slotForOrd(q_sel));
}

// ── Drawing ───────────────────────────────────────────────────────────────────
static const char* catShort(uint8_t c){
    switch(c){ case CAT_PHYSICAL:return "PHYS"; case CAT_MENTAL:return "MIND";
        case CAT_CREATIVE:return "ART"; case CAT_KINDNESS:return "KIND"; default:return "GEN"; }
}

// ── Inventory tab: list owned items with count, description, "used in" ──
static void qDrawInventory(){
    int n = qpInventoryCount();
    if (n == 0){
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("No items yet", W/2, H/2-16, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("Earn rewards by", W/2, H/2+6, 1);
        fb.drawString("completing quests", W/2, H/2+20, 1);
        fb.fillRect(0,H-14,W,14,PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("holdL: tab", 4, H-7, 1);
        return;
    }
    if (q_invSel >= n) q_invSel = n-1;

    // Scrolling window of rows (each row ~30px). Show up to 6, keep sel visible.
    const int rowH = 30, top = 24, visible = 6;
    int first = q_invSel - visible/2; if (first < 0) first = 0;
    if (first > n - visible) first = (n>visible)? n-visible : 0;

    for (int r=0; r<visible && first+r<n; r++){
        int slot = first+r;
        int id = qpInventoryItemId(slot);
        if (id < 0) continue;
        const ItemDef& d = qpItemDef(id);
        int y = top + r*rowH;
        bool sel = (slot==q_invSel);
        if (sel) fb.fillRoundRect(2,y-1,W-4,rowH-2,3,PURPLE_DARK);
        // name + count
        fb.setTextDatum(TL_DATUM);
        fb.setTextColor(sel?LIME_PALE:TEAL_LIGHT);
        fb.drawString(d.name, 6, y+2, 2);
        char cnt[8]; snprintf(cnt,sizeof(cnt),"x%u",(unsigned)qpItemCount(id));
        fb.setTextDatum(TR_DATUM); fb.setTextColor(LIME);
        fb.drawString(cnt, W-6, y+2, 2);
        // "used in" tag + tier badge + armed indicator
        fb.setTextDatum(TL_DATUM); fb.setTextColor(LIME_DARK);
        fb.drawString(qpTagLabel(d.tag), 6, y+17, 1);
        uint16_t tierCol = (d.tier==TIER_SPECIAL)?PURPLE_LIGHT :
                           (d.tier==TIER_HIGH)   ?LIME :
                           (d.tier==TIER_MID)    ?TEAL_BRIGHT : TEAL_DARK;
        fb.setTextDatum(TR_DATUM); fb.setTextColor(tierCol);
        uint16_t pend = qpPendingCount(id);
        if (pend){
            char ar[16]; snprintf(ar,sizeof(ar),"ARMED x%u",(unsigned)pend);
            fb.drawString(ar, W-6, y+17, 1);
        } else {
            fb.drawString(qpTierName(d.tier), W-6, y+17, 1);
        }
    }

    // selected item description at the bottom (or the transient use-result)
    if (q_invMsg[0] && millis() - q_invMsgAt < 1800){
        fb.fillRect(0,H-30,W,16,LIME_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(q_invMsg, W/2, H-22, 1);
    } else {
        int id = qpInventoryItemId(q_invSel);
        if (id >= 0){
            const ItemDef& d = qpItemDef(id);
            fb.fillRect(0,H-30,W,16,TEAL_DARK);
            fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
            fb.drawString(d.desc, W/2, H-22, 1);
        }
    }
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L:next holdL:tab", 4, H-7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R:use", W-4, H-7, 1);
}

// ── Profile tab: level, XP, streak, totals ──
static void qDrawProfile(){
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(LIME_PALE);
    fb.drawString("PLAYER PROFILE", W/2, 30, 2);

    // big level
    char lv[16]; snprintf(lv,sizeof(lv),"%u",(unsigned)qpLevel());
    fb.setTextColor(LIME);
    fb.drawString(lv, W/2, 70, 7);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("LEVEL", W/2, 104, 1);

    // XP bar
    uint32_t into=qpXpIntoLevel(), span=qpXpForNextLevel();
    int bx=20,by=120,bw=W-40,bh=8;
    fb.drawRect(bx,by,bw,bh,TEAL_DARK);
    int fw=span?(int)((uint64_t)into*(bw-2)/span):0;
    fb.fillRect(bx+1,by+1,fw,bh-2,TEAL_BRIGHT);
    char xb[24]; snprintf(xb,sizeof(xb),"%u / %u XP",(unsigned)into,(unsigned)span);
    fb.setTextColor(LIME_PALE); fb.drawString(xb, W/2, by+bh+8, 1);

    // stat rows
    fb.setTextDatum(TL_DATUM);
    int y=150;
    auto row=[&](const char* k, const char* v){
        fb.setTextColor(TEAL_LIGHT); fb.drawString(k, 16, y, 1);
        fb.setTextDatum(TR_DATUM); fb.setTextColor(LIME);
        fb.drawString(v, W-16, y, 1);
        fb.setTextDatum(TL_DATUM); y+=20;
    };
    char b[20];
    snprintf(b,sizeof(b),"%u days",(unsigned)q_streak); row("Streak", b);
    snprintf(b,sizeof(b),"%d",qpInventoryCount());     row("Item types", b);
    row("Clock", clockValid()?"set":"approx");

    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("holdL: tab", 4, H-7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("holdR: wifi", W-4, H-7, 1);
}

void questsDraw() {
    fb.fillSprite(TEAL_DEEP);

    // "Is this today?" date-confirm overlay (after power-loss reboot)
    if (q_confirmDate) {
        int yr, mo, dy; sysclockGetDate(yr, mo, dy);
        static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec"};
        fb.fillRect(0, 0, W, 18, PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("SET THE DATE", 4, 9, 1);

        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("Lost track of time.", W/2, 60, 1);
        fb.drawString("Is this today?", W/2, 78, 2);

        char ds[24];
        int mi = (mo >= 1 && mo <= 12) ? mo - 1 : 0;
        snprintf(ds, sizeof(ds), "%s %d, %d", MON[mi], dy, yr);
        fb.setTextColor(LIME);
        fb.drawString(ds, W/2, 140, 4);

        fb.setTextColor(TEAL_DARK);
        fb.drawString("tap L: +1 day", W/2, 200, 1);
        fb.drawString("hold L: -1 day", W/2, 216, 1);
        fb.setTextColor(LIME_PALE);
        fb.drawString("tap R: yes, that's today", W/2, 250, 1);

        fb.pushSprite(0, 0);
        return;
    }

    // AP setup portal screen
    if (q_portalOn) {
        fb.fillRect(0,0,W,18,PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("QUEST SETUP", 4, 9, 1);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(TEAL_BRIGHT);
        fb.drawString("Wi-Fi setup active", W/2, 60, 1);
        fb.setTextColor(LIME_PALE);
        fb.drawString("1. Join Wi-Fi:", W/2, 96, 1);
        fb.setTextColor(LIME);
        fb.drawString(Q_AP, W/2, 116, 2);
        fb.setTextColor(LIME_PALE);
        fb.drawString("2. Open in browser:", W/2, 150, 1);
        fb.setTextColor(TEAL_BRIGHT);
        fb.drawString("192.168.4.1", W/2, 170, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString("3. Set up your week", W/2, 200, 1);
        fb.drawString("   and tap SAVE", W/2, 214, 1);
        fb.fillRect(0,H-14,W,14,PURPLE_DARK);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString("R: done / close portal", W/2, H-7, 1);
        fb.pushSprite(0,0);
        return;
    }

    // ── Tab bar header (Schedule / Inventory / Profile) ──
    static const char* TAB_NAMES[] = { "QUESTS", "ITEMS", "PROFILE" };
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    int tw = W/3;
    for (int t=0; t<QTAB_COUNT; t++){
        bool on = (t==q_tab);
        if (on) fb.fillRect(t*tw,0,tw,18,PURPLE);
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(on?LIME_PALE:TEAL_DARK);
        fb.drawString(TAB_NAMES[t], t*tw+tw/2, 9, 1);
    }

    if (q_tab == QTAB_INVENTORY) { qDrawInventory(); fb.pushSprite(0,0); return; }
    if (q_tab == QTAB_PROFILE)   { qDrawProfile();   fb.pushSprite(0,0); return; }

    // ── Schedule tab ──
    // Day + XP bar
    fb.setTextDatum(TL_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString(DAY_NAMES[q_todayIdx], 4, 22, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_DARK);
    char lv[16]; snprintf(lv,sizeof(lv),"LVL %u",(unsigned)qpLevel());
    fb.drawString(lv, W-4, 24, 1);
    uint32_t into=qpXpIntoLevel(), span=qpXpForNextLevel();
    int bx=4,by=36,bw=W-8,bh=8;
    fb.drawRect(bx,by,bw,bh,TEAL_DARK);
    int fw=span?(int)((uint64_t)into*(bw-2)/span):0;
    fb.fillRect(bx+1,by+1,fw,bh-2,TEAL_BRIGHT);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    char xb[24]; snprintf(xb,sizeof(xb),"%u / %u XP",(unsigned)into,(unsigned)span);
    fb.drawString(xb, W/2, by+bh+7, 1);

    int active = todayActive();
    int y=56;
    if (active==0) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("No quests today", W/2, H/2-10, 1);
        fb.drawString("Set up in AP portal", W/2, H/2+8, 1);
    } else {
        int doneCount=0;
        for (int ord=0; ord<active; ord++) {
            int slot=slotForOrd(ord);
            QuestTask& q=g_sched.days[q_todayIdx][slot];
            bool done=(q_doneMask&(1u<<slot))!=0; if(done)doneCount++;
            bool seld=(ord==q_sel);
            int rowY=y+ord*34;
            if (seld) fb.fillRoundRect(2,rowY-2,W-4,32,4,PURPLE_DARK);
            // checkbox
            fb.drawRoundRect(6,rowY+4,16,16,3, done?LIME:TEAL_DARK);
            if (done){ fb.fillRoundRect(8,rowY+6,12,12,2,LIME);
                fb.setTextColor(TEAL_DEEP); fb.setTextDatum(MC_DATUM);
                fb.drawString("v",14,rowY+12,1); }
            // label
            fb.setTextDatum(TL_DATUM); fb.setTextColor(done?TEAL_DARK:TEAL_LIGHT);
            char lbl[Q_LABEL_LEN]; strncpy(lbl,q.label,sizeof(lbl)-1); lbl[sizeof(lbl)-1]='\0';
            fb.drawString(lbl, 28, rowY+3, 1);
            // category + difficulty stars
            fb.setTextColor(LIME_DARK);
            char meta[20]; char stars[6]={0};
            for(int s=0;s<q.difficulty && s<5;s++) stars[s]='*';
            snprintf(meta,sizeof(meta),"%s %s",catShort(q.category),stars);
            fb.drawString(meta, 28, rowY+16, 1);
        }
        if (doneCount==active){
            fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME);
            fb.drawString("ALL DONE TODAY!", W/2, H-38, 2);
        }
    }

    // Footer
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString("L:next holdL:tab", 4, H-7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R:check", W-4, H-7, 1);

    // Reward popup
    if (q_showReward) {
        int bxp=12, byp=H/2-34, bwp=W-24, bhp=68;
        fb.fillRoundRect(bxp,byp,bwp,bhp,8, q_levelUp?PURPLE:TEAL_DARK);
        fb.drawRoundRect(bxp,byp,bwp,bhp,8, LIME);
        fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
        fb.drawString(q_levelUp?"LEVEL UP!":"REWARD!", W/2, byp+16, 2);
        fb.setTextColor(TEAL_LIGHT);
        fb.drawString(q_rewardMsg, W/2, byp+40, 1);
        if (q_levelUp){ char ll[16]; snprintf(ll,sizeof(ll),"Level %u",(unsigned)qpLevel());
            fb.setTextColor(LIME); fb.drawString(ll, W/2, byp+54, 1); }
    }

    fb.pushSprite(0,0);
}
