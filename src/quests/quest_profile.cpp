// quest_profile.cpp - the RPG profile + reward economy, owned by quest mode.
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "quest_profile.h"
#include "reward_config.h"

static QuestProfile g_qp;
static const uint32_t QP_VERSION = 5;   // v5: tiers, pending[] pipeline, table grew to 30

// ── Item definitions ──────────────────────────────────────────────────────────
static const ItemDef ITEM_DEFS[QP_ITEM_COUNT] = {
    { "Multiball",   TAG_PINBALL,  TIER_HIGH,    "Pinball: extra ball at next launch" },
    { "Slow Ball",   TAG_PINBALL,  TIER_LOW,     "Pinball: ball moves slower next game" },
    { "Boost",       TAG_PINBALL,  TIER_LOW,     "Pinball: launch with extra speed" },
    { "Power Flip",  TAG_PINBALL,  TIER_MID,     "Pinball: stronger flippers next game" },
    { "Scatter",     TAG_PINBALL,  TIER_MID,     "Pinball: fires scatter projectiles" },
    { "Snack",       TAG_PET,      TIER_LOW,     "Pet: small feed + happiness" },
    { "Potion",      TAG_PET,      TIER_MID,     "Pet: bigger feed + heal" },
    { "Protein",     TAG_PET,      TIER_HIGH,    "Pet: feed + a pet XP boost" },
    { "Tonic",       TAG_PET,      TIER_MID,     "Pet: full clean + happiness" },
    { "Torch",       TAG_CYOA,     TIER_LOW,     "Tale: pass a dark/sight obstacle" },
    { "Rope",        TAG_CYOA,     TIER_LOW,     "Tale: pass a climb/descent obstacle" },
    { "Ration",      TAG_CYOA,     TIER_LOW,     "Tale: auto-heals when HP runs low" },
    { "Medkit",      TAG_CYOA,     TIER_MID,     "Tale: big auto-heal when HP is dire" },
    { "Luck Charm",  TAG_CYOA,     TIER_MID,     "Tale: +2 on your next skill check" },
    { "Multitool",   TAG_CYOA,     TIER_MID,     "Tale: pass a tech/repair obstacle" },
    { "Reroll",      TAG_CYOA,     TIER_HIGH,    "Tale: auto-rerolls a failed check" },
    { "Coin Pack",   TAG_AQUARIUM, TIER_MID,     "Aquarium: +50 coins on entry" },
    { "Rare Fish",   TAG_AQUARIUM, TIER_SPECIAL, "Aquarium: unlocks an exclusive fish" },
    { "Decor Box",   TAG_AQUARIUM, TIER_SPECIAL, "Aquarium: unlocks exclusive decor" },
    { "Chip Stack",  TAG_SLOTS,    TIER_MID,     "Jacks/Better: +250 start balance" },
    { "Insurance",   TAG_SLOTS,    TIER_HIGH,    "Jacks/Better: refunds one bust" },
    { "Fish Food",   TAG_AQUARIUM, TIER_LOW,     "Aquarium: +10 pellets on entry" },
    { "Extra Ball",  TAG_PINBALL,  TIER_HIGH,    "Pinball: +1 reserve ball next game" },
    { "Lucky Break", TAG_POCKET,   TIER_LOW,     "Pocket: +1 reserve ball next game" },
    { "Chalk Up",    TAG_POCKET,   TIER_MID,     "Pocket: final score x2 next game" },
    { "Wide Blade",  TAG_FRUIT,    TIER_MID,     "Fruit: wider slash reach next game" },
    { "Spare Life",  TAG_FRUIT,    TIER_LOW,     "Fruit: +1 life next arcade game" },
    { "Frenzy Core", TAG_FRUIT,    TIER_HIGH,    "Fruit: start next game in frenzy" },
    { "Bug Bait",    TAG_PET,      TIER_MID,     "Pet: next hunt draws richer bugs" },
    { "XP Scroll",   TAG_ANY,      TIER_HIGH,    "Instant: +100 XP when used" },
};
const ItemDef& qpItemDef(uint8_t id) {
    if (id >= QP_ITEM_COUNT) id = 0;
    return ITEM_DEFS[id];
}

