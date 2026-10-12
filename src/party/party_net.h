// party_net.h - N-player party framework over ESP-NOW (host-authoritative).
//
// Model (per spec):
//   * First device to open a Party Game is the HOST. No election, no ping-pong.
//   * Any other device that opens it discovers the host and is ASKED to join.
//     Yes -> it becomes a joiner. No -> it leaves to the menu.
//   * The HOST owns ALL truth: who is UP, the phase, every score, and the timer
//     (running flag + ms remaining + total). The host broadcasts a full STATE
//     snapshot every tick.
//   * JOINERS are dumb terminals: they hold the last snapshot and render exactly
//     what it says. They never run the game logic themselves. The only thing
//     they send is button events (join / guess / start).
//
// Games (charades now; trivia/board later) plug in via a PartyGame descriptor
// and implement only their own prompt + drawing. The framework drives turns,
// scoring, the shared clock, and the snapshot stream.
#pragma once
#include <stdint.h>

static const int PARTY_MAX_PLAYERS = 8;
static const int PARTY_NAME_LEN    = 12;

// Snapshot phase (host-authoritative; joiners mirror this).
enum PartyPhase : uint8_t {
    PH_LOBBY   = 0,   // gathering players (host waits, shows roster)
    PH_ARMED   = 1,   // UP player must press to start the timer
    PH_LIVE    = 2,   // timer running; non-UP players can guess
    PH_GUESS   = 3,   // someone buzzed in; UP player judges
    PH_REVEAL  = 4,   // correct answer shown briefly
    PH_SCORES  = 5,   // end-of-round scoreboard
    PH_OVER    = 6,   // someone hit the target
};

struct PartyPlayer {
    uint16_t id;                    // last 2 bytes of MAC (stable device id)
    char     name[PARTY_NAME_LEN];  // silly nickname
    uint16_t score;
    bool     active;
};

// ---- framework API used by a game ------------------------------------------
int          partyPlayerCount();
PartyPlayer* partyPlayer(int idx);
int          partyMyIndex();
int          partyWhoUp();
bool         partyIsHost();
bool         partyIAmUp();
PartyPhase   partyPhase();
uint32_t     partyRoundSeed();
uint32_t     partyMsLeft();       // synced timer remaining
uint32_t     partyMsTotal();
uint16_t     partyScoreTarget();

// Host-only helpers a game calls to drive the match:
void partyHostAward(int playerIdx, int pts);   // add points (host authoritative)
void partyHostSetPhase(PartyPhase ph);         // move the match to a phase
void partyHostNextTurn();                       // rotate UP + new round
uint8_t partyGuesser();                          // index of who buzzed (or 255)

// A pluggable game.
struct PartyGame {
    const char* name;
    void (*roundStart)(uint32_t seed);   // host & joiners: derive prompt from seed
    void (*drawPerform)();               // draw for the UP player
    void (*drawWatch)();                 // draw for a non-UP player (guesser view)
    void (*drawJudge)();                 // UP player's right/wrong judge screen
    void (*drawReveal)();                // the confirmed answer
    const char* (*promptText)();         // current prompt (for reveal/perform)
};

// ---- app entry points (wired in main.cpp) ----------------------------------
void partyEnter();
void partyTick();
void partyDraw();
void partyShutdown();
