// MantisCubeSolver — policy beam search API.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cube_core.h"

struct McPolicyOptions {
    uint16_t beamWidth;    // 0 = compile-time default
    uint8_t  maxDepth;
    uint8_t  frames;       // multi-frame restarts, 1..24
    uint8_t  goodEnough;   // stop early once a solution this short is found
    bool     symmetryDedup;
    McYieldFn yieldFn;
    void* yieldUser;
};

inline void mcDefaultPolicyOptions(McPolicyOptions& o) {
    o.beamWidth = 0;
    // Tuned on the trained weights (24 random 25-move scrambles, host bench):
    //   depth 24, keep searching for <=20 moves : 11/24 solved
    //   depth 30, take the first solution       : 17/24 solved, faster
    // On a microcontroller the first valid solution beats a shorter one found
    // after burning every frame. 30 is the deepest the 32-move path holds.
    o.maxDepth = 30;
    o.frames = 8;
    o.goodEnough = 32;
    o.symmetryDedup = true;
    o.yieldFn = nullptr;
    o.yieldUser = nullptr;
}

void mcPolicy(const McCube& s, int32_t* logits);
bool mcPolicySolve(const McCube& start, McSolution& out,
                   McProgress& progress, const McPolicyOptions& opts);
size_t mcPolicyArenaBytes();
// The arena is heap, not BSS. Acquire on app Enter, release on Shutdown.
bool   mcPolicyArenaAcquire();
void   mcPolicyArenaRelease();
bool   mcPolicyArenaReady();
// True when the first-layer lookup table matches the compiled weights.
bool   mcPolicyFastTableActive();