// Human-readable tag label for "used in" display.
const char* qpTagLabel(uint8_t tag) {
    if (tag == TAG_ANY)     return "Anywhere";
    if (tag & TAG_PINBALL)  return "Pinball";
    if (tag & TAG_PET)      return "Pet";
    if (tag & TAG_CYOA)     return "Mantis Tale";
    if (tag & TAG_AQUARIUM) return "Aquarium";
    if (tag & TAG_SLOTS)    return "Jacks/Better";
    if (tag & TAG_POCKET)   return "Corner Pocket";
    if (tag & TAG_FRUIT)    return "Fruit Frenzy";
    return "Any";
}
const char* qpTierName(uint8_t tier) {
    switch (tier) {
        case TIER_MID:     return "MID";
        case TIER_HIGH:    return "HIGH";
        case TIER_SPECIAL: return "SPECIAL";
        default:           return "LOW";
    }
}

// ── Level curve ───────────────────────────────────────────────────────────────
static uint32_t xpToSpanLevel(uint32_t level) {
    if (level < 1) level = 1;
    return 50u * level + (25u * level * level) / 10u;
}
uint32_t qpLevel() {
    uint32_t lvl=1, acc=0;
    while (true) { uint32_t s=xpToSpanLevel(lvl);
        if (g_qp.totalXp < acc+s) break; acc+=s; lvl++; if(lvl>999)break; }
    return lvl;
}
uint32_t qpXp() { return g_qp.totalXp; }
uint32_t qpXpIntoLevel() {
    uint32_t lvl=1, acc=0;
    while (true){ uint32_t s=xpToSpanLevel(lvl);
        if(g_qp.totalXp<acc+s)break; acc+=s; lvl++; if(lvl>999)break; }
    return g_qp.totalXp-acc;
}
uint32_t qpXpForNextLevel() { return xpToSpanLevel(qpLevel()); }
void qpAddXp(uint32_t amount) { g_qp.totalXp += amount; qpSave(); }

// ── Dice ──────────────────────────────────────────────────────────────────────
int qpOwnedDiceCount() {
    int n=0; for(int i=0;i<QP_MAX_DICE;i++) if(g_qp.dice[i].owned) n++; return n;
}
int qpEquippedCount() {
    int n=0; for(int s=0;s<QP_DICE_SLOTS;s++){ int8_t d=g_qp.slot[s];
        if(d>=0 && d<QP_MAX_DICE && g_qp.dice[d].owned) n++; } return n;
}
int qpAddDie(uint8_t sides, uint8_t attr) {
    for (int i=0;i<QP_MAX_DICE;i++){
        if(!g_qp.dice[i].owned){
            g_qp.dice[i].sides=sides; g_qp.dice[i].attr=attr; g_qp.dice[i].owned=1;
            for(int s=0;s<QP_DICE_SLOTS;s++){ if(g_qp.slot[s]<0){ g_qp.slot[s]=(int8_t)i; break; } }
            return i;
        }
    }
    return -1;
}
const char* qpDieAttrName(uint8_t attr){
    switch(attr){ case DA_LUCKY:return "Lucky"; case DA_KEEN:return "Keen";
        case DA_HEAVY:return "Heavy"; case DA_WILD:return "Wild";
        case DA_SWIFT:return "Swift"; case DA_BRAVE:return "Brave";
        case DA_WISE:return "Wise"; default:return "Plain"; }
}
static int rollFace(uint8_t sides){ if(sides<2)sides=6; return (int)(esp_random()%sides)+1; }

int qpRollEquipped(const char* tag, char* bd, int len){
    int total=0, bp=0; if(bd&&len>0) bd[0]='\0';
    bool ag = tag && !strcmp(tag,"agility");
    bool co = tag && !strcmp(tag,"courage");
    bool wi = tag && !strcmp(tag,"wits");
    for(int s=0;s<QP_DICE_SLOTS;s++){
        int8_t di=g_qp.slot[s];
        if(di<0||di>=QP_MAX_DICE||!g_qp.dice[di].owned) continue;
        Die&d=g_qp.dice[di]; int f=rollFace(d.sides);
        switch(d.attr){
            case DA_LUCKY: if(f==1){int r=rollFace(d.sides); if(r>f)f=r;} break;
            case DA_KEEN:  f+=1; break;
            case DA_HEAVY: if(f<2)f=2; f+=2; break;
            case DA_WILD:  if(f==(int)d.sides) f+=rollFace(d.sides); break;
            case DA_SWIFT: if(ag)f+=1; break;
            case DA_BRAVE: if(co)f+=2; break;
            case DA_WISE:  if(wi)f+=2; break;
            default: break;
        }
        total+=f;
        if(bd && bp<len-6) bp+=snprintf(bd+bp,len-bp,"%s%d",(bp?"+":""),f);
    }
    int lvlMod=(int)(qpLevel()/3);
    if(lvlMod>0){ total+=lvlMod; if(bd&&bp<len-8) snprintf(bd+bp,len-bp,"+%d(lvl)",lvlMod); }
    return total;
}

