// MantisCubeSolver — endgame tablebase API.
#pragma once
#include <stdint.h>
#include "cube_core.h"

// Mirrors MC_ENDGAME_DEPTH in the generated header, declared here so callers
// do not have to pull the tablebase arrays into their translation unit.
#define MC_ENDGAME_MAX_MOVES 8
#define MC_NSYM_PUB 24

// Conjugated move table, exposed for multi-frame restarts.
extern const uint8_t (*mcSymMove())[18];

// Canonical class representative under the 24 rotations.
// Returns the symmetry index k with S_k^-1 * s * S_k == rep.
int  mcCanonical(const McCube& s, McCube& rep);
void mcConjugate(const McCube& in, int k, McCube& out);

// Self-verifying lookup. Returns move count, or -1 if the state lies outside
// the endgame ball. Never returns a wrong solution.
int  mcEndgameSolve(const McCube& start, uint8_t* moves, int maxMoves);
bool mcInEndgame(const McCube& s);
