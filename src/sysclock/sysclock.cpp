// sysclock.cpp - Shared offline-capable system clock.
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <Preferences.h>
#include <WiFi.h>
#include "sysclock.h"

static const char*  SC_NVS   = "sysclock";
static const time_t SC_FLOOR = 1700000000;   // ~2023-11; anything below = invalid
static bool         scSynced = false;          // got a real NTP fix this session
static bool         scNeedsConfirm = false;    // restored from anchor; ask user "is this today?"

// Persisted: the last epoch we are confident about.
static time_t scLoadAnchor() {
    Preferences p; time_t e = 0;
    if (p.begin(SC_NVS, true)) { e = (time_t)p.getULong("epoch", 0); p.end(); }
    return e;
}
static void scSaveAnchor(time_t e) {
    Preferences p;
    if (p.begin(SC_NVS, false)) { p.putULong("epoch", (uint32_t)e); p.end(); }
}

// ---- LOCAL TIME -----------------------------------------------------------
// NTP gives us UTC. "What day is it" and the wall-clock date must be computed in
// the user's LOCAL timezone, or the date rolls over to tomorrow in the evening
// for anyone west of UTC (e.g. Mountain Time flips at 5-6pm). The timezone
// offset (minutes east of UTC) is stored in the shared "pinball" namespace by
// the Setup / weather / miner screens; default -420 = US Mountain (UTC-7).
static int scBaseOffsetMin() {
    int off = -420;
    Preferences p;
    if (p.begin("pinball", true)) { off = p.getInt("tz_off", -420); p.end(); }
    return off;
}

// US DST: 2nd Sunday of March 02:00 local .. 1st Sunday of November 02:00 local.
// Computed on the base-offset-shifted epoch so the local calendar date is right.
static bool scIsUSDST(time_t utc, int baseOffMin) {
    time_t local = utc + (time_t)baseOffMin * 60;
    struct tm t; gmtime_r(&local, &t);   // gmtime on shifted epoch = local wall time
    int m = t.tm_mon + 1;      // 1..12
    int d = t.tm_mday;         // 1..31
    int wday = t.tm_wday;      // 0=Sun
    int hour = t.tm_hour;

    if (m < 3 || m > 11) return false;   // Dec-Feb: no DST
    if (m > 3 && m < 11) return true;    // Apr-Oct: DST

    // Date of the Sunday on/after a given day-of-month:
    //   the calendar date of "this" weekday's most recent Sunday is d - wday.
    // The first Sunday of the month is ((d - wday) % 7 + 7) % 7, then +1..7.
    int firstSunday = ((d - wday) % 7 + 7) % 7;   // 0..6 = date-1 of 1st Sunday-ish
    firstSunday = (firstSunday == 0) ? 7 : firstSunday;   // 1..7
    if (m == 3) {
        int secondSunday = firstSunday + 7;      // 2nd Sunday of March
        if (d > secondSunday) return true;
        if (d < secondSunday) return false;
        return hour >= 2;                        // on the day: after 02:00
    } else { // m == 11
        int firstSun = firstSunday;              // 1st Sunday of November
        if (d < firstSun) return true;
        if (d > firstSun) return false;
        return hour < 2;                         // on the day: before 02:00
    }
}

// Total local offset in seconds (base + DST hour if active).
static int scLocalOffsetSec() {
    int base = scBaseOffsetMin();
    time_t utc = time(nullptr);
    if (utc < SC_FLOOR) { time_t a = scLoadAnchor(); if (a >= SC_FLOOR) utc = a; }
    bool dst = scIsUSDST(utc, base);
    return (base + (dst ? 60 : 0)) * 60;
}

// Local epoch = UTC epoch shifted into local wall-clock time. Use this for any
// "date"/"day" computation; keep raw UTC for NTP/anchor storage.
static time_t scLocalNow() {
    time_t utc = time(nullptr);
    if (utc < SC_FLOOR) { time_t a = scLoadAnchor(); if (a >= SC_FLOOR) utc = a; }
    return utc + scLocalOffsetSec();
}

void sysclockBegin() {
    // If the RTC already holds a valid time (e.g. preserved across deep sleep),
    // keep it. Otherwise seed from the last persisted anchor so the day counter
    // doesn't reset to 1970 on a cold boot.
    time_t now = time(nullptr);
    if (now < SC_FLOOR) {
        time_t anchor = scLoadAnchor();
        if (anchor >= SC_FLOOR) {
            struct timeval tv = { anchor, 0 };
            settimeofday(&tv, nullptr);
            // Restored from a stale anchor on a cold boot: the date is probably
            // close (last time we knew) but we lost track of elapsed off-time.
            // Flag it so a mode can ask "is this still today?" — pre-filled with
            // this estimate, so the user is only a click or two from correct.
            scNeedsConfirm = true;
        } else {
            // No anchor at all (first-ever boot): seed a sane default and ask.
            struct timeval tv = { (time_t)1735689600, 0 };  // 2025-01-01 UTC
            settimeofday(&tv, nullptr);
            scNeedsConfirm = true;
        }
    } else {
        // RTC time survived; treat it as good and refresh the anchor.
        scSaveAnchor(now);
    }
}