// ── Inventory ─────────────────────────────────────────────────────────────────
void qpAddItem(uint8_t itemId, uint16_t n){
    if(itemId>=QP_ITEM_COUNT) return;
    uint32_t v=(uint32_t)g_qp.items[itemId]+n; if(v>9999)v=9999;
    g_qp.items[itemId]=(uint16_t)v; qpSave();
}
bool qpUseItem(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT||g_qp.items[itemId]==0) return false;
    g_qp.items[itemId]--; qpSave(); return true;
}
uint16_t qpItemCount(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT) return 0; return g_qp.items[itemId];
}

// Inventory enumeration: walk only item types the player actually owns (count>0).
int qpInventoryCount(){
    int n=0;
    for(int i=0;i<QP_ITEM_COUNT;i++) if(g_qp.items[i]>0) n++;
    return n;
}
int qpInventoryItemId(int slot){
    int n=0;
    for(int i=0;i<QP_ITEM_COUNT;i++){
        if(g_qp.items[i]>0){ if(n==slot) return i; n++; }
    }
    return -1;
}
// ── Pending ("armed") item pipeline ───────────────────────────────────────────
// "Use" in the quest inventory moves an item from items[] into pending[]. The
// item's app consumes it via qpTakePending on its next session start and
// applies the effect there. Persisted inside the profile blob so an armed item
// survives reboot. This is the ONE consumption path for every item type - no
// app may auto-consume straight from items[].
bool qpArmItem(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT||g_qp.items[itemId]==0) return false;
    g_qp.items[itemId]--;
    if(g_qp.pending[itemId]<9999) g_qp.pending[itemId]++;
    qpSave(); return true;
}
uint16_t qpPendingCount(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT) return 0; return g_qp.pending[itemId];
}
uint16_t qpTakePending(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT) return 0;
    uint16_t n=g_qp.pending[itemId];
    if(n){ g_qp.pending[itemId]=0; qpSave(); }
    return n;
}
bool qpTakeOnePending(uint8_t itemId){
    if(itemId>=QP_ITEM_COUNT||g_qp.pending[itemId]==0) return false;
    g_qp.pending[itemId]--; qpSave(); return true;
}

const char* qpUseInventorySlot(int slot){
    static char msg[36];
    int id = qpInventoryItemId(slot);
    if(id < 0) return "";
    // Instant items apply on the spot.
    if(id==ITEM_XP_SCROLL){
        if(qpUseItem((uint8_t)id)){ qpAddXp(100); return "+100 XP!"; }
        return "";
    }
    if(!qpArmItem((uint8_t)id)) return "";
    snprintf(msg,sizeof(msg),"ARMED: next %s",qpTagLabel(qpItemDef((uint8_t)id).tag));
    return msg;
}
// Back-compat aquarium wrappers (now just views over pending[]).
uint8_t qpTakeAqCoinPacks(){ uint16_t n=qpTakePending(ITEM_AQ_COINPACK); return n>255?255:(uint8_t)n; }
uint8_t qpTakeAqRareFish(){  uint16_t n=qpTakePending(ITEM_AQ_RAREFISH); return n>255?255:(uint8_t)n; }
uint8_t qpTakeAqDecor(){     uint16_t n=qpTakePending(ITEM_AQ_DECOR);    return n>255?255:(uint8_t)n; }
int qpItemsForTag(uint8_t tag, uint8_t* out, int maxOut){
    int n=0;
    for(uint8_t i=0;i<QP_ITEM_COUNT && n<maxOut;i++){
        if((ITEM_DEFS[i].tag & tag) && g_qp.items[i]>0) out[n++]=i;
    }
    return n;
}

