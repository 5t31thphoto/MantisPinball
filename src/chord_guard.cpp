// FILE: src/chord_guard.cpp
#include <Arduino.h>
#include "chord_guard.h"

extern bool btnL, btnR, btnL_prev, btnR_prev;

// Same scheme as the aquarium's aqChord* logic, verbatim in behaviour.
static bool     cgChordActive   = false;   // both buttons currently down
static bool     cgLPoisoned     = false;   // current LEFT press part of a chord
static bool     cgRPoisoned     = false;   // current RIGHT press part of a chord
static uint32_t cgChordCoolUntil = 0;      // presses starting before this are poisoned

void chordGuardReset(){
    cgChordActive = false;
    cgLPoisoned = cgRPoisoned = false;
    cgChordCoolUntil = 0;
}

void chordGuardTick(){
    uint32_t now = millis();

    // both down together -> chord; poison whatever presses are in flight
    if (btnL && btnR) {
        cgChordActive = true;
        cgLPoisoned = cgRPoisoned = true;
    }
    // chord fully released -> start the cooldown window
    if (cgChordActive && !btnL && !btnR) {
        cgChordActive = false;
        cgChordCoolUntil = now + 350;
    }
    // presses that begin during the cooldown are part of the gesture too
    // (this is what catches the second click of the double-click)
    if (now < cgChordCoolUntil) {
        if (btnL) cgLPoisoned = true;
        if (btnR) cgRPoisoned = true;
    }
    // Un-poison only once a button has been fully up for at least one tick
    // past the cooldown. Checking _prev keeps the release tick itself
    // suppressed: the mode's button handlers run after this and must still see
    // the poison on the frame the button comes up.
    if (!cgChordActive && now >= cgChordCoolUntil) {
        if (!btnL && !btnL_prev) cgLPoisoned = false;
        if (!btnR && !btnR_prev) cgRPoisoned = false;
    }
}

bool chordLEdge(){ return btnL && !btnL_prev && !cgLPoisoned; }
bool chordREdge(){ return btnR && !btnR_prev && !cgRPoisoned; }
bool chordLHeld(){ return btnL && !cgLPoisoned; }
bool chordRHeld(){ return btnR && !cgRPoisoned; }
