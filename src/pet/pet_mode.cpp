// pet_mode.cpp - the mantis pet: care, training, battle, trade.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>
#include <math.h>
#include "pet_mode.h"
#include "../chord_guard.h"
#include "pet_state.h"
#include "../theme_colors.h"
#include "../quests/quest_profile.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

// ── Screens ───────────────────────────────────────────────────────────────────
enum PetScreen {
    PS_HOME,        // pet + action ring
    PS_MSG,         // transient message after a care action
    PS_HUNT,        // bug-hunting minigame (catch snacks when out of food)
    PS_BATTLE_MENU, // choose CPU vs Link
    PS_LINK,        // searching for a peer (battle or trade)
    PS_BATTLE,      // active turn battle
    PS_BATTLE_END,  // win/lose summary
    PS_TRADE,       // ESP-NOW trade screen
    PS_STATS        // detailed stats
};
static PetScreen ps = PS_HOME;

// Action ring
static const char* ACTIONS[] = { "FEED","HUNT","CLEAN","PLAY","TRAIN","BATTLE","TRADE","STATS" };
static const int   ACTION_COUNT = 8;
static int actionSel = 0;

// transient message
static char petMsg[40] = "";
static uint32_t petMsgAt = 0;

// ── Battle state (shared by CPU, training, and link battles) ──────────────────
enum BMove { BM_ATTACK=0, BM_GUARD=1, BM_SPECIAL=2 };
struct Combatant {
    char name[16];
    int  hp, maxHp;
    int  atk, def, spd;
    bool guarding;
    bool charging;    // mid-charge this turn: blocks 1/4 of incoming damage
    int  special;     // legacy charge count (kept for save compat)
    int  charge;      // turns of special charge built up (fires at CHARGE_FULL)
};
static const int CHARGE_FULL = 2;   // turns to charge before the 2x finisher
static Combatant me, foe;
static int  battleSel = 0;        // move/menu selection
static bool battleMyTurn = true;
static bool battleIsLink = false;
static bool battleIsTrain = false;
static char battleLog[40] = "";
static bool battleWon = false;
static bool battleAwaitingFoe = false;

// ── ESP-NOW (same proven pattern as Versus) ───────────────────────────────────
static const uint8_t PET_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static bool petEspReady = false;
static bool petLinked   = false;
static uint8_t petPeer[6];
static uint16_t petMyId = 0;

enum PetMsgType : uint8_t {
    PM_HELLO=1, PM_CONFIRM, PM_MOVE, PM_STATE, PM_TRADE_OFFER, PM_TRADE_OK, PM_BYE
};
struct PetMsg {
    uint8_t  type;
    uint16_t id;
    uint8_t  move;       // BMove for PM_MOVE
    int16_t  hp;         // sender's view of target hp after their move
    uint8_t  lvl;        // sender pet level
    uint8_t  atk, def, spd;
    uint8_t  itemId;     // PM_TRADE_OFFER / PM_TRADE_OK
};
static volatile bool rxHello=false, rxConfirm=false, rxMove=false,
                     rxTradeOffer=false, rxTradeOk=false, rxBye=false;
static volatile uint8_t rxMoveVal=0, rxItem=0, rxLvl=1, rxAtk=8, rxDef=5, rxSpd=6;
static uint8_t  rxMac[6];
static uint16_t rxId=0;

static void petOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if ((size_t)len < sizeof(PetMsg)) return;
    const PetMsg* m = (const PetMsg*)data;
    memcpy(rxMac, mac, 6); rxId = m->id;
    switch (m->type) {
        case PM_HELLO:       rxHello=true; break;
        case PM_CONFIRM:     rxConfirm=true; break;
        case PM_MOVE:        rxMoveVal=m->move; rxMove=true; break;
        case PM_TRADE_OFFER: rxItem=m->itemId; rxTradeOffer=true; break;
        case PM_TRADE_OK:    rxItem=m->itemId; rxTradeOk=true; break;
        case PM_BYE:         rxBye=true; break;
        default: break;
    }
    if (m->type==PM_HELLO || m->type==PM_CONFIRM) {
        rxLvl=m->lvl; rxAtk=m->atk; rxDef=m->def; rxSpd=m->spd;
    }
}
static bool petAddPeer(const uint8_t* mac){
    esp_now_peer_info_t p; memset(&p,0,sizeof(p));
    memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
    if (esp_now_is_peer_exist(mac)) return true;
    return esp_now_add_peer(&p)==ESP_OK;
}
static void petSend(uint8_t type, const uint8_t* to, uint8_t move=0, int16_t hp=0, uint8_t item=0){
    PetMsg m; memset(&m,0,sizeof(m));
    m.type=type; m.id=petMyId; m.move=move; m.hp=hp; m.itemId=item;
    m.lvl=(uint8_t)petLevel(); m.atk=petAttack(); m.def=petDefense(); m.spd=petSpeed();
    esp_now_send(to,(uint8_t*)&m,sizeof(m));
}
static bool petNetBegin(){
    if (petEspReady) return true;
    WiFi.mode(WIFI_STA);
    if (esp_now_init()!=ESP_OK) return false;
    esp_now_register_recv_cb(esp_now_recv_cb_t(petOnRecv));
    petAddPeer(PET_BCAST);
    petEspReady=true;
    if (petMyId==0) petMyId = (uint16_t)(esp_random() & 0xFFFF);
    return true;
}
static void petNetEnd(){
    if (petEspReady){
        if (petLinked) petSend(PM_BYE, petPeer);
        delay(20);
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        petEspReady=false;
    }
    petLinked=false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}
static void clearRx(){
    rxHello=rxConfirm=rxMove=rxTradeOffer=rxTradeOk=rxBye=false;
}

// link sub-purpose
static bool linkForBattle = true;
static uint32_t linkLastHello = 0;
static uint32_t linkStartAt = 0;

// ── Care actions ──────────────────────────────────────────────────────────────
static void showMsg(const char* m){
    strncpy(petMsg, m, sizeof(petMsg)-1); petMsg[sizeof(petMsg)-1]='\0';
    petMsgAt = millis(); ps = PS_MSG;
}

static void doFeed(){
    PetState& p = petData();
    if (qpUseItem(ITEM_PET_PROTEIN)) {
        petAddXp(25);
        if (p.hunger<100) p.hunger=(p.hunger+25>100)?100:p.hunger+25;
        showMsg("Protein! +25 XP, fed");
    } else if (qpUseItem(ITEM_PET_POTION)) {
        if (p.hunger<100) p.hunger=(p.hunger+40>100)?100:p.hunger+40;
        if (p.hp<petMaxHp()) p.hp=(p.hp+20>petMaxHp())?petMaxHp():p.hp+20;
        showMsg("Potion! fed + healed");
    } else if (qpUseItem(ITEM_PET_SNACK)) {
        if (p.hunger<100) p.hunger=(p.hunger+20>100)?100:p.hunger+20;
        if (p.happiness<100) p.happiness=(p.happiness+10>100)?100:p.happiness+10;
        showMsg("Snack! yum");
    } else {
        showMsg("No food! Try HUNT ->");
        return;
    }
    petAddXp(2);
    petSave();
}
static void doClean(){
    PetState& p = petData();
    if (qpUseItem(ITEM_PET_TONIC)) {
        p.clean=100; if (p.happiness<100) p.happiness=(p.happiness+15>100)?100:p.happiness+15;
        showMsg("Tonic! sparkling clean");
    } else {
        p.clean=(p.clean+30>100)?100:p.clean+30;
        showMsg("Scrubbed the cabinet");
    }
    petAddXp(2); petSave();
}
static void doPlay(){
    PetState& p = petData();
    if (p.energy < 15) { showMsg("Too tired to play"); return; }
    p.energy -= 15;
    if (p.happiness<100) p.happiness=(p.happiness+25>100)?100:p.happiness+25;
    petAddXp(5);
    showMsg("Played! +5 XP, happy");
    petSave();
}