// ── Perks / story unlocks ─────────────────────────────────────────────────────
void qpGrantPerk(uint32_t perk){ g_qp.pinballPerksPending|=perk; qpSave(); }
uint32_t qpConsumePerks(){ uint32_t p=g_qp.pinballPerksPending; g_qp.pinballPerksPending=0; qpSave(); return p; }
bool qpStoryUnlocked(uint32_t flag){ return (g_qp.storyUnlocks&flag)!=0; }
void qpUnlockStory(uint32_t flag){ g_qp.storyUnlocks|=flag; qpSave(); }

// Auto-unlock stories whose trigger is a player-level milestone. Other stories
// (streak, aquarium achievement) are unlocked by their own modes calling
// qpUnlockStory directly. Returns a newly-unlocked flag (or 0) so the caller
// can show a celebratory banner. Safe to call often; it only acts once per story.
uint32_t qpCheckStoryUnlocks(){
    uint32_t newly = 0;
    if (qpLevel() >= 10 && !(g_qp.storyUnlocks & STORY_SIGNAL)) {
        g_qp.storyUnlocks |= STORY_SIGNAL;
        newly |= STORY_SIGNAL;
    }
    if (newly) qpSave();
    return newly;
}
void qpGrantPetXp(uint32_t amount){ g_qp.petXpPending += amount; qpSave(); }
uint32_t qpConsumePetXp(){ uint32_t v=g_qp.petXpPending; g_qp.petXpPending=0; qpSave(); return v; }

// ── Reward generation ─────────────────────────────────────────────────────────
// XP scales with difficulty. Difficulty also raises the chance and quality of
// an item drop, and a rarer chance of a die. Category biases the item pool and
// die attributes so rewards feel thematically earned.
// Category -> which item tags feel "earned" for that kind of task.
static uint8_t catAffinity(uint8_t cat){
    switch(cat){
        case CAT_PHYSICAL: return TAG_PINBALL | TAG_POCKET | TAG_FRUIT;
        case CAT_MENTAL:   return TAG_CYOA;
        case CAT_CREATIVE: return TAG_CYOA | TAG_FRUIT | TAG_POCKET;
        case CAT_KINDNESS: return TAG_PET | TAG_AQUARIUM;
        default:           return TAG_ANY;
    }
}

// Pick a random item of the given tier, preferring the category's affinity
// tags; falls back to any item of the tier. Returns 0xFF if the tier is empty
// (cannot happen with the current table, but stay safe).
static uint8_t pickItemInTier(uint8_t tier, uint8_t cat){
    uint8_t pool[QP_ITEM_COUNT]; int n=0;
    uint8_t aff = catAffinity(cat);
    // An item's tier/tag can be re-assigned by the reward config, which is how
    // the installer "creates" new reward behaviour without a firmware rebuild.
    auto effTier=[](uint8_t i)->uint8_t{ uint8_t t; return rcItemTier(i,t)?t:ITEM_DEFS[i].tier; };
    auto effTag =[](uint8_t i)->uint8_t{ uint8_t g; return rcItemTag(i,g)?g:ITEM_DEFS[i].tag;  };
    for(uint8_t i=0;i<QP_ITEM_COUNT;i++)
        if(effTier(i)==tier && (effTag(i) & aff)) pool[n++]=i;
    if(n==0)
        for(uint8_t i=0;i<QP_ITEM_COUNT;i++)
            if(effTier(i)==tier) pool[n++]=i;
    if(n==0) return 0xFF;
    return pool[esp_random()%n];
}

