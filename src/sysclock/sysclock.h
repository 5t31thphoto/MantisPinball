// sysclock.h - Shared offline-capable system clock for all modes.
//
// The T-Display-S3 has no battery-backed RTC, so a cold reboot loses the time.
// This module gives every mode one consistent notion of "now":
//
//   * When WiFi is available, sysclockSyncNTP() sets the time from NTP and
//     persists the epoch to NVS.
//   * Deep sleep preserves the RTC, so time keeps running across sleep/wake.
//   * On a cold boot the time is restored from the last persisted epoch, so the
//     day counter never jumps backward; NTP corrects it on the next sync.
//
// Modes should call sysclockNow() / sysclockEpochDays() instead of time(NULL),
// and check sysclockValid() to know whether the time is real or estimated.
#pragma once
#include <time.h>
#include <stdint.h>

// Call once at boot (restores last-known epoch from NVS so time() is monotonic).
void     sysclockBegin();

// Try to sync from NTP. Requires WiFi already connected. Non-blocking-ish:
// waits up to timeoutMs for the first sync. Returns true on success.
bool     sysclockSyncNTP(uint32_t timeoutMs = 8000);

// Current epoch seconds (real if synced, else best estimate from last anchor).
time_t   sysclockNow();

// Whole days since the Unix epoch (LOCAL time) — stable key for "what day is it".
uint64_t sysclockEpochDays();

// Day of week in LOCAL time: 0=Sunday .. 6=Saturday. Use this instead of
// localtime_r(), which needs the TZ env var set (it isn't) and returns UTC.
int      sysclockWeekday();

// True if the time has been set from a real source (NTP) this power session
// or restored from a recent persisted anchor.
bool     sysclockValid();

// Persist the current epoch to NVS (called automatically after NTP sync; modes
// may call it before sleeping for extra safety).
void     sysclockPersist();

// Opportunistically sync from NTP using shared WiFi creds (independent of
// weather mode). Brief, self-contained, tears WiFi back down. budgetMs bounds
// the attempt. Returns true if the clock is now valid.
bool     sysclockTryAutoSync(uint32_t budgetMs = 6000);

// ---- user date confirmation / manual entry --------------------------------
// True if the time was restored from a stale anchor (cold boot / power loss)
// and hasn't been confirmed or NTP-synced. A mode can use this to show a quick
// "is this today?" prompt, pre-filled with the estimate.
bool     sysclockNeedsConfirm();
void     sysclockGetDate(int& year, int& month, int& day);   // current estimate
void     sysclockSetDate(int year, int month, int day);      // manual set (confirms)
void     sysclockConfirm();                                  // accept the estimate as-is
void     sysclockAdjustDays(int delta);                      // nudge by whole days
