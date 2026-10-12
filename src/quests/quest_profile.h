// quest_profile.h - The RPG profile, OWNED BY QUEST MODE.
//
// Quest mode is the core of the device-wide leveling and reward economy. This
// profile is its property; every other mode (CYOA, Pinball, Pet) only READS it
// or pays XP/items INTO it. It persists to NVS (namespace "qprofile").
//
// Contents:
//   - XP + derived level (the player's single character level).
//   - A collectible DICE collection (carries across questing and CYOA).
//   - A shared INVENTORY of consumable items, each TAGGED with which mode(s)
//     can consume it (pinball / pet / cyoa / any). One inventory, many uses.
//   - A reward GENERATOR: given a task's category + difficulty, it produces an
//     appropriate reward (XP plus sometimes a die or a consumable). This is how
//     an infinite, always-useful stream of rewards is generated from a simple
//     7-day to-do list.
#pragma once
#include <stdint.h>

// ── Dice ──────────────────────────────────────────────────────────────────────
static constexpr int QP_MAX_DICE   = 16;
static constexpr int QP_DICE_SLOTS = 5;

enum DieAttr : uint8_t {
    DA_NONE=0, DA_LUCKY, DA_KEEN, DA_HEAVY, DA_WILD,
    DA_SWIFT, DA_BRAVE, DA_WISE, DA_COUNT
};

struct Die {
    uint8_t sides;   // 4,6,8,10,12,20
    uint8_t attr;    // DieAttr
    uint8_t owned;   // 1 if collected
};

// ── Inventory items ───────────────────────────────────────────────────────────
// Each item TYPE has a fixed definition (name, effect, which modes use it).
// The profile stores a COUNT per item type. Items are consumed by the modes.
static constexpr int QP_ITEM_COUNT = 30;   // number of distinct item types

// Which mode(s) an item can be used in. Bitmask so "any" = all bits.
enum ItemTag : uint8_t {
    TAG_PINBALL  = 1 << 0,
    TAG_PET      = 1 << 1,
    TAG_CYOA     = 1 << 2,
    TAG_AQUARIUM = 1 << 3,
    TAG_SLOTS    = 1 << 4,
    TAG_POCKET   = 1 << 5,
    TAG_FRUIT    = 1 << 6,
    TAG_ANY      = 0x7F,
};

// Reward tiers. Quest difficulty maps to tier odds (see qpGenerateReward):
// 1-2 star tasks can drop LOW items; 3 star favors MID; 4 star can drop any
// tier (MID most likely); 5 star pays HIGH with a shot at SPECIAL. SPECIAL
// drops additionally require player level >= 8 (hidden gate; downgrades to
// HIGH below that).
enum ItemTier : uint8_t { TIER_LOW = 0, TIER_MID, TIER_HIGH, TIER_SPECIAL };

// Item type ids. Pinball items map to the existing powerups; pet items are
// snacks/potions; cyoa items are adventuring gear/consumables.
enum ItemId : uint8_t {
    // Pinball consumables (injected at next game start)
    ITEM_PB_MULTIBALL = 0,
    ITEM_PB_SLOWBALL,
    ITEM_PB_BOOST,
    ITEM_PB_POWERFLIP,
    ITEM_PB_PROJECTILE,
    // Pet consumables
    ITEM_PET_SNACK,        // small heal + happiness
    ITEM_PET_POTION,       // bigger heal
    ITEM_PET_PROTEIN,      // a pet level boost
    ITEM_PET_TONIC,        // cleanse / cure
    // CYOA adventuring consumables
    ITEM_CY_TORCH,         // auto-pass a "dark/sight" obstacle
    ITEM_CY_ROPE,          // auto-pass a "climb/descent" obstacle
    ITEM_CY_RATION,        // restore HP in a story
    ITEM_CY_MEDKIT,        // bigger HP restore
    ITEM_CY_LUCKCHARM,     // +2 to next skill check
    ITEM_CY_MULTITOOL,     // auto-pass a "tech/repair" obstacle
    ITEM_CY_REROLL,        // reroll a failed check once
    // Aquarium (quest-reward exclusive)
    ITEM_AQ_COINPACK,      // +50 aquarium coins
    ITEM_AQ_RAREFISH,      // unlocks an exclusive fish species
    ITEM_AQ_DECOR,         // unlocks an exclusive decoration
    // Slots / Jacks or Better
    ITEM_SL_BALANCE,       // start next game with +250 balance
    ITEM_SL_INSURANCE,     // one-time: a busted hand is refunded next game
    // --- v5 additions (append only; ids are persisted) ---
    ITEM_AQ_FEED,          // aquarium: +10 fish pellets
    ITEM_PB_EXTRABALL,     // pinball: +1 reserve ball next game
    ITEM_PK_LUCKYBREAK,    // pocket: +1 reserve ball next game
    ITEM_PK_CHALK,         // pocket: final score x2 next game
    ITEM_FR_WIDE,          // fruit: wider slash tolerance next game
    ITEM_FR_LIFE,          // fruit: +1 life next arcade game
    ITEM_FR_FRENZY,        // fruit: start next game in frenzy
    ITEM_PET_BAIT,         // pet: next hunt attracts more/richer bugs
    ITEM_XP_SCROLL,        // instant: +100 XP when used
};