// Difficulty -> tier odds. Returns true and sets outTier if an item drops.
//   1*  : 25% LOW
//   2*  : 45% LOW, 8% MID
//   3*  : guaranteed - 30% LOW, 62% MID, 8% HIGH
//   4*  : guaranteed - 18% LOW, 50% MID, 22% HIGH, 10% SPECIAL
//   5*  : guaranteed - 75% HIGH, 25% SPECIAL
// SPECIAL additionally requires player level >= 8 (hidden gate); below that it
// downgrades to HIGH so hard quests still pay premium.
static bool rollRewardTier(uint8_t diff, uint8_t& outTier){
    int r = (int)(esp_random()%100);
    // Installer-configured odds take priority: a cumulative table of
    // none/low/mid/high/special percentages for this difficulty.
    {
        uint8_t cum[5];
        if (rcTierOdds(diff, cum)){
            if (r < cum[0]) return false;               // no item
            if (r < cum[1]) { outTier = TIER_LOW;     return true; }
            if (r < cum[2]) { outTier = TIER_MID;     return true; }
            if (r < cum[3]) { outTier = TIER_HIGH;    return true; }
            outTier = TIER_SPECIAL;
            if (qpLevel() < 8) outTier = TIER_HIGH;     // same gate as before
            return true;
        }
    }
    switch(diff){
        case 1: if(r<25){ outTier=TIER_LOW; return true; } return false;
        case 2:
            if(r<45){ outTier=TIER_LOW; return true; }
            if(r<53){ outTier=TIER_MID; return true; }
            return false;
        case 3:
            outTier = (r<30)?TIER_LOW : (r<92)?TIER_MID : TIER_HIGH;
            return true;
        case 4:
            outTier = (r<18)?TIER_LOW : (r<68)?TIER_MID : (r<90)?TIER_HIGH : TIER_SPECIAL;
            break;
        default: // 5
            outTier = (r<75)?TIER_HIGH : TIER_SPECIAL;
            break;
    }
    if(outTier==TIER_SPECIAL && qpLevel()<8) outTier=TIER_HIGH;
    return true;
}

static void pickDieForCategory(uint8_t cat, uint8_t diff, uint8_t& sides, uint8_t& attr){
    // Difficulty sets base sides; category sets the attribute flavor.
    sides = (diff>=5)?12 : (diff>=4)?10 : (diff>=3)?8 : 6;
    switch(cat){
        case CAT_PHYSICAL: attr = (esp_random()&1)?DA_SWIFT:DA_BRAVE; break;
        case CAT_MENTAL:   attr = (esp_random()&1)?DA_WISE:DA_KEEN;   break;
        case CAT_CREATIVE: attr = DA_WILD; break;
        case CAT_KINDNESS: attr = DA_LUCKY; break;
        default:           attr = (esp_random()&1)?DA_KEEN:DA_NONE;   break;
    }
}

RewardResult qpGenerateReward(uint8_t category, uint8_t difficulty){
    if (difficulty < 1) difficulty = 1;
    if (difficulty > 5) difficulty = 5;
    if (category >= CAT_COUNT) category = CAT_GENERAL;

    RewardResult r; memset(&r,0,sizeof(r));
    // XP: 10 * difficulty + small random spread, unless the installer's reward
    // config (/content/rewards.cfg) overrides this difficulty. No config = the
    // original hardcoded behaviour, exactly as before.
    {
        uint16_t xbase, xspread;
        if (rcXp(difficulty, xbase, xspread))
            r.xp = (uint16_t)(xbase + (xspread ? (esp_random()%(xspread+1)) : 0));
        else
            r.xp = (uint16_t)(10*difficulty + (esp_random()%(5*difficulty+1)));
    }

    // Item drop by TIER: 1-2 star tasks pay XP with a chance at a LOW item;
    // 3 star and up always include an item, climbing the tier ladder (see
    // rollRewardTier). Category still flavors WHICH item within the tier.
    uint8_t tier;
    if (rollRewardTier(difficulty, tier)){
        uint8_t id = pickItemInTier(tier, category);
        if (id != 0xFF){ r.gotItem = true; r.itemId = id; }
    }
    // Die drop is rarer: only diff>=3, ~8%*diff - or whatever the reward config
    // says for this difficulty.
    {
        uint8_t dpct, dsides;
        bool haveCfg = rcDie(difficulty, dpct, dsides);
        int chance = haveCfg ? (int)dpct : (difficulty>=3 ? 6*difficulty : 0);
        if (chance > 0 && (int)(esp_random()%100) < chance){
            r.gotDie = true;
            pickDieForCategory(category, difficulty, r.dieSides, r.dieAttr);
            if (haveCfg && dsides) r.dieSides = dsides;
        }
    }

    // Build summary string.
    int p = snprintf(r.summary, sizeof(r.summary), "+%u XP", (unsigned)r.xp);
    if (r.gotItem && p < (int)sizeof(r.summary)-2)
        p += snprintf(r.summary+p, sizeof(r.summary)-p, ", %s", qpItemDef(r.itemId).name);
    if (r.gotDie && p < (int)sizeof(r.summary)-2)
        snprintf(r.summary+p, sizeof(r.summary)-p, ", d%u die", (unsigned)r.dieSides);
    return r;
}