// ── HUNT minigame ─────────────────────────────────────────────────────────────
// Over-the-shoulder view (like a Pokemon encounter): the mantis's head and
// raised forelimbs sit in the foreground at the bottom; bugs skitter across a
// receding ground plane, drifting in and out of a highlighted STRIKE ZONE
// band. Tap R to lash the claws out; if a bug is in the zone when the claws
// land, it's caught. Time-boxed (~25s) and costs energy to start. Caught bugs
// are banked as Snack items in the shared quest inventory so you can Feed with
// them later. Bug Bait (armed via Quests) makes richer bugs appear.
enum BugKind { BUG_GNAT=0, BUG_FLY, BUG_MOTH, BUG_BEETLE, BUG_FIREFLY };
struct HuntBug {
    bool     active;
    float    x;          // screen x
    float    depth;      // 0 (far) .. 1 (near); drives y + size
    float    vx;         // horizontal drift
    float    vDepth;     // moves toward/away
    uint8_t  kind;
    uint32_t bornAt;
};
static const int HUNT_MAX_BUGS = 5;
static HuntBug huntBugs[HUNT_MAX_BUGS];
static uint32_t huntEndAt   = 0;
static int      huntCaught  = 0;
static uint32_t huntStrikeAt = 0;    // claw-lunge animation start (0 = idle)
static bool     huntStruckHit = false;
static bool     huntBait     = false;
static uint32_t huntLastSpawn = 0;

// The strike zone is a horizontal band in the near-mid field. A bug is
// "in the zone" when its depth (nearness) falls in this window.
static constexpr float HUNT_ZONE_LO = 0.55f;
static constexpr float HUNT_ZONE_HI = 0.80f;
static constexpr int   HUNT_COST    = 20;    // energy to start a hunt
static constexpr uint32_t HUNT_TIME = 25000; // ms per hunt

static int huntBugSnackValue(uint8_t kind){
    // richer bugs = more likely to bank a snack; beetle/firefly are prizes
    switch(kind){ case BUG_BEETLE: return 2; case BUG_FIREFLY: return 2;
                  case BUG_MOTH: return 1; default: return 1; }
}

static void huntSpawnBug(){
    for (int i=0;i<HUNT_MAX_BUGS;i++){
        if (huntBugs[i].active) continue;
        HuntBug& b = huntBugs[i];
        b.active = true;
        b.x = 20 + esp_random()%(W-40);
        b.depth = 0.15f + (esp_random()%30)/100.0f;   // enters from the far field
        b.vx = ((esp_random()%100)/100.0f - 0.5f) * 1.4f;
        b.vDepth = 0.004f + (esp_random()%6)/1000.0f;  // drifts nearer
        // Bait tilts the spawn table toward the good bugs.
        int roll = esp_random()%100;
        if (huntBait) roll = (roll + 40) % 100;
        b.kind = (roll<40)?BUG_GNAT : (roll<65)?BUG_FLY : (roll<83)?BUG_MOTH
               : (roll<95)?BUG_BEETLE : BUG_FIREFLY;
        b.bornAt = millis();
        return;
    }
}

static void startHunt(){
    PetState& p = petData();
    if (p.energy < HUNT_COST){ showMsg("Too tired to hunt"); return; }
    p.energy -= HUNT_COST;
    huntBait = qpTakePending(ITEM_PET_BAIT) > 0;   // armed Bug Bait
    for (int i=0;i<HUNT_MAX_BUGS;i++) huntBugs[i].active=false;
    huntCaught = 0;
    huntStrikeAt = 0; huntStruckHit=false;
    huntEndAt = millis() + HUNT_TIME;
    huntLastSpawn = 0;
    ps = PS_HUNT;
    petSave();
}

static void huntResolveStrike(){
    // Claws land ~180ms into the lunge; catch the NEAREST bug in the zone.
    int best=-1; float bestDepth=-1;
    for (int i=0;i<HUNT_MAX_BUGS;i++){
        HuntBug& b=huntBugs[i];
        if (!b.active) continue;
        if (b.depth>=HUNT_ZONE_LO && b.depth<=HUNT_ZONE_HI){
            if (b.depth>bestDepth){ bestDepth=b.depth; best=i; }
        }
    }
    if (best>=0){
        huntStruckHit = true;
        int val = huntBugSnackValue(huntBugs[best].kind);
        qpAddItem(ITEM_PET_SNACK, (uint16_t)val);   // bank the snack(s)
        huntCaught += val;
        petAddXp(3);
        huntBugs[best].active=false;
    } else {
        huntStruckHit = false;
    }
}

static void huntTick(){
    uint32_t now = millis();
    bool rEdge = chordREdge();
    bool lEdge = chordLEdge();

    // L bails out of the hunt early, banking whatever was caught.
    if (lEdge){
        char m[40]; snprintf(m,sizeof(m),"Hunt done: %d snack%s",huntCaught,huntCaught==1?"":"s");
        petSave(); showMsg(m); return;
    }

    // strike
    if (rEdge && huntStrikeAt==0){
        huntStrikeAt = now;
        huntStruckHit = false;
    }
    if (huntStrikeAt){
        uint32_t t = now - huntStrikeAt;
        if (t>=180 && !huntStruckHit && t<200) huntResolveStrike();
        if (t>360) huntStrikeAt = 0;   // reset to idle after the lunge
    }

    // spawn cadence (a touch faster with bait)
    uint32_t gap = huntBait ? 700 : 1000;
    if (now - huntLastSpawn > gap){ huntSpawnBug(); huntLastSpawn = now; }

    // move bugs; retire ones that pass fully near (they "got away")
    for (int i=0;i<HUNT_MAX_BUGS;i++){
        HuntBug& b=huntBugs[i];
        if (!b.active) continue;
        b.x += b.vx;
        if (b.x<12){ b.x=12; b.vx=-b.vx; }
        if (b.x>W-12){ b.x=W-12; b.vx=-b.vx; }
        b.depth += b.vDepth;
        // occasional dart back and forth in depth so they weave in/out of zone
        if ((esp_random()%100)<3) b.vDepth = -b.vDepth;
        if (b.depth>1.05f || b.depth<0.05f) b.active=false;
    }

    if (now >= huntEndAt){
        char m[40]; snprintf(m,sizeof(m),"Hunt over: %d snack%s",huntCaught,huntCaught==1?"":"s");
        petSave(); showMsg(m);
    }
}