struct ItemDef {
    const char* name;
    uint8_t     tag;     // ItemTag bitmask
    uint8_t     tier;    // ItemTier (drives quest reward tier rolls + UI color)
    const char* desc;    // short "what it does / where used" line
};
const ItemDef& qpItemDef(uint8_t id);
const char*    qpTagLabel(uint8_t tag);
const char*    qpTierName(uint8_t tier);
int            qpInventoryCount();        // # of distinct owned item types
int            qpInventoryItemId(int slot);
// "Use" from the quest inventory ARMS the item: it moves from items[] into
// pending[], and the associated app consumes+applies it the next time that app
// starts a session (qpTakePending). Instant items (XP Scroll) apply on the
// spot. Returns a short human-readable status line for the UI.
const char*    qpUseInventorySlot(int slot);
// Pending ("armed") item pipeline - the ONE way apps receive used items.
bool           qpArmItem(uint8_t itemId);        // items[]-- -> pending[]++
uint16_t       qpPendingCount(uint8_t itemId);
uint16_t       qpTakePending(uint8_t itemId);    // consume ALL pending of id
bool           qpTakeOnePending(uint8_t itemId); // consume exactly one
// Back-compat wrappers used by the aquarium (now backed by pending[]).
uint8_t        qpTakeAqCoinPacks();
uint8_t        qpTakeAqRareFish();
uint8_t        qpTakeAqDecor();

// ── Reward categories & difficulty ────────────────────────────────────────────
// The AP sets each task's category + difficulty; the engine turns those into a
// reward. Categories bias WHICH kind of reward; difficulty scales XP and the
// chance/quality of a bonus item or die.
enum TaskCategory : uint8_t {
    CAT_GENERAL = 0,   // balanced
    CAT_PHYSICAL,      // chores/exercise -> pinball + pet items, swift/brave dice
    CAT_MENTAL,        // study/reading   -> cyoa items, wise/keen dice
    CAT_CREATIVE,      // art/music/build -> cyoa gear, wild dice
    CAT_KINDNESS,      // helping others  -> pet items, lucky dice
    CAT_COUNT
};

// difficulty: 1 (easy) .. 5 (epic)
struct RewardResult {
    uint16_t xp;
    bool     gotItem;
    uint8_t  itemId;
    bool     gotDie;
    uint8_t  dieSides;
    uint8_t  dieAttr;
    char     summary[40];   // human-readable, e.g. "+25 XP, Torch"
};

// ── The profile struct (POD; serialized as an NVS blob) ────────────────────────
struct QuestProfile {
    uint32_t version;
    uint32_t totalXp;
    Die      dice[QP_MAX_DICE];
    int8_t   slot[QP_DICE_SLOTS];        // indices into dice[], -1 = empty
    uint16_t items[QP_ITEM_COUNT];       // count of each ItemId
    uint32_t pinballPerksPending;        // legacy (v4) perk bits; migrated into pending[]
    uint32_t storyUnlocks;               // STORY_* bitmask of unlocked CYOA tales
    uint32_t petXpPending;               // pet XP deposited by quests/cyoa, picked up by Pet Mode
    uint16_t pending[QP_ITEM_COUNT];     // ARMED items awaiting pickup by their app (v5)
};

// Pinball perk flags (legacy quick-perk path, still consumed by pinball).
enum PerkFlag : uint32_t {
    PERK_MULTIBALL = 1u<<0, PERK_SLOWBALL = 1u<<1,
    PERK_POWERFLIP = 1u<<2, PERK_BOOST    = 1u<<3, PERK_PROJECTILE = 1u<<4,
};

// CYOA story unlock flags.
enum StoryFlag : uint32_t {
    STORY_HOLLOW   = 1u<<0,   // "The Hollow Road" (always unlocked)
    STORY_SIGNAL   = 1u<<1,   // "Signal from the Ridge" (unlocked at level 10)
    STORY_DERELICT = 1u<<2,   // "The Derelict Array" (7-day quest streak)
    STORY_TIDE     = 1u<<3,   // "The Drowned Reliquary" (unlock all aquarium fish)
};

// ── API (all under quest ownership) ────────────────────────────────────────────
void     qpLoad();
void     qpSave();
void     qpResetDefaults();

uint32_t qpLevel();
uint32_t qpXp();
uint32_t qpXpIntoLevel();
uint32_t qpXpForNextLevel();
void     qpAddXp(uint32_t amount);          // may level up

// Dice
int      qpOwnedDiceCount();
int      qpEquippedCount();
int      qpAddDie(uint8_t sides, uint8_t attr);   // returns dice idx or -1
int      qpRollEquipped(const char* tag, char* breakdown, int len);
const char* qpDieAttrName(uint8_t attr);

// Inventory
void     qpAddItem(uint8_t itemId, uint16_t n);
bool     qpUseItem(uint8_t itemId);          // decrement if available; true if used
uint16_t qpItemCount(uint8_t itemId);
int      qpItemsForTag(uint8_t tag, uint8_t* outIds, int maxOut);  // list owned items usable in a mode

// Pinball perks
void     qpGrantPerk(uint32_t perk);
uint32_t qpConsumePerks();

// Story unlocks
bool     qpStoryUnlocked(uint32_t flag);
void     qpUnlockStory(uint32_t flag);
uint32_t qpCheckStoryUnlocks();   // auto-unlock level-gated stories; returns newly-unlocked flag

// Pet XP hook: quests/cyoa deposit pet XP here; Pet Mode consumes it on entry.
void     qpGrantPetXp(uint32_t amount);
uint32_t qpConsumePetXp();

// Reward generation: the heart of the economy.
RewardResult qpGenerateReward(uint8_t category, uint8_t difficulty);
void         qpApplyReward(const RewardResult& r);   // commits XP/item/die + saves

QuestProfile& qpData();
