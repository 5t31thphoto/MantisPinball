// FILE: src/power_guard.h
// Save-on-shutdown flush registry. No polling, no timers, no battery code -
// the firmware already samples the battery once a second in tickBattery() and
// calls goToSleep() at BAT_CUTOFF, and goToSleep() already flushes everything
// through pgFlushAll(). This header only lets each persistent app hand the
// firmware its dirty-flag-guarded flush so those two existing shutdown paths
// (the manual 3 s sleep-hold and the automatic low-voltage cutoff) can persist
// it. A flush with a clean dirty flag is free, so this costs nothing when
// nothing changed.
#pragma once

typedef void (*FlushFn)();

// Register an app's flush once (from its Enter). Duplicates are ignored.
void pgRegisterFlush(FlushFn fn);

// Flush every registered app (each is dirty-flag guarded). Called from the two
// shutdown paths that already exist in main.cpp. Safe to call anytime.
void pgFlushAll();
