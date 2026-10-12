// FILE: src/party/party_board.h
// Parallel board-game framework for Party Mode's turn-based games.
//
// WHY THIS EXISTS SEPARATELY: party_net.cpp is the charades framework - a
// host-authoritative snapshot stream built around a performer/guesser phase
// machine. The turn-based board games (Connect Four, Battleship, Tic-Tac-Toe,
// Hold'em, the board-game clone, RPS, Hangman) are a different interaction:
// players take turns submitting MOVES, and each device can own private state.
// Rather than rewrite the working charades engine, this framework sits beside
// it and provides a generic turn/move core plus REUSABLE MODULES that any
// board game opts into (grid cursor, keyboard, deck, dice, wager, hidden board).
//
// TRANSPORT is the proven Versus/Pet house pattern, NOT a snapshot stream:
//   * recv callback only memcpy's into a buffer + sets a volatile flag
//   * HELLO 300ms -> CONFIRM, roles by chip id (lowest id = host)
//   * a small flat message struct, seq-deduped
//   * BYE -> delay -> unregister -> deinit -> WIFI_OFF teardown
//   * radio never comes up in solo
//
// A game implements BoardGame and calls into the shared modules it wants. The
// framework drives: discovery/lobby, whose turn it is, broadcasting each move
// to peers, and the game-over/rematch flow. The game decides everything else.
#pragma once
#include <stdint.h>

static const int PB_MAX_PLAYERS = 6;
static const int PB_NAME_LEN    = 12;
static const int PB_MOVE_BYTES  = 24;   // opaque per-move payload a game defines

// ---- a move: opaque bytes the game encodes/decodes itself ------------------
struct PbMove {
    uint8_t player;                 // who made it (seat index)
    uint8_t len;                    // bytes used in data
    uint8_t data[PB_MOVE_BYTES];    // game-defined
};

// ---- the pluggable board game ----------------------------------------------
struct BoardGame {
    const char* name;
    uint8_t     gameId;             // stable id (matches the menu slot)
    uint8_t     minPlayers;
    uint8_t     maxPlayers;

    // lifecycle within a match
    void (*start)(uint32_t seed, int nPlayers, int mySeat);  // new match
    void (*tickLocal)();            // read buttons, build+submit a move on your turn
    void (*applyMove)(const PbMove* m);   // apply a move (yours echoed, or a peer's)
    void (*draw)();                 // render current state for THIS device
    bool (*isOver)(int* winnerSeat);// true + winner when the match ends
    void (*onEnd)(int winnerSeat);  // award XP / wager transfer / cleanup
};

// ---- framework API a game calls --------------------------------------------
int   pbPlayerCount();
int   pbMySeat();
int   pbWhoseTurn();               // seat index whose turn it is
bool  pbMyTurn();
bool  pbIsHost();
uint32_t pbSeed();
const char* pbPlayerName(int seat);

// Submit a move from the local player. The framework echoes it to applyMove
// locally AND broadcasts it to peers; it then advances the turn (unless the
// game called pbKeepTurn() during applyMove, e.g. for an extra roll).
void  pbSubmitMove(const PbMove* m);
// Submit an action that is NOT a turn: applied locally AND on every peer, with
// the turn rotation untouched. This is what simultaneous play needs - trivia
// buzzers, poker folds, anyone-can-act moments.
void  pbSubmitInfo(const PbMove* m);
void  pbAdvanceTurn();             // explicit turn pass (games that need it)
void  pbKeepTurn();                // call inside applyMove to NOT auto-advance
void  pbEndMatch(int winnerSeat);  // a game declares the match over

// REFEREE SUPPORT (games where the host device holds hidden state, e.g.
// Battleship). pbSendToSeat delivers a move to ONE player without broadcasting,
// so the referee can tell an owner "you were hit" without telling the table
// whose ship it was. Host-only; a no-op elsewhere or in pass-and-play.
void  pbSendToSeat(int seat, const PbMove* m);
// Send a move to the HOST device only (e.g. a secret fleet layout). On the host
// this is a no-op - the caller applies it directly since it IS the referee.
void  pbSendToHost(const PbMove* m);
// Broadcast a move WITHOUT applying it locally or advancing the turn - for a
// referee announcing an adjudicated result it has already applied itself.
void  pbBroadcastInfo(const PbMove* m);

// game-stack selection (used by the board-stack menu screen)
int   pbGameCount();
const char* pbGameName(int i);
void  pbSelectGame(int i);
bool  pbWantsExit();   // true once the user backs out; hub returns to the stack

// app entry points (driven by the party hub)
void partyBoardEnter();
void partyBoardTick();
void partyBoardDraw();
void partyBoardShutdown();

// ---- shared reusable modules (each game uses what it wants) -----------------
// GRID CURSOR: the two-button placement idiom. L = move cursor (scroll),
// long-L = rotate/secondary, R = select. A game configures the grid dims and
// reads the cursor + fired events.
namespace pbgrid {
    void  begin(int cols, int rows);
    void  tick();                  // consume this frame's input
    int   cx();                    // cursor column
    int   cy();                    // cursor row
    bool  moved();                 // cursor moved this frame
    bool  selected();              // R fired this frame
    bool  rotated();               // long-L fired this frame
    void  setCursor(int x, int y);
}

// KEYBOARD: on-screen A-Z + backspace + enter, L navigates, R selects.
namespace pbkbd {
    void  begin(int maxLen);
    void  tick();
    const char* text();            // current typed string
    bool  entered();               // enter fired this frame
    void  clear();
    // for the game's own draw: which key is highlighted, and the key rows
    int   cursorKey();
    const char* rowStr(int row);   // rows 0..3; '<' = backspace, '#' = enter
}

// DICE: shared-seed dice so all devices agree. roll() is deterministic from
// the match seed + a turn counter the framework maintains.
namespace pbdice {
    int   roll(int sides);         // 1..sides, deterministic per call-in-sequence
    void  reset();
}

// DECK: a shuffled 52-card deck from the match seed (Hold'em / card games).
namespace pbdeck {
    void  shuffle();               // reshuffle from seed
    int   draw();                  // 0..51, or -1 if empty; encode as rank*4+suit
    int   remaining();
}

// WAGER: two independent things.
//   * CHIPS are a SESSION-ONLY in-RAM currency - how Hold'em keeps score and
//     settles a hand. They are never persisted (no flash, not a quest item);
//     they reset each match. Default betting uses chips.
//   * REAL WAGERING (opt-in) stakes quest-inventory items: escrowItem removes
//     the bet from the local player, awardItem grants the pot to the winner,
//     transferring loser -> winner via qpUseItem / qpAddItem (the pet-trade
//     path). Only this mode touches the quest profile.
namespace pbwager {
    void  reset();                                 // clear chips + escrow for a new match
    bool  escrowItem(uint8_t itemId, uint16_t n);  // qpUseItem x n; false if short
    void  awardItem(uint8_t itemId, uint16_t n);   // qpAddItem to winner
    uint32_t chips();                              // session-only, in RAM
    void  addChips(int delta);
}
