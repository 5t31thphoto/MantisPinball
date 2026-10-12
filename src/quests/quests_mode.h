// quests_mode.h - Gamified 7-day recurring to-do; the core leveling/reward hub.
//
// Quest mode owns the RPG profile (see quest_profile.h). Each task has a
// category and a difficulty (1..5); the reward engine turns those into XP +
// consumable items + occasional dice. Rewards feed every other mode.
//
// Offline 7-day schedule: the weekly cycle never depends on the network. The
// "current day" is resolved in this priority order:
//   1. If the RTC clock is set (NTP earlier, or user-set), use its weekday.
//   2. Else, advance from an AP-configured start weekday using elapsed days
//      measured by a persisted boot/uptime accumulator.
//   3. The user can always MANUALLY advance the day (failsafe) - hold both or
//      use the in-mode day control.
//
// Buttons in the daily view: L = move selection, R = check off the task.
#pragma once
#include <stdint.h>

void questsEnter();
void questsTick();
void questsDraw();
void questsShutdown();

// ── Config schema (written by the AP setup portal) ────────────────────────────
static constexpr int Q_DAYS      = 7;     // 0=Sun .. 6=Sat
static constexpr int Q_MAX_TASKS = 6;     // per day
static constexpr int Q_LABEL_LEN = 28;

struct QuestTask {
    char    label[Q_LABEL_LEN];
    uint8_t category;     // TaskCategory
    uint8_t difficulty;   // 1..5
    uint8_t active;       // 1 if used
};

struct QuestSchedule {
    uint32_t  version;
    uint8_t   startDow;             // AP-set day-of-week anchor (0=Sun)
    QuestTask days[Q_DAYS][Q_MAX_TASKS];
};

void          questsConfigLoad();
void          questsConfigSave();
void          questsSeedDefaultConfig();
QuestSchedule& questsConfig();
