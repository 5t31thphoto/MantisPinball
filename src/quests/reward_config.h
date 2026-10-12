// FILE: src/quests/reward_config.h
// Data-driven quest reward tuning.
//
// The reward economy used to be entirely hardcoded in quest_profile.cpp: the XP
// formula, the tier odds per difficulty, the die chance, and each item's
// tier/tag. This reader lets the installer's "Quest Rewards" tab write a plain
// text config that the firmware loads at boot, so rewards can be retuned
// WITHOUT RECOMPILING.
//
// If the file is missing or malformed, every accessor reports "not configured"
// and quest_profile falls back to its original hardcoded behaviour - so a device
// with no config behaves exactly as it did before.
//
// FILE FORMAT (/content/rewards.cfg, plain text, '#' comments):
//   @REWARDCFG 1
//   D|<diff>|<xpBase>|<xpSpread>|<diePct>|<dieSides>
//   T|<diff>|<pNone>|<pLow>|<pMid>|<pHigh>|<pSpecial>      (percent, sums to 100)
//   C|<cat>|<attr1>,<attr2>,...                            (die attribute pool)
//   I|<itemId>|<tier>|<tagMask>|<name>                     (per-item override)
#pragma once
#include <stdint.h>

// Load /content/rewards.cfg from LittleFS. Safe to call more than once.
// Returns true if a valid config was loaded.
bool rcLoad();
bool rcLoaded();

// --- accessors; each returns false when the config doesn't cover that case ---
bool rcXp(uint8_t diff, uint16_t& base, uint16_t& spread);
// Fills a 5-slot cumulative percent table [none,low,mid,high,special].
bool rcTierOdds(uint8_t diff, uint8_t out[5]);
bool rcDie(uint8_t diff, uint8_t& pct, uint8_t& sides);
// Die attribute pool for a category; returns count written into out (max 4).
int  rcDieAttrs(uint8_t cat, uint8_t out[4]);
// Per-item overrides (tier / tag / display name). Return false = use built-in.
bool rcItemTier(uint8_t id, uint8_t& tier);
bool rcItemTag(uint8_t id, uint8_t& tag);
const char* rcItemName(uint8_t id);      // nullptr = use built-in name