bool sysclockSyncNTP(uint32_t timeoutMs) {
    // If we already have a real fix this session, don't re-poll - just return.
    // This is what keeps weather mode fast: it calls us every time it connects,
    // but after the first successful sync this is an instant no-op instead of a
    // multi-second blocking poll.
    if (scSynced && !scNeedsConfirm) return true;
    // Caller must have WiFi up. configTime is non-blocking; we poll for a fix.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; modes apply their own offset
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        time_t now = time(nullptr);
        if (now >= SC_FLOOR) {
            scSynced = true;
            scNeedsConfirm = false;   // a real fix overrides any estimate
            scSaveAnchor(now);
            return true;
        }
        delay(200);
    }
    return false;
}

time_t sysclockNow() {
    return time(nullptr);
}

uint64_t sysclockEpochDays() {
    // Days must be counted in LOCAL time, or the day rolls over in the evening
    // for anyone west of UTC. scLocalNow() applies the tz + DST offset.
    time_t local = scLocalNow();
    if (local < 0) local = 0;
    return (uint64_t)(local / 86400);
}

int sysclockWeekday() {
    // Local day-of-week (0=Sun). Derived from the local-shifted epoch so it
    // agrees with sysclockEpochDays()/sysclockGetDate(); does NOT use
    // localtime_r (which would need the TZ env var and return UTC here).
    time_t local = scLocalNow();
    if (local < SC_FLOOR) local = 1735689600;
    struct tm tmv; gmtime_r(&local, &tmv);
    return tmv.tm_wday;
}

bool sysclockValid() {
    // Real if synced this session, or the current time is above the floor.
    return scSynced || time(nullptr) >= SC_FLOOR;
}

void sysclockPersist() {
    time_t now = time(nullptr);
    if (now >= SC_FLOOR) scSaveAnchor(now);
}

// Opportunistically set the clock from NTP using the shared WiFi credentials,
// WITHOUT depending on weather mode. Safe to call from the menu when the clock
// is unknown: it only acts if creds exist and we don't already have a fix, uses
// a short timeout, and always tears WiFi back down. Returns true on success.
// `budgetMs` bounds the whole attempt so the UI never stalls for long.
bool sysclockTryAutoSync(uint32_t budgetMs) {
    // Already have a REAL fix this session -> nothing to do. A stale anchor
    // (time above floor but scNeedsConfirm) should still trigger a sync, so we
    // don't early-out just because time() is above the floor.
    if (scSynced && !scNeedsConfirm) return true;
    String ssid, pass;
    {
        Preferences p;
        if (p.begin("btcminer", true)) {
            ssid = p.getString("ssid", "");
            pass = p.getString("pass", "");
            p.end();
        }
    }
    if (ssid.length() == 0) return false;     // no creds; nothing to do

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < budgetMs) {
        delay(100);
    }
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
        uint32_t remain = (millis() - start < budgetMs) ? budgetMs - (millis()-start) : 1000;
        ok = sysclockSyncNTP(remain);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return ok;
}

// ---- user date confirmation / manual entry --------------------------------

bool sysclockNeedsConfirm() { return scNeedsConfirm; }

void sysclockGetDate(int& year, int& month, int& day) {
    // Local calendar date (tz + DST applied) so "today" matches the wall clock.
    time_t local = scLocalNow();
    if (local < SC_FLOOR) local = 1735689600;
    struct tm tmv; gmtime_r(&local, &tmv);   // gmtime on local-shifted epoch
    year  = tmv.tm_year + 1900;
    month = tmv.tm_mon + 1;
    day   = tmv.tm_mday;
}

// Days from 1970-01-01 to the given Y/M/D (proleptic Gregorian, UTC).
// Howard Hinnant's days_from_civil algorithm - no libc timegm needed.
static long scDaysFromCivil(int y, unsigned m, unsigned d){
    y -= (m <= 2);
    long era = (y >= 0 ? y : y-399) / 400;
    unsigned yoe = (unsigned)(y - era*400);
    unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return era*146097L + (long)doe - 719468L;
}

// Set the date from user input (keeps time-of-day at noon UTC so day boundaries
// are stable). Marks the clock confirmed and persists the anchor.
void sysclockSetDate(int year, int month, int day) {
    long days = scDaysFromCivil(year, (unsigned)month, (unsigned)day);
    time_t e = (time_t)(days * 86400L + 12*3600L);   // noon UTC
    if (e < SC_FLOOR) e = 1735689600;
    struct timeval tv = { e, 0 };
    settimeofday(&tv, nullptr);
    scNeedsConfirm = false;
    scSaveAnchor(e);
}

// "Yes, the estimate is right" — accept the current restored date as-is.
void sysclockConfirm() {
    scNeedsConfirm = false;
    sysclockPersist();
}

// Advance/rewind the date by whole days (for the quick adjust UI).
void sysclockAdjustDays(int delta) {
    time_t now = time(nullptr);
    if (now < SC_FLOOR) now = 1735689600;
    now += (time_t)delta * 86400;
    if (now < SC_FLOOR) now = SC_FLOOR;
    struct timeval tv = { now, 0 };
    settimeofday(&tv, nullptr);
    scSaveAnchor(now);
}
