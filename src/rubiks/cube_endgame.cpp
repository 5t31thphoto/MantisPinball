// MantisCubeSolver — symmetry canonicalisation and endgame tablebase.
//
// Portable C++. No Arduino, no heap, no PSRAM. Compiles for the ESP32-S3 and
// as a host library, so the tkinter tab exercises the same code the device
// runs rather than a lookalike.
#include "cube_endgame_api.h"
#include "cube_symmetry.h"
#include "cube_endgame.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Symmetry conjugation: S_k^-1 * X * S_k
// ---------------------------------------------------------------------------
// 24 conjugations per canonicalisation. On a scalar desktop CPU that cost is
// why published solvers just ship the unfolded megabyte. Here it is 24 passes
// of byte-wise permute-and-add over 20 bytes — the shape the PIE unit exists
// for, and the reason a 621,649-state tablebase fits in 33 KB.

void mcConjugate(const McCube& in, int k, McCube& out) {
    const uint8_t* scp = MC_S_CP[k];
    const uint8_t* sco = MC_S_CO[k];
    const uint8_t* sep = MC_S_EP[k];
    const uint8_t* seo = MC_S_EO[k];
    const uint8_t* icp = MC_SINV_CP[k];
    const uint8_t* ico = MC_SINV_CO[k];
    const uint8_t* iep = MC_SINV_EP[k];
    const uint8_t* ieo = MC_SINV_EO[k];

    for (int i = 0; i < 8; i++) {
        uint8_t src = in.c[scp[i]];
        uint8_t p = src & 7;
        uint8_t o = (uint8_t)((ico[p] + ((src >> 3) & 3) + sco[i]) % 3);
        out.c[i] = (uint8_t)(icp[p] | (o << 3));
    }
    for (int i = 0; i < 12; i++) {
        uint8_t src = in.e[sep[i]];
        uint8_t p = src & 15;
        uint8_t o = (uint8_t)(ieo[p] ^ ((src >> 4) & 1) ^ seo[i]);
        out.e[i] = (uint8_t)(iep[p] | (o << 4));
    }
}

static int mcCompare(const McCube& a, const McCube& b) {
    return memcmp(&a, &b, sizeof(McCube));
}

int mcCanonical(const McCube& s, McCube& rep) {
    McCube best, cand;
    mcConjugate(s, 0, best);
    int bestK = 0;
    for (int k = 1; k < MC_NSYM; k++) {
        mcConjugate(s, k, cand);
        if (mcCompare(cand, best) < 0) { best = cand; bestK = k; }
    }
    rep = best;
    return bestK;
}

// ---------------------------------------------------------------------------
// Minimal perfect hash
// ---------------------------------------------------------------------------
// No class keys are stored. An out-of-ball state lands on an arbitrary slot
// and yields a wrong move, which the self-verifying chain rejects. That is
// what makes the index 12.7 KB instead of 522 KB.

#define MC_MASK64 0xFFFFFFFFFFFFFFFFULL
#define MC_FNV_PRIME 0x100000001B3ULL
#define MC_FNV_OFFSET 0xCBF29CE484222325ULL

static uint64_t mcFnv(const uint8_t* p, int n, uint64_t seed) {
    uint64_t h = MC_FNV_OFFSET ^ seed;
    for (int i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= MC_FNV_PRIME;
    }
    return h;
}

static uint32_t mcHashSlot(const McCube& rep) {
    const uint8_t* raw = (const uint8_t*)&rep;
    uint64_t a = mcFnv(raw, 20, 0x9E3779B97F4A7C15ULL);
    uint64_t b = mcFnv(raw, 20, 0xC2B2AE3D27D4EB4FULL);
    uint32_t bucket = (uint32_t)(a % (uint64_t)MC_ENDGAME_BUCKETS);
    uint64_t h2 = b % (uint64_t)MC_ENDGAME_SLOTS;
    if (h2 == 0) h2 = 1;
    uint64_t d = (uint64_t)MC_ENDGAME_DISP[bucket];
    return (uint32_t)((a + d * h2) % (uint64_t)MC_ENDGAME_SLOTS);
}

static uint8_t mcPackedMove(uint32_t slot) {
    uint32_t bit = slot * 5u;
    uint32_t byte = bit >> 3;
    uint32_t off = bit & 7u;
    uint16_t win = (uint16_t)(MC_ENDGAME_MOVES[byte]
                 | ((uint16_t)MC_ENDGAME_MOVES[byte + 1] << 8));
    return (uint8_t)((win >> off) & 31u);
}

// ---------------------------------------------------------------------------
// Self-verifying endgame solve
// ---------------------------------------------------------------------------

int mcEndgameSolve(const McCube& start, uint8_t* moves, int maxMoves) {
    McCube cur = start;
    int n = 0;
    for (int step = 0; step <= MC_ENDGAME_DEPTH; step++) {
        if (mcIsSolved(cur)) return n;
        if (n >= maxMoves) return -1;

        McCube rep;
        int k = mcCanonical(cur, rep);
        uint8_t packed = mcPackedMove(mcHashSlot(rep));
        if (packed >= 18) return -1;

        // The stored move belongs to the representative; conjugate it back.
        uint8_t m = MC_SYM_MOVE[k][packed];
        moves[n++] = m;
        mcApplyMove(cur, m);
    }
    // Ran the full depth without reaching solved: the state was outside the
    // ball. Execution was the membership test.
    return mcIsSolved(cur) ? n : -1;
}

bool mcInEndgame(const McCube& s) {
    uint8_t tmp[MC_ENDGAME_MAX_MOVES];
    return mcEndgameSolve(s, tmp, MC_ENDGAME_MAX_MOVES) >= 0;
}


const uint8_t (*mcSymMove())[18] { return MC_SYM_MOVE; }