// ── Battle helpers ────────────────────────────────────────────────────────────
static void setupMe(){
    strncpy(me.name, petData().name, sizeof(me.name)-1); me.name[sizeof(me.name)-1]='\0';
    me.maxHp=petMaxHp(); me.hp=me.maxHp;
    me.atk=petAttack(); me.def=petDefense(); me.spd=petSpeed();
    me.guarding=false; me.charging=false; me.special=0; me.charge=0;
}
static void setupFoeCPU(bool training){
    int lvl=(int)petLevel();
    if (training){
        strncpy(foe.name,"Dummy",sizeof(foe.name)-1);
        int fl=lvl>1?lvl-1:1;
        foe.maxHp=20+fl*4; foe.atk=6+fl; foe.def=4+fl; foe.spd=4+fl;
    } else {
        static const char* names[]={"Beetle","Hornet","Spider","Roach","Wasp"};
        strncpy(foe.name,names[esp_random()%5],sizeof(foe.name)-1);
        int fl=lvl+(int)(esp_random()%3)-1; if(fl<1)fl=1;
        foe.maxHp=30+fl*6; foe.atk=8+fl*2; foe.def=5+fl*2; foe.spd=6+fl;
    }
    foe.name[sizeof(foe.name)-1]='\0';
    foe.hp=foe.maxHp; foe.guarding=false; foe.charging=false; foe.special=0; foe.charge=0;
}
static void setupFoeLink(){
    strncpy(foe.name,"Rival",sizeof(foe.name)-1); foe.name[sizeof(foe.name)-1]='\0';
    foe.maxHp=30+rxLvl*6; foe.hp=foe.maxHp;
    foe.atk=rxAtk; foe.def=rxDef; foe.spd=rxSpd; foe.guarding=false; foe.charging=false; foe.special=0; foe.charge=0;
}
static int applyMove(Combatant& a, Combatant& d, int move){
    if (move==BM_GUARD){ a.guarding=true; a.charging=false; snprintf(battleLog,sizeof(battleLog),"%s braces",a.name); return 0; }
    if (move==BM_SPECIAL){
        if (a.charge < CHARGE_FULL){
            // Charging: spend the turn winding up. No attack. You can't fully
            // guard, but you instinctively brace - charging blocks 1/4 of an
            // incoming hit (less than GUARD's 1/2). Still a real risk.
            a.charge++;
            a.guarding = false;
            a.charging = true;
            snprintf(battleLog,sizeof(battleLog),"%s charges... (%d/%d)",a.name,a.charge,CHARGE_FULL);
            return 0;
        }
        // Fully charged: unleash the finisher and reset. After two turns of
        // winding up (dealing no damage, only partial defense), a 2x hit would
        // be WORSE than just attacking three times - so the payoff is 3.5x,
        // clearly beating three normal hits and rewarding the risk.
        a.charge = 0;
        a.charging = false;
        int base = (a.atk * 7) / 2;             // 3.5x attack
        int dmg = base - d.def/2 + (int)(esp_random()%4);
        if (dmg < 6) dmg = 6;
        if (d.guarding){ dmg/=2; d.guarding=false; }
        else if (d.charging){ dmg = (dmg*3)/4; }   // charging blocks 1/4
        d.hp -= dmg; if (d.hp<0) d.hp=0;
        snprintf(battleLog,sizeof(battleLog),"%s SPECIAL! %d",a.name,dmg);
        return dmg;
    }
    // Normal attack
    int base = a.atk;
    bool crit = false;
    a.charging = false;
    int dmg = base - d.def/2 + (int)(esp_random()%3) - 1;
    if (dmg < 2) dmg = 2;
    if ((esp_random()%10)==0){ dmg += dmg/2; crit = true; }   // occasional crit
    if (d.guarding){ dmg/=2; d.guarding=false; }
    else if (d.charging){ dmg = (dmg*3)/4; }   // charging blocks 1/4
    d.hp -= dmg; if (d.hp<0) d.hp=0;
    if (crit) snprintf(battleLog,sizeof(battleLog),"%s crits %d!",a.name,dmg);
    else      snprintf(battleLog,sizeof(battleLog),"%s hits %d",a.name,dmg);
    return dmg;
}
static int cpuPickMove(){
    // If the finisher is charged, use it.
    if (foe.charge >= CHARGE_FULL) return BM_SPECIAL;
    // Low HP: play safe (guard or chip), don't expose self charging.
    if (foe.hp < foe.maxHp/4) return (esp_random()%2)? BM_GUARD:BM_ATTACK;
    // Mid fight: sometimes start charging the finisher (a gamble).
    if (foe.hp > foe.maxHp/2 && (esp_random()%4==0)) return BM_SPECIAL;
    return BM_ATTACK;
}
static void endBattle(bool won){
    battleWon=won;
    if (won){
        petData().battlesWon++;
        int reward = battleIsTrain?15:30;
        petAddXp(reward);
        snprintf(battleLog,sizeof(battleLog),"WON! +%d pet XP",reward);
    } else {
        petAddXp(5);
        snprintf(battleLog,sizeof(battleLog),"Lost... +5 XP");
    }
    petData().hp = me.hp>0?me.hp:1;
    petSave();
    ps=PS_BATTLE_END;
}
static void resolveTurnCPU(int myMove){
    me.guarding=false;
    if (me.spd >= foe.spd){
        applyMove(me, foe, myMove);
        if (foe.hp<=0){ endBattle(true); return; }
        applyMove(foe, me, cpuPickMove());
        if (me.hp<=0){ endBattle(false); return; }
    } else {
        applyMove(foe, me, cpuPickMove());
        if (me.hp<=0){ endBattle(false); return; }
        applyMove(me, foe, myMove);
        if (foe.hp<=0){ endBattle(true); return; }
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void petEnter(){
    chordGuardReset();
    fb.setRotation(0);
    petLoad();
    uint32_t pend = qpConsumePetXp();   // pet XP from quests/cyoa
    if (pend) petAddXp(pend);
    ps=PS_HOME; actionSel=0;
    // Items ARMED in the Quest inventory apply at the start of this session.
    // (Direct feeding from the FEED action still consumes from inventory too;
    // arming is the "set it up before you visit" path.)
    {
        PetState& pp = petData();
        uint16_t n; bool any=false; char m[40]="";
        if ((n = qpTakePending(ITEM_PET_SNACK))) {
            for (uint16_t i=0;i<n;i++){ pp.hunger=(uint8_t)min(100,pp.hunger+20);
                pp.happiness=(uint8_t)min(100,pp.happiness+10); }
            any=true;
        }
        if ((n = qpTakePending(ITEM_PET_POTION))) {
            for (uint16_t i=0;i<n;i++){ pp.hunger=(uint8_t)min(100,pp.hunger+40);
                pp.hp=(uint8_t)min((int)petMaxHp(),pp.hp+20); }
            any=true;
        }
        if ((n = qpTakePending(ITEM_PET_PROTEIN))) {
            for (uint16_t i=0;i<n;i++){ petAddXp(25);
                pp.hunger=(uint8_t)min(100,pp.hunger+25); }
            any=true;
        }
        if ((n = qpTakePending(ITEM_PET_TONIC))) {
            pp.clean=100; pp.happiness=(uint8_t)min(100,pp.happiness+15);
            any=true;
        }
        if (any){ petSave();
            snprintf(m,sizeof(m),"Quest treats applied!");
            showMsg(m);
        }
    }
    petEspReady=false; petLinked=false;
    if (petMyId==0) petMyId=(uint16_t)(esp_random()&0xFFFF);
}
void petShutdown(){
    if (petEspReady) petNetEnd();
    petSave();
}

static void startLink(bool forBattle){
    linkForBattle=forBattle;
    clearRx();
    if (!petNetBegin()){ showMsg("link failed"); return; }
    petLinked=false;
    linkStartAt=millis(); linkLastHello=0;
    ps=PS_LINK;
}

// ── Input / update ────────────────────────────────────────────────────────────
void petTick(){
    chordGuardTick();
    bool lEdge = chordLEdge();
    bool rEdge = chordREdge();

    petDecayTick();   // slow care decay only while open

    switch (ps){
    case PS_HOME:
        if (lEdge) actionSel=(actionSel+1)%ACTION_COUNT;
        if (rEdge){
            switch (actionSel){
                case 0: doFeed(); break;
                case 1: startHunt(); break;                     // HUNT
                case 2: doClean(); break;
                case 3: doPlay(); break;
                case 4: // TRAIN
                    battleIsTrain=true; battleIsLink=false;
                    setupMe(); setupFoeCPU(true);
                    battleSel=0; strncpy(battleLog,"A practice bout begins",sizeof(battleLog)-1);
                    battleLog[sizeof(battleLog)-1]='\0';
                    ps=PS_BATTLE; break;
                case 5: ps=PS_BATTLE_MENU; battleSel=0; break;  // BATTLE
                case 6: startLink(false); break;                // TRADE
                case 7: ps=PS_STATS; break;                     // STATS
            }
        }
        break;

    case PS_HUNT:
        huntTick();
        break;

    case PS_MSG:
        if (lEdge||rEdge||millis()-petMsgAt>1600) ps=PS_HOME;
        break;

    case PS_STATS:
        if (lEdge||rEdge) ps=PS_HOME;
        break;

    case PS_BATTLE_MENU:
        if (lEdge) battleSel^=1;
        if (rEdge){
            if (battleSel==0){
                battleIsTrain=false; battleIsLink=false;
                setupMe(); setupFoeCPU(false);
                battleSel=0; strncpy(battleLog,"A wild bug appears!",sizeof(battleLog)-1);
                battleLog[sizeof(battleLog)-1]='\0';
                ps=PS_BATTLE;
            } else startLink(true);
        }
        break;

    case PS_LINK: {
        uint32_t now=millis();
        if (now-linkLastHello>300){ petSend(PM_HELLO, PET_BCAST); linkLastHello=now; }
        if (rxHello && !petLinked){
            memcpy(petPeer,rxMac,6); petAddPeer(petPeer);
            petSend(PM_CONFIRM, petPeer); petLinked=true;
        }
        if (rxConfirm && !petLinked){
            memcpy(petPeer,rxMac,6); petAddPeer(petPeer); petLinked=true;
        }
        if (petLinked){
            clearRx();
            if (linkForBattle){
                battleIsLink=true; battleIsTrain=false;
                setupMe(); setupFoeLink();
                battleMyTurn = (petMyId <= rxId);   // lower id goes first
                battleSel=0; battleAwaitingFoe=false;
                strncpy(battleLog,"Linked! Battle!",sizeof(battleLog)-1);
                battleLog[sizeof(battleLog)-1]='\0';
                ps=PS_BATTLE;
            } else { ps=PS_TRADE; battleSel=0; }
        }
        if (lEdge||rEdge||now-linkStartAt>15000){ petNetEnd(); ps=PS_HOME; }
        break; }

    case PS_BATTLE:
        if (!battleIsLink){
            if (lEdge) battleSel=(battleSel+1)%3;
            if (rEdge) resolveTurnCPU(battleSel);
        } else {
            if (battleMyTurn && !battleAwaitingFoe){
                if (lEdge) battleSel=(battleSel+1)%3;
                if (rEdge){
                    me.guarding=false;
                    applyMove(me, foe, battleSel);
                    petSend(PM_MOVE, petPeer, battleSel, foe.hp);
                    if (foe.hp<=0){ endBattle(true); break; }
                    battleMyTurn=false; battleAwaitingFoe=true;
                }
            } else {
                if (rxMove){
                    rxMove=false;
                    Combatant foeTmp=foe;
                    applyMove(foeTmp, me, rxMoveVal); foe=foeTmp;
                    if (me.hp<=0){ endBattle(false); break; }
                    battleMyTurn=true; battleAwaitingFoe=false;
                }
                if (rxBye){ rxBye=false; endBattle(true); }
            }
        }
        break;

    case PS_BATTLE_END:
        if (lEdge||rEdge){ if (petEspReady) petNetEnd(); ps=PS_HOME; }
        break;

    case PS_TRADE: {
        static const uint8_t tradeItems[]={ITEM_PET_SNACK,ITEM_PET_POTION,ITEM_PET_PROTEIN,ITEM_PET_TONIC};
        if (lEdge) battleSel=(battleSel+1)%4;
        if (rEdge){
            uint8_t it=tradeItems[battleSel];
            if (qpItemCount(it)>0){ petSend(PM_TRADE_OFFER, petPeer, 0,0, it); showMsg("Offer sent"); }
            else showMsg("You have none of that");
        }
        if (rxTradeOffer){
            rxTradeOffer=false;
            qpAddItem(rxItem,1);
            petSend(PM_TRADE_OK, petPeer, 0,0, rxItem);
            showMsg("Received an item!");
        }
        if (rxTradeOk){
            rxTradeOk=false;
            qpUseItem(rxItem);   // our offered item leaves inventory
            showMsg("Trade complete");
        }
        if (rxBye){ rxBye=false; petNetEnd(); ps=PS_HOME; }
        break; }
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────
// Mood drives the mantis's pose and animation. Derived from its stats.
enum PetMood { MOOD_HAPPY, MOOD_CONTENT, MOOD_HUNGRY, MOOD_DIRTY, MOOD_SAD };
static PetMood petMoodNow(){
    PetState& p = petData();
    if (p.hp < petMaxHp()/4)           return MOOD_SAD;
    if (p.hunger < 30)                 return MOOD_HUNGRY;
    if (p.clean < 30)                  return MOOD_DIRTY;
    if (p.happiness > 70 && p.hunger > 50) return MOOD_HAPPY;
    return MOOD_CONTENT;
}

// A fully animated, expressive mantis. `cx,cy` is the standing point (feet),
// `s` the body scale. `t` is a free-running time in ms. The pose breathes,
// sways, blinks, waves antennae and forelegs, and shifts with mood.
static void drawMantisAnimated(int cx, int feetY, int s, uint32_t t, PetMood mood){
    uint16_t body=LIME, dark=LIME_DARK, eyeC=PURPLE_DARK, accent=LIME_PALE;
    if (mood==MOOD_DIRTY){ body=rgb565(120,140,70); dark=rgb565(80,95,45); }   // grimy green
    if (mood==MOOD_SAD){   body=rgb565(120,160,120); dark=rgb565(80,110,80); } // pale, wan

    // ---- timing signals ----
    float ts = t / 1000.0f;
    // breathing: gentle vertical scale pulse
    float breath = sinf(ts * 2.2f);
    // sway: side-to-side lean, faster & wider when happy, slow when sad
    float swaySpd = (mood==MOOD_HAPPY)?3.0f : (mood==MOOD_SAD?1.0f:1.8f);
    float swayAmp = (mood==MOOD_HAPPY)?4.0f : (mood==MOOD_SAD?1.0f:2.5f);
    float sway = sinf(ts * swaySpd) * swayAmp;
    // squat bob: happy mantis bounces (a little squat-and-rise)
    float bob = 0;
    if (mood==MOOD_HAPPY) bob = -fabsf(sinf(ts*3.0f))*4.0f;       // bounce up
    else if (mood==MOOD_HUNGRY) bob = fabsf(sinf(ts*1.2f))*2.0f;  // droop down
    // blink: eyes shut briefly on a slow cycle
    int blinkPhase = (int)(t % 3200);
    bool blinking = blinkPhase < 140;

    int headLift = (mood==MOOD_SAD || mood==MOOD_HUNGRY) ? 6 : 0;  // head hangs when low

    // body anchor: feet fixed, body sways/bobs above
    float bodyH = s;
    int hipY  = feetY - 2;
    int cxB = cx + (int)sway;                        // body x leans with sway
    int abdY = hipY - (int)(bodyH*0.5f);             // abdomen center
    int thoraxY = abdY - (int)(bodyH*0.32f) + (int)bob;
    int headY = thoraxY - (int)(bodyH*0.28f) + headLift + (int)bob;

    // ---- six legs, all rooted on the (swaying) body so they stay attached, ----
    // each bent at a "knee" like a real insect leg, feet resting near the base.
    // A tiny per-leg phase gives a subtle living shuffle without lifting off.
    float ts2 = ts;
    int baseY = feetY + 1;                            // where feet rest
    // helper inline: draw a 2-segment leg from a body root to a planted foot
    // (root on body, knee out and down, foot on the ground)
    #define MANTIS_LEG(rootX, rootY, kneeDX, kneeDY, footDX, ph) do { \
        int _rx=(rootX), _ry=(rootY); \
        int _kx=_rx+(kneeDX), _ky=_ry+(kneeDY); \
        int _fx=_rx+(footDX)+(int)(sinf(ts2*2.0f+(ph))*1.5f); \
        fb.drawLine(_rx,_ry,_kx,_ky,dark); \
        fb.drawLine(_kx,_ky,_fx,baseY,dark); \
    } while(0)

    // front pair (from lower thorax), reach forward
    MANTIS_LEG(cxB-3, thoraxY+4, -7, 5, -11, 0.0f);
    MANTIS_LEG(cxB+3, thoraxY+4,  7, 5,  11, 0.6f);
    // middle pair (from upper abdomen)
    MANTIS_LEG(cxB-3, abdY-2, -9, 4, -13, 1.2f);
    MANTIS_LEG(cxB+3, abdY-2,  9, 4,  13, 1.8f);
    // hind pair (from lower abdomen), reach back, longest
    MANTIS_LEG(cxB-3, abdY+3, -8, 6, -10, 2.4f);
    MANTIS_LEG(cxB+3, abdY+3,  8, 6,  10, 3.0f);
    #undef MANTIS_LEG

    // ---- abdomen (breathing pulse on width) ----
    int abdW = 6 + (int)(breath*1.0f);
    fb.fillRoundRect(cxB-abdW/2, abdY-(int)(bodyH*0.18f), abdW, (int)(bodyH*0.5f), 3, body);
    // segment lines for detail
    for(int i=1;i<4;i++) fb.drawFastHLine(cxB-abdW/2+1, abdY-(int)(bodyH*0.18f)+i*(int)(bodyH*0.12f), abdW-2, dark);

    // ---- thorax ----
    fb.fillRoundRect(cxB-4, thoraxY, 8, (thoraxY<abdY? abdY-thoraxY: 6), 3, dark);

    // ---- raptorial forelegs: animated "prayer" that opens/strikes ----
    // happy: wave them; content: slow praying motion; hungry: reach out
    float armPhase = sinf(ts * (mood==MOOD_HAPPY?4.0f:2.0f));
    int armReach = (mood==MOOD_HUNGRY)? 6 : 0;
    int elbowX = 10 + (int)(armPhase*3) + armReach;
    int elbowY = (int)(armPhase*4);
    // left arm
    fb.drawLine(cxB-3, thoraxY+2, cxB-elbowX, thoraxY-2+elbowY, body);
    fb.drawLine(cxB-elbowX, thoraxY-2+elbowY, cxB-elbowX+4, thoraxY-9+elbowY, accent); // forearm (lighter)
    // right arm (mirror, slightly offset phase for life)
    float armPhase2 = sinf(ts * (mood==MOOD_HAPPY?4.0f:2.0f) + 0.6f);
    int elbowX2 = 10 + (int)(armPhase2*3) + armReach;
    int elbowY2 = (int)(armPhase2*4);
    fb.drawLine(cxB+3, thoraxY+2, cxB+elbowX2, thoraxY-2+elbowY2, body);
    fb.drawLine(cxB+elbowX2, thoraxY-2+elbowY2, cxB+elbowX2-4, thoraxY-9+elbowY2, accent);

    // ---- prothorax "neck": the long ovular segment mantises are known for, ----
    // connecting the thorax up to the head so the triangular head isn't floating.
    // It leans slightly with the head and tapers from a wider base to a narrow top.
    int neckTopY = headY + 5;            // just under the head
    int neckBotY = thoraxY + 1;          // top of the thorax
    int neckLen  = neckBotY - neckTopY;
    if (neckLen < 6) neckLen = 6;
    // a subtle lean: the neck's top tracks the head's tiny bob/sway
    int neckLean = (int)(sway * 0.3f);
    int ntx = cxB + neckLean;            // neck top x
    int nbx = cxB;                       // neck base x
    // draw as a stack of short horizontal spans, tapering wide->narrow bottom->top,
    // giving a smooth ovular column (filled, with a darker centerline seam)
    for (int i = 0; i <= neckLen; i++){
        float f = (float)i / (float)neckLen;          // 0 at base .. 1 at top
        int y = neckBotY - i;
        int halfW = 3 - (int)(f * 1.0f);              // 3px base -> 2px top
        if (halfW < 2) halfW = 2;
        int xc = nbx + (int)((ntx - nbx) * f);
        fb.drawFastHLine(xc - halfW, y, halfW*2, body);
    }
    // centerline seam for a little dimensionality
    fb.drawLine(nbx, neckBotY, ntx, neckTopY, dark);
    // a couple of fine ridge ticks along the neck (mantis segmentation)
    for (int i = 2; i < neckLen-1; i += 3){
        float f = (float)i / (float)neckLen;
        int y = neckBotY - i;
        int xc = nbx + (int)((ntx - nbx) * f);
        fb.drawPixel(xc - 2, y, dark);
        fb.drawPixel(xc + 2, y, dark);
    }

    // ---- head (triangular), seated on top of the neck ----
    fb.fillTriangle(cxB-6, headY+6, cxB+6, headY+6, cxB, headY-4, body);
    // ---- eyes (big, expressive) ----
    int eyeY = headY+2;
    if (blinking){
        fb.drawFastHLine(cxB-5, eyeY, 3, eyeC);     // closed: a line
        fb.drawFastHLine(cxB+2, eyeY, 3, eyeC);
    } else {
        fb.fillCircle(cxB-3, eyeY, 2, accent);      // white
        fb.fillCircle(cxB+3, eyeY, 2, accent);
        // pupils track the sway for a "looking around" feel
        int pd = (sway>0)?1:(sway<0?-1:0);
        fb.fillCircle(cxB-3+pd, eyeY, 1, eyeC);
        fb.fillCircle(cxB+3+pd, eyeY, 1, eyeC);
        if (mood==MOOD_HAPPY){ // happy sparkle
            fb.drawPixel(cxB-4, eyeY-1, TFT_WHITE);
            fb.drawPixel(cxB+2, eyeY-1, TFT_WHITE);
        }
    }
    // ---- mouth/mandible mood cue ----
    if (mood==MOOD_HAPPY)      fb.drawLine(cxB-2, headY+6, cxB+2, headY+6, accent);          // content line
    else if (mood==MOOD_SAD)   fb.drawLine(cxB-2, headY+7, cxB+2, headY+5, eyeC);            // frown tilt
    else if (mood==MOOD_HUNGRY)fb.fillCircle(cxB, headY+6, 1, eyeC);                          // open (hungry)

    // ---- antennae: wave independently ----
    float aw = sinf(ts*3.5f)*3.0f;
    fb.drawLine(cxB-3, headY-3, cxB-6+(int)aw, headY-12, body);
    fb.drawLine(cxB+3, headY-3, cxB+6+(int)(-aw), headY-12, body);

    // ---- mood FX ----
    if (mood==MOOD_DIRTY){
        // A small swarm of flies buzzing erratically around the whole mantis.
        // Each fly orbits a center on its own radius/speed, with a fast jitter
        // so the path looks buzzy, plus flickering wing dots.
        int swarmCx = cxB;
        int swarmCy = (thoraxY + abdY) / 2;
        const int NFLIES = 5;
        for(int i=0;i<NFLIES;i++){
            float spd = 2.2f + i*0.5f;                       // each fly its own speed
            float ang = ts*spd + i*1.7f;                     // orbit angle
            float rad = 14 + (i%3)*5;                        // orbit radius varies
            // erratic buzz: high-frequency jitter on top of the orbit
            float jx = sinf(ts*17.0f + i*3.0f)*2.0f;
            float jy = cosf(ts*19.0f + i*2.0f)*2.0f;
            int fx = swarmCx + (int)(cosf(ang)*rad + jx);
            int fy = swarmCy + (int)(sinf(ang)*rad*0.7f + jy);
            // body
            fb.fillCircle(fx, fy, 1, PURPLE_DARK);
            // flickering wings (toggle on a fast cycle)
            if (((int)(ts*20.0f + i) & 1)){
                fb.drawPixel(fx-2, fy-1, PURPLE_LIGHT);
                fb.drawPixel(fx+2, fy-1, PURPLE_LIGHT);
            } else {
                fb.drawPixel(fx-2, fy,   PURPLE_LIGHT);
                fb.drawPixel(fx+2, fy,   PURPLE_LIGHT);
            }
        }
        // a faint stink-squiggle rising off the back
        float sq = sinf(ts*6.0f);
        int sx = cxB - 10;
        fb.drawLine(sx, abdY-2, sx+(int)(sq*2), abdY-8, dark);
        fb.drawLine(sx+(int)(sq*2), abdY-8, sx-(int)(sq*2), abdY-14, dark);
    }
    if (mood==MOOD_HAPPY){
        // occasional musical/heart note rising
        int notePhase = (int)(t % 2600);
        if (notePhase < 1300){
            int ny = thoraxY - 10 - notePhase/60;
            int nx = cxB + 12 + (int)(sinf(notePhase/200.0f)*3);
            fb.fillCircle(nx, ny, 2, PURPLE_LIGHT);
            fb.drawFastVLine(nx+2, ny-5, 5, PURPLE_LIGHT);
        }
    }
    if (mood==MOOD_SAD || mood==MOOD_HUNGRY){
        // a single droplet (sweat/tear) every few seconds
        int dPhase = (int)(t % 4000);
        if (dPhase < 1000){
            int dy = headY + 4 + dPhase/120;
            fb.fillCircle(cxB+7, dy, 1, TEAL_LIGHT);
        }
    }
}

// Back-compat wrapper for the battle screen (simple, centered pose).
static void drawMantis(int cx, int cy, int s){
    drawMantisAnimated(cx, cy + s/2, s, millis(), petMoodNow());
}

static void statBar(const char* label, int val, int maxv, int y, uint16_t col){
    fb.setTextDatum(ML_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(label, 6, y+4, 1);
    int bx=44, bw=W-bx-8, bh=8;
    fb.drawRect(bx,y,bw,bh,TEAL_DARK);
    int fw = maxv? (val*(bw-2))/maxv : 0; if(fw<0)fw=0; if(fw>bw-2)fw=bw-2;
    fb.fillRect(bx+1,y+1,fw,bh-2,col);
}

static void drawHome(){
    PetState& p = petData();
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString(p.name, 4, 9, 1);
    fb.setTextDatum(MR_DATUM);
    char lv[16]; snprintf(lv,sizeof(lv),"Lv %u",(unsigned)petLevel());
    fb.drawString(lv, W-4, 9, 1);

    // Animated, expressive mantis reacting to its mood.
    PetMood mood = petMoodNow();
    drawMantisAnimated(W/2, 110, 44, millis(), mood);
    // a soft mood caption
    static const char* MOODTXT[] = {"happy!","content","hungry...","needs a wash","not feeling well"};
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(MOODTXT[mood], W/2, 116, 1);

    statBar("HP",   p.hp, petMaxHp(), 120, LIME);
    statBar("FOOD", p.hunger,100,134, TEAL_BRIGHT);
    statBar("FUN",  p.happiness,100,148, PURPLE_LIGHT);
    statBar("NRG",  p.energy,100,162, TEAL_LIGHT);
    statBar("CLEAN",p.clean,100,176, LIME_PALE);
    statBar("XP",   petXp(), petXpForNext(), 190, LIME_DARK);

    int ay=232;
    fb.drawFastHLine(0, ay-6, W, TEAL_DARK);
    fb.setTextDatum(MC_DATUM);
    for (int d=-1; d<=1; d++){
        int idx=((actionSel+d)%ACTION_COUNT+ACTION_COUNT)%ACTION_COUNT;
        int x=W/2 + d*56;
        if (d==0){ fb.setTextColor(LIME); fb.drawString(ACTIONS[idx], x, ay+10, 2); }
        else     { fb.setTextColor(TEAL_DARK); fb.drawString(ACTIONS[idx], x, ay+10, 1); }
    }
    static const char* DESC[]={
        "Feed using pet items","Hunt bugs for snacks","Clean the cabinet","Play (uses energy)",
        "Train for XP (solo)","Battle CPU or a friend","Trade items w/ a friend",
        "View detailed stats" };
    fb.setTextColor(TEAL_LIGHT); fb.setTextDatum(MC_DATUM);
    fb.drawString(DESC[actionSel], W/2, ay+34, 1);

    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextColor(LIME_PALE);
    fb.setTextDatum(ML_DATUM); fb.drawString("L: scroll", 4, H-7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: select", W-4, H-7, 1);
}

static void drawMsg(){
    drawHome();
    fb.fillRoundRect(12, H/2-24, W-24, 48, 8, PURPLE);
    fb.drawRoundRect(12, H/2-24, W-24, 48, 8, LIME);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(petMsg, W/2, H/2, 1);
}

static void drawStats(){
    PetState& p=petData();
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("STATS",4,9,1);
    fb.setTextColor(TEAL_LIGHT); fb.setTextDatum(TL_DATUM);
    char b[40]; int y=28;
    snprintf(b,sizeof(b),"Name: %s",p.name); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Level: %u",(unsigned)petLevel()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"XP: %u / %u",(unsigned)petXp(),(unsigned)petXpForNext()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"HP: %u / %u",p.hp,petMaxHp()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Attack: %u",petAttack()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Defense: %u",petDefense()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Speed: %u",petSpeed()); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Battles won: %u",(unsigned)p.battlesWon); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Food:%u Fun:%u",p.hunger,p.happiness); fb.drawString(b,6,y,1); y+=16;
    snprintf(b,sizeof(b),"Energy:%u Clean:%u",p.energy,p.clean); fb.drawString(b,6,y,1);
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("press to go back",W/2,H-7,1);
}

static void drawHunt(){
    uint32_t now = millis();
    // sky-to-ground gradient backdrop (over-the-shoulder field)
    fb.fillSprite(rgb565(30, 46, 40));
    // receding ground plane: horizon around y=70, ground fills below
    int horizon = 78;
    fb.fillRect(0, horizon, W, H-horizon, rgb565(46, 60, 40));
    // ground perspective lines converging toward a vanishing point
    for (int i=-3;i<=3;i++){
        int topx = W/2 + i*10;
        int botx = W/2 + i*70;
        fb.drawLine(topx, horizon, botx, H-40, rgb565(38,50,34));
    }
    for (int gy=horizon; gy<H-40; gy+=14){
        fb.drawFastHLine(0, gy, W, rgb565(40,52,36));
    }

    // strike-zone band (map depth window -> screen y)
    auto depthToY = [&](float d){ return (int)(horizon + d*(float)(H-40-horizon)); };
    int zTop = depthToY(HUNT_ZONE_LO), zBot = depthToY(HUNT_ZONE_HI);
    fb.fillRect(0, zTop, W, zBot-zTop, rgb565(58, 74, 44));
    fb.drawFastHLine(0, zTop, W, LIME_DARK);
    fb.drawFastHLine(0, zBot, W, LIME_DARK);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME_DARK);
    fb.drawString("strike zone", W-3, zTop+7, 1);

    // bugs (draw far-to-near so nearer bugs overlap)
    for (int pass=0; pass<2; pass++)
    for (int i=0;i<HUNT_MAX_BUGS;i++){
        HuntBug& b=huntBugs[i];
        if (!b.active) continue;
        bool near = b.depth>0.5f;
        if ((pass==0)==near) continue;
        int by = depthToY(b.depth);
        int sz = 2 + (int)(b.depth*7.0f);
        uint16_t col, wing;
        switch(b.kind){
            case BUG_GNAT:    col=rgb565(150,150,140); wing=rgb565(190,190,180); break;
            case BUG_FLY:     col=rgb565(90,90,100);   wing=rgb565(150,150,170); break;
            case BUG_MOTH:    col=rgb565(180,160,120); wing=rgb565(220,205,170); break;
            case BUG_BEETLE:  col=rgb565(80,140,70);   wing=rgb565(120,200,110); break;
            default:          col=LIME;                wing=LIME_PALE; break;   // firefly
        }
        // wings flutter
        int flap = ((now/60 + i*7) % 2) ? sz : sz/2;
        fb.fillEllipse((int)b.x - sz, by, flap, sz/2+1, wing);
        fb.fillEllipse((int)b.x + sz, by, flap, sz/2+1, wing);
        fb.fillCircle((int)b.x, by, sz, col);
        if (b.kind==BUG_FIREFLY && (now/200%2)) fb.fillCircle((int)b.x, by+sz, 2, LIME_PALE);
        // in-zone highlight ring
        if (b.depth>=HUNT_ZONE_LO && b.depth<=HUNT_ZONE_HI)
            fb.drawCircle((int)b.x, by, sz+3, LIME);
    }

    // ── foreground: over-the-shoulder mantis head + striking forelimbs ──
    bool lunging = (huntStrikeAt!=0);
    float lungeT = lunging ? (float)(now - huntStrikeAt)/360.0f : 0.0f;
    if (lungeT>1) lungeT=1;
    // reach peaks mid-lunge then retracts
    float reach = lunging ? sinf(lungeT*3.14159f) : 0.0f;
    int baseY = H - 18;
    // head silhouette (big, close, bottom-center)
    fb.fillTriangle(W/2-38, H, W/2+38, H, W/2, baseY-26, rgb565(70,110,60));
    fb.fillCircle(W/2-16, baseY-6, 6, rgb565(20,25,20));  // compound eye L
    fb.fillCircle(W/2+16, baseY-6, 6, rgb565(20,25,20));  // compound eye R
    fb.fillCircle(W/2-16, baseY-7, 2, LIME_PALE);
    fb.fillCircle(W/2+16, baseY-7, 2, LIME_PALE);
    // two rattling forelimbs that shoot up into the strike zone
    int clawTargetY = (zTop+zBot)/2;
    int clawY = (int)(baseY - reach * (baseY - clawTargetY));
    for (int side=-1; side<=1; side+=2){
        int hx = W/2 + side*22;
        int elbowX = W/2 + side*40;
        int elbowY = baseY - 10;
        fb.drawLine(hx, baseY, elbowX, elbowY, rgb565(90,140,70));
        fb.drawLine(elbowX, elbowY, W/2 + side*14, clawY, rgb565(110,170,90));
        // grasping claw
        fb.drawLine(W/2 + side*14, clawY, W/2 + side*6, clawY-6, LIME);
        fb.drawLine(W/2 + side*14, clawY, W/2 + side*22, clawY-4, LIME);
    }
    // strike flash on a catch
    if (lunging && huntStruckHit && lungeT>0.4f && lungeT<0.8f){
        fb.drawCircle(W/2, clawY, 12, LIME_PALE);
        fb.drawCircle(W/2, clawY, 16, LIME);
    }

    // ── HUD ──
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("HUNT", 4, 9, 1);
    int msLeft = (int)(huntEndAt>now ? huntEndAt-now : 0);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(msLeft<6000?PURPLE_LIGHT:TEAL_BRIGHT);
    char t[16]; snprintf(t,sizeof(t),"%d.%ds", msLeft/1000, (msLeft%1000)/100);
    fb.drawString(t, W/2, 9, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(LIME);
    char cc[16]; snprintf(cc,sizeof(cc),"snacks %d", huntCaught);
    fb.drawString(cc, W-4, 9, 1);
    if (huntBait){ fb.setTextDatum(MC_DATUM); fb.setTextColor(TFT_GOLD);
        fb.drawString("BAIT ACTIVE", W/2, 30, 1); }

    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L: done", 4, H-7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: strike zone", W-4, H-7, 1);
}

static void drawBattleMenu(){
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("BATTLE",4,9,1);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(battleSel==0?LIME:TEAL_DARK); fb.drawString("vs CPU", W/2, 90, 2);
    fb.setTextColor(battleSel==1?LIME:TEAL_DARK); fb.drawString("vs Friend (link)", W/2, 130, 2);
    fb.setTextColor(TEAL_LIGHT);
    fb.drawString(battleSel==0?"Fight a random bug":"Find a nearby device", W/2, 170, 1);
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("L: switch",4,H-7,1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: choose",W-4,H-7,1);
}

static void drawLink(){
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(linkForBattle?"LINK BATTLE":"LINK TRADE", 4,9,1);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("Searching for a", W/2, 120, 2);
    fb.drawString("nearby device...", W/2, 144, 2);
    int dots=(millis()/400)%4; char d[5]={0}; for(int i=0;i<dots;i++) d[i]='.';
    fb.setTextColor(LIME); fb.drawString(d, W/2, 168, 2);
    fb.setTextColor(TEAL_DARK);
    fb.drawString("Both pick the same", W/2, 200, 1);
    fb.drawString("mode to connect.", W/2, 214, 1);
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("press to cancel",W/2,H-7,1);
}

static void hpBar(Combatant& c, int y){
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString(c.name, 6, y, 1);
    int bx=6, bw=W-12, bh=8, by=y+10;
    fb.drawRect(bx,by,bw,bh,TEAL_DARK);
    int fw=c.maxHp?(c.hp*(bw-2))/c.maxHp:0; if(fw<0)fw=0;
    uint16_t col=(c.hp>c.maxHp/2)?LIME:(c.hp>c.maxHp/4?TEAL_BRIGHT:PURPLE_LIGHT);
    fb.fillRect(bx+1,by+1,fw,bh-2,col);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_DARK);
    char h[16]; snprintf(h,sizeof(h),"%d/%d",c.hp,c.maxHp); fb.drawString(h,W-6,y,1);
}

static void drawBattle(){
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString(battleIsTrain?"TRAINING":(battleIsLink?"LINK BATTLE":"BATTLE"), W/2,9,1);
    hpBar(foe, 28);
    drawMantis(W/2, 96, 28);
    hpBar(me, 150);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(battleLog, W/2, 188, 1);

    bool ourTurn = (!battleIsLink) || (battleMyTurn && !battleAwaitingFoe);
    static const char* MOVES[]={"ATTACK","GUARD","SPECIAL"};
    int my=210;
    if (ourTurn){
        for (int i=0;i<3;i++){
            int ry=my+i*22;
            if (i==battleSel){ fb.fillRoundRect(8,ry-2,W-16,20,4,PURPLE_DARK); fb.setTextColor(LIME);}
            else fb.setTextColor(TEAL_DARK);
            fb.setTextDatum(ML_DATUM);
            char row[28];
            if (i==BM_SPECIAL){
                if (me.charge >= CHARGE_FULL) snprintf(row,sizeof(row),"%s READY!",MOVES[i]);
                else snprintf(row,sizeof(row),"%s charge %d/%d",MOVES[i],me.charge,CHARGE_FULL);
            }
            else snprintf(row,sizeof(row),"%s",MOVES[i]);
            fb.drawString(row, 14, ry+8, 1);
        }
        fb.fillRect(0,H-14,W,14,PURPLE_DARK);
        fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("L: move",4,H-7,1);
        fb.setTextDatum(MR_DATUM); fb.drawString("R: do it",W-4,H-7,1);
    } else {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("foe's turn...", W/2, my+20, 1);
    }
}

static void drawBattleEnd(){
    fb.fillSprite(TEAL_DEEP);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(battleWon?LIME:PURPLE_LIGHT);
    fb.drawString(battleWon?"VICTORY!":"DEFEAT", W/2, 90, 4);
    fb.setTextColor(LIME_PALE); fb.drawString(battleLog, W/2, 140, 1);
    fb.setTextColor(TEAL_LIGHT);
    char lv[28]; snprintf(lv,sizeof(lv),"Pet Lv %u (%u/%u)",
        (unsigned)petLevel(),(unsigned)petXp(),(unsigned)petXpForNext());
    fb.drawString(lv, W/2, 170, 1);
    fb.setTextColor(TEAL_DARK); fb.drawString("press to continue", W/2, H-20, 1);
}

static void drawTrade(){
    fb.fillRect(0,0,W,18,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("TRADE",4,9,1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(petLinked?LIME:PURPLE_LIGHT);
    fb.drawString(petLinked?"linked":"...", W-4,9,1);
    static const char* NAMES[]={"Snack","Potion","Protein","Tonic"};
    static const uint8_t ids[]={ITEM_PET_SNACK,ITEM_PET_POTION,ITEM_PET_PROTEIN,ITEM_PET_TONIC};
    fb.setTextColor(TEAL_LIGHT); fb.setTextDatum(MC_DATUM);
    fb.drawString("Offer a pet item:", W/2, 36, 1);
    for (int i=0;i<4;i++){
        int ry=60+i*34;
        if (i==battleSel){ fb.fillRoundRect(10,ry-2,W-20,30,4,PURPLE_DARK); fb.setTextColor(LIME);}
        else fb.setTextColor(TEAL_DARK);
        fb.setTextDatum(ML_DATUM);
        char row[28]; snprintf(row,sizeof(row),"%s  x%u",NAMES[i],(unsigned)qpItemCount(ids[i]));
        fb.drawString(row, 16, ry+8, 2);
    }
    fb.fillRect(0,H-14,W,14,PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE); fb.drawString("L: pick",4,H-7,1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: offer",W-4,H-7,1);
}

void petDraw(){
    fb.fillSprite(TEAL_DEEP);
    switch (ps){
        case PS_HOME:        drawHome(); break;
        case PS_MSG:         drawMsg(); break;
        case PS_HUNT:        drawHunt(); break;
        case PS_STATS:       drawStats(); break;
        case PS_BATTLE_MENU: drawBattleMenu(); break;
        case PS_LINK:        drawLink(); break;
        case PS_BATTLE:      drawBattle(); break;
        case PS_BATTLE_END:  drawBattleEnd(); break;
        case PS_TRADE:       drawTrade(); break;
        default:             drawHome(); break;
    }
    fb.pushSprite(0,0);
}