void qpApplyReward(const RewardResult& r){
    g_qp.totalXp += r.xp;
    // A portion of every quest's XP also nurtures the mantis (picked up by Pet
    // Mode on next entry). This is the "quests feed the pet" link.
    g_qp.petXpPending += (r.xp / 3) + 1;
    if (r.gotItem) {
        // Rewards land in the INVENTORY only. Applying them is always the
        // player's explicit "use" (arm) from the quest inventory screen -
        // nothing auto-triggers on the next game anymore.
        uint32_t v=(uint32_t)g_qp.items[r.itemId]+1; if(v>9999)v=9999;
        g_qp.items[r.itemId]=(uint16_t)v;
    }
    if (r.gotDie) qpAddDie(r.dieSides, r.dieAttr);
    qpSave();
}

// ── Persistence ───────────────────────────────────────────────────────────────
void qpResetDefaults(){
    memset(&g_qp,0,sizeof(g_qp));
    g_qp.version=QP_VERSION;
    for(int s=0;s<QP_DICE_SLOTS;s++) g_qp.slot[s]=-1;
    qpAddDie(6,DA_NONE);
    qpAddDie(6,DA_NONE);   // two starter d6 so early CYOA checks are attemptable
    g_qp.storyUnlocks = STORY_HOLLOW;   // first story always available
}
// The exact v4 blob layout (items table was 21 wide; no pending[]).
struct QuestProfileV4 {
    uint32_t version;
    uint32_t totalXp;
    Die      dice[QP_MAX_DICE];
    int8_t   slot[QP_DICE_SLOTS];
    uint16_t items[21];
    uint32_t pinballPerksPending;
    uint32_t storyUnlocks;
    uint32_t petXpPending;
};

void qpLoad(){
    Preferences p; bool ok=false;
    if(p.begin("qprofile",true)){
        size_t n=p.getBytesLength("p");
        if(n==sizeof(QuestProfile)){
            p.getBytes("p",&g_qp,sizeof(QuestProfile));
            if(g_qp.version==QP_VERSION) ok=true;
        } else if(n==sizeof(QuestProfileV4)){
            // Migrate a v4 profile in place - keep everything earned.
            QuestProfileV4 v4;
            p.getBytes("p",&v4,sizeof(v4));
            if(v4.version==4){
                memset(&g_qp,0,sizeof(g_qp));
                g_qp.version      = QP_VERSION;
                g_qp.totalXp      = v4.totalXp;
                memcpy(g_qp.dice, v4.dice, sizeof(g_qp.dice));
                memcpy(g_qp.slot, v4.slot, sizeof(g_qp.slot));
                memcpy(g_qp.items, v4.items, sizeof(v4.items));
                g_qp.storyUnlocks = v4.storyUnlocks;
                g_qp.petXpPending = v4.petXpPending;
                // Legacy auto-queued perk bits become armed items (one each)
                // so a perk that was "in flight" still lands.
                if(v4.pinballPerksPending & PERK_MULTIBALL)  g_qp.pending[ITEM_PB_MULTIBALL]++;
                if(v4.pinballPerksPending & PERK_SLOWBALL)   g_qp.pending[ITEM_PB_SLOWBALL]++;
                if(v4.pinballPerksPending & PERK_BOOST)      g_qp.pending[ITEM_PB_BOOST]++;
                if(v4.pinballPerksPending & PERK_POWERFLIP)  g_qp.pending[ITEM_PB_POWERFLIP]++;
                if(v4.pinballPerksPending & PERK_PROJECTILE) g_qp.pending[ITEM_PB_PROJECTILE]++;
                ok=true;
            }
        }
        p.end();
    }
    if(ok){
        // Sweep any legacy aquarium pendings from the old "aqpend" namespace.
        Preferences a;
        if(a.begin("aqpend",false)){
            g_qp.pending[ITEM_AQ_COINPACK] += a.getUChar("coins",0);
            g_qp.pending[ITEM_AQ_RAREFISH] += a.getUChar("rare",0);
            g_qp.pending[ITEM_AQ_DECOR]    += a.getUChar("decor",0);
            a.clear(); a.end();
        }
        qpSave();
    } else { qpResetDefaults(); qpSave(); }
}
void qpSave(){
    Preferences p;
    if(p.begin("qprofile",false)){ p.putBytes("p",&g_qp,sizeof(QuestProfile)); p.end(); }
}
QuestProfile& qpData(){ return g_qp; }
