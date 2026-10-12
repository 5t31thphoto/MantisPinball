// FILE: src/chord_guard.h
// Both-button exit-gesture guard, shared by the app modes that need it.
//
// The launcher's exit gesture is a double-click of BOTH buttons, detected in
// main.cpp from the raw btnL/btnR globals. Certain non-pinball modes must not
// treat those presses as real input (a feed, a page turn, a refresh) while the
// user is performing the chord on the way to the menu. This is the aquarium's
// proven poison scheme lifted into one place so every such mode behaves
// identically.
//
// IMPORTANT: this is ONLY for modes where both-buttons-at-once is never a valid
// simultaneous input. The pinball / flipper games rely on pressing L and R
// together as normal play and must NOT use this.
//
// Usage in a mode:
//   void fooTick() {
//       chordGuardTick();                 // once, before reading buttons
//       if (chordLEdge()) { ... }         // instead of (btnL && !btnL_prev)
//       if (chordREdge()) { ... }         // instead of (btnR && !btnR_prev)
//   }
//   void fooEnter() { chordGuardReset(); } // clears stale poison on entry
//
// The raw globals are left untouched so main.cpp still sees the gesture; only
// the mode's *interpretation* of the presses is suppressed. Accepted cost: one
// genuine tap within ~350ms of a chord is swallowed - fine for these modes.
#pragma once

// Call once per tick, before reading any button edges.
void chordGuardTick();

// Clear all poison/cooldown state. Call from the mode's Enter.
void chordGuardReset();

// Poison-aware button edges. Use these in place of the raw
// (btnL && !btnL_prev) / (btnR && !btnR_prev) tests. A press that is part of
// the both-button exit gesture returns false here.
bool chordLEdge();
bool chordREdge();

// Poison-aware "button currently down" (for hold gestures). False while the
// press is part of the exit chord.
bool chordLHeld();
bool chordRHeld();
