// MantisCubeSolver — quantised policy inference and beam search.
//
// One forward pass on a parent ranks all 18 children. A value function would
// need 18 passes plus 18 move applications; this is the single largest win in
// the design and comes from the EfficientCube formulation. Moves are applied
// only to survivors, so memory traffic scales with beam width, not width x 18.
//
// Two multipliers sit on top, both paid for in arithmetic rather than memory
// because arithmetic is what this chip has in surplus:
//
//   * Symmetry dedup. Twenty-four rotations of one position would otherwise
//     occupy twenty-four slots of a 192-wide beam. Canonicalising before
//     insertion makes a narrow frontier behave like a much wider one.
//
//   * Multi-frame restarts. The same cube under a different axis assignment
//     yields a genuinely different solution. Sequential with early exit,
//     keeping the shortest.
#include <stdlib.h>
#include <string.h>
#include "cube_policy.h"
#include "cube_model.h"
#include "cube_endgame_api.h"

// First-layer lookup table (tools/mantis_cube/hosttests/gen_l1table.cpp, run by
// CI before every build). Used only when its signature matches the weights
// actually compiled in, so a stale table can never change the answer. Build
// with -DMC_NO_L1TABLE to leave it out (CI does this if flash runs short).
#if !defined(MC_NO_L1TABLE) && defined(__has_include)
#  if __has_include("cube_l1table.h")
#    include "cube_l1table.h"
#    define MC_HAVE_L1T 1
#  endif
#endif
static bool g_l1ok = false;
#ifdef MC_HAVE_L1T
static uint32_t mcWeightSig() {
    uint32_t h = 2166136261u;
    const uint32_t nE = MC_N_SLOTS * MC_N_VALUES * MC_EMBED_DIM;
    const uint32_t nW = (uint32_t)MC_HIDDEN_OUT[0] * MC_FEATURE_DIM;
    for (uint32_t i = 0; i < (nE * MC_EMBED_BITS + 7) / 8; i++) { h ^= MC_W_EMBED[i]; h *= 16777619u; }
    for (uint32_t i = 0; i < (nW * MC_HIDDEN_BITS + 7) / 8; i++) { h ^= MC_W_HIDDEN[0][i]; h *= 16777619u; }
    return h;
}
#endif
bool mcPolicyFastTableActive() { return g_l1ok; }

#include <string.h>

// Beam width sets the arena size, and the arena comes out of a heap that has
// ~38.8 KB free with the 108 KB framebuffer already up (see mem_report.json).
// The solver also takes an 8 KB FreeRTOS task stack while it runs, so:
//     W=192 -> 24.6 KB arena + 8 KB stack = 32.6 KB  (84% of free heap)
//     W=128 -> 19.1 KB arena + 8 KB stack = 27.1 KB  (70%, ~11 KB headroom)
// 128 ships as the default because a solve that fails to allocate is worse
// than a slightly narrower beam. Override with -DMC_BEAM_W=192 if a device
// build measures more headroom.
#ifndef MC_BEAM_W
#define MC_BEAM_W 128
#endif
#define MC_PATH_BYTES ((MC_MAX_SOLUTION * 5 + 7) / 8)
#define MC_DEDUP_SLOTS 1024

namespace {

struct Beam {
    McCube  state[MC_BEAM_W];
    uint8_t path[MC_BEAM_W][MC_PATH_BYTES];
    int32_t score[MC_BEAM_W];
    uint16_t count;
};

// The search arena is ~24 KB. On this device that MUST NOT live in static BSS:
// mem_report.json is explicit that big buffers are malloc'd on Enter and freed
// on Shutdown, never left in BSS, because BSS is permanent and would cut the
// free heap for all 25 other apps for the whole runtime. So the arena is one
// heap block, taken when the app opens and given back when it closes. The
// macros below keep every use site unchanged.
struct PolicyArena {
    Beam     a, b;
    uint32_t seen[MC_DEDUP_SLOTS];
    int32_t  act[MC_FEATURE_DIM > 512 ? MC_FEATURE_DIM : 512];
    int32_t  tmp[512];
    int32_t  logits[MC_N_MOVES];
};
PolicyArena* g_ar = nullptr;

#define g_a      (g_ar->a)
#define g_b      (g_ar->b)
#define g_seen   (g_ar->seen)
#define g_act    (g_ar->act)
#define g_tmp    (g_ar->tmp)
#define g_logits (g_ar->logits)

}  // namespace

size_t mcPolicyArenaBytes() {
    return sizeof(PolicyArena);
}

bool mcPolicyArenaAcquire() {
    if (g_ar) return true;
    g_ar = (PolicyArena*)malloc(sizeof(PolicyArena));
    if (!g_ar) return false;
    memset(g_ar, 0, sizeof(PolicyArena));
#ifdef MC_HAVE_L1T
    g_l1ok = (MC_L1T_HIDDEN == MC_HIDDEN_OUT[0]) && (mcWeightSig() == MC_L1T_SIG);
#endif
    return true;
}

void mcPolicyArenaRelease() {
    if (g_ar) { free(g_ar); g_ar = nullptr; }
}

bool mcPolicyArenaReady() { return g_ar != nullptr; }

static inline int32_t mcUnpack(const uint8_t* base, uint32_t idx, int bits) {
    uint32_t bit = idx * (uint32_t)bits;
    uint32_t byte = bit >> 3;
    uint32_t off = bit & 7u;
    uint32_t win = (uint32_t)base[byte]
                 | ((uint32_t)base[byte + 1] << 8)
                 | ((uint32_t)base[byte + 2] << 16);
    int32_t v = (int32_t)((win >> off) & ((1u << bits) - 1u));
    return v - (1 << (bits - 1));
}

static inline int mcSlotValue(const McCube& s, int slot) {
    if (slot < 8) return (s.c[slot] & 7) * 3 + ((s.c[slot] >> 3) & 3);
    int i = slot - 8;
    return (s.e[i] & 15) * 2 + ((s.e[i] >> 4) & 1);
}

// Integer per-channel requantisation.
//
// acc[o] carries scale s_x * s_w[o]. Multiplying by the stored relative
// channel multiplier q[o] and shifting by 15 puts every output on the common
// scale s_x * max(s_w), which is uniform across the layer and therefore
// cancels under ReLU. No floats, no calibration, and nothing that can drift
// between the host reference and the device.
//
// After ReLU the layer is renormalised by a dynamic right shift so the next
// matmul cannot overflow int32. The shift is uniform across channels, so it
// scales every logit equally and leaves the ranking - the only thing beam
// search consumes - untouched.

static int mcRenormShift(const int32_t* v, int n) {
    int32_t mx = 0;
    for (int i = 0; i < n; i++) if (v[i] > mx) mx = v[i];
    int sh = 0;
    while ((mx >> sh) > 32767) sh++;
    return sh;
}

// Forward pass. 83% of solve time was here, almost all of it in mcUnpack()
// running once per weight. The weights are packed LSB-first and every row
// starts byte-aligned (row lengths are even), so:
//   * 4-bit rows are read a byte (two weights) at a time
//   * 8-bit rows are read a byte at a time
//   * the signed offset is pulled out of the loop:
//       sum a*(w - off) == sum a*w - off * sum a
// Pure integer arithmetic with no overflow (worst intermediate ~1.6e8), so
// the logits are IDENTICAL to the original, not approximately equal.
// mcPolicyGeneric() is the original loop, kept as the reference/fallback.
static void mcPolicyGeneric(const McCube& s, int32_t* logits);



static inline int32_t mcDotNibbles(const uint8_t* row, const int32_t* a, int n, int32_t sumA) {
    int32_t acc = 0;
    for (int i = 0; i < n; i += 2) {
        uint8_t b = row[i >> 1];
        acc += a[i] * (int32_t)(b & 15u) + a[i + 1] * (int32_t)(b >> 4);
    }
    return acc - 8 * sumA;
}
static inline int32_t mcDotBytes(const uint8_t* row, const int32_t* a, int n, int32_t sumA) {
    int32_t acc = 0;
    for (int i = 0; i < n; i++) acc += a[i] * (int32_t)row[i];
    return acc - 128 * sumA;
}

void mcPolicy(const McCube& s, int32_t* logits) {
#if MC_HIDDEN_BITS != 4 || MC_OUTPUT_BITS != 8 || MC_EMBED_BITS != 8 || (MC_FEATURE_DIM % 2)
    mcPolicyGeneric(s, logits);
    return;
#else
    int in_dim = MC_FEATURE_DIM;
    int first = 0;
#ifdef MC_HAVE_L1T
    if (g_l1ok) {
        // Layer 1 from the table: 20 rows of partial sums instead of 81,920
        // multiply-adds. Exactly the same integer accumulator as below.
        const int out_dim = MC_HIDDEN_OUT[0];
        for (int o = 0; o < out_dim; o++) g_tmp[o] = 0;
        for (int slot = 0; slot < MC_N_SLOTS; slot++) {
            const int16_t* row = MC_L1T + (size_t)(slot * MC_N_VALUES + mcSlotValue(s, slot)) * out_dim;
            for (int o = 0; o < out_dim; o++) g_tmp[o] += row[o];
        }
        const uint16_t* qm = MC_Q_HIDDEN[0];
        for (int o = 0; o < out_dim; o++) {
            int32_t y = (int32_t)(((int64_t)g_tmp[o] * (int64_t)qm[o]) >> 15);
            g_tmp[o] = y > 0 ? y : 0;                     // ReLU
        }
        int sh = mcRenormShift(g_tmp, out_dim);
        if (sh) for (int o = 0; o < out_dim; o++) g_tmp[o] >>= sh;
        memcpy(g_act, g_tmp, sizeof(int32_t) * (size_t)out_dim);
        in_dim = out_dim;
        first = 1;
    } else
#endif
    {
        for (int slot = 0; slot < MC_N_SLOTS; slot++) {
            int v = mcSlotValue(s, slot);
            const uint8_t* e = MC_W_EMBED + (size_t)(slot * MC_N_VALUES + v) * MC_EMBED_DIM;
            for (int d = 0; d < MC_EMBED_DIM; d++)
                g_act[slot * MC_EMBED_DIM + d] = (int32_t)e[d] - 128;
        }
    }
    for (int layer = first; layer < MC_N_HIDDEN; layer++) {
        const uint8_t* w = MC_W_HIDDEN[layer];
        const uint16_t* qm = MC_Q_HIDDEN[layer];
        int out_dim = MC_HIDDEN_OUT[layer];
        if (in_dim & 1) { mcPolicyGeneric(s, logits); return; }   // never with this model
        int32_t sumA = 0;
        for (int i = 0; i < in_dim; i++) sumA += g_act[i];
        const int rowBytes = in_dim >> 1;
        for (int o = 0; o < out_dim; o++) {
            int32_t acc = mcDotNibbles(w + (size_t)o * rowBytes, g_act, in_dim, sumA);
            int64_t scaled = (int64_t)acc * (int64_t)qm[o];
            int32_t y = (int32_t)(scaled >> 15);
            g_tmp[o] = y > 0 ? y : 0;                     // ReLU
        }
        int sh = mcRenormShift(g_tmp, out_dim);
        if (sh) for (int o = 0; o < out_dim; o++) g_tmp[o] >>= sh;
        memcpy(g_act, g_tmp, sizeof(int32_t) * (size_t)out_dim);
        in_dim = out_dim;
    }
    int32_t sumA = 0;
    for (int i = 0; i < in_dim; i++) sumA += g_act[i];
    for (int o = 0; o < MC_N_MOVES; o++) {
        int32_t acc = mcDotBytes(MC_W_OUT + (size_t)o * in_dim, g_act, in_dim, sumA);
        logits[o] = (int32_t)(((int64_t)acc * (int64_t)MC_Q_OUT[o]) >> 15);
    }
#endif
}

static void mcPolicyGeneric(const McCube& s, int32_t* logits) {
    for (int slot = 0; slot < MC_N_SLOTS; slot++) {
        int v = mcSlotValue(s, slot);
        uint32_t base = (uint32_t)((slot * MC_N_VALUES + v) * MC_EMBED_DIM);
        for (int d = 0; d < MC_EMBED_DIM; d++)
            g_act[slot * MC_EMBED_DIM + d] =
                mcUnpack(MC_W_EMBED, base + (uint32_t)d, MC_EMBED_BITS);
    }

    int in_dim = MC_FEATURE_DIM;
    for (int layer = 0; layer < MC_N_HIDDEN; layer++) {
        const uint8_t* w = MC_W_HIDDEN[layer];
        const uint16_t* qm = MC_Q_HIDDEN[layer];
        int out_dim = MC_HIDDEN_OUT[layer];
        for (int o = 0; o < out_dim; o++) {
            int32_t acc = 0;
            uint32_t base = (uint32_t)o * (uint32_t)in_dim;
            for (int i = 0; i < in_dim; i++)
                acc += g_act[i] * mcUnpack(w, base + (uint32_t)i,
                                           MC_HIDDEN_BITS);
            int64_t scaled = (int64_t)acc * (int64_t)qm[o];
            int32_t y = (int32_t)(scaled >> 15);
            g_tmp[o] = y > 0 ? y : 0;                     // ReLU
        }
        int sh = mcRenormShift(g_tmp, out_dim);
        if (sh) for (int o = 0; o < out_dim; o++) g_tmp[o] >>= sh;
        memcpy(g_act, g_tmp, sizeof(int32_t) * (size_t)out_dim);
        in_dim = out_dim;
    }

    for (int o = 0; o < MC_N_MOVES; o++) {
        int32_t acc = 0;
        uint32_t base = (uint32_t)o * (uint32_t)in_dim;
        for (int i = 0; i < in_dim; i++)
            acc += g_act[i] * mcUnpack(MC_W_OUT, base + (uint32_t)i,
                                       MC_OUTPUT_BITS);
        logits[o] = (int32_t)(((int64_t)acc * (int64_t)MC_Q_OUT[o]) >> 15);
    }
}

static inline void mcPathSet(uint8_t* path, int idx, uint8_t move) {
    uint32_t bit = (uint32_t)idx * 5u;
    uint32_t byte = bit >> 3, off = bit & 7u;
    uint16_t win = (uint16_t)(path[byte] | ((uint16_t)path[byte + 1] << 8));
    win = (uint16_t)(win & ~(uint16_t)(31u << off));
    win = (uint16_t)(win | (uint16_t)((move & 31u) << off));
    path[byte] = (uint8_t)(win & 0xFF);
    path[byte + 1] = (uint8_t)(win >> 8);
}

static inline uint8_t mcPathGet(const uint8_t* path, int idx) {
    uint32_t bit = (uint32_t)idx * 5u;
    uint32_t byte = bit >> 3, off = bit & 7u;
    uint16_t win = (uint16_t)(path[byte] | ((uint16_t)path[byte + 1] << 8));
    return (uint8_t)((win >> off) & 31u);
}

static uint32_t mcFingerprint(const McCube& s) {
    McCube rep;
    mcCanonical(s, rep);
    const uint8_t* p = (const uint8_t*)&rep;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 20; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static bool mcSeenInsert(uint32_t fp) {
    uint32_t i = fp & (MC_DEDUP_SLOTS - 1);
    for (int probe = 0; probe < 16; probe++) {
        uint32_t v = g_seen[i];
        if (v == 0) { g_seen[i] = fp; return false; }
        if (v == fp) return true;
        i = (i + 1) & (MC_DEDUP_SLOTS - 1);
    }
    return false;
}

static bool mcRedundant(int prev, int m) {
    if (prev < 0) return false;
    if (prev / 3 == m / 3) return true;
    int fa = prev / 3, fb = m / 3;
    if ((fa + 3) % 6 == fb && fa > fb) return true;
    return false;
}

static void mcInsert(Beam& dst, uint16_t width, const McCube& st,
                     const uint8_t* path, int plen, uint8_t move,
                     int32_t score) {
    uint16_t pos = dst.count;
    if (pos >= width) {
        uint16_t worst = 0;
        for (uint16_t i = 1; i < dst.count; i++)
            if (dst.score[i] < dst.score[worst]) worst = i;
        if (dst.score[worst] >= score) return;
        pos = worst;
    } else {
        dst.count++;
    }
    dst.state[pos] = st;
    memcpy(dst.path[pos], path, MC_PATH_BYTES);
    mcPathSet(dst.path[pos], plen, move);
    dst.score[pos] = score;
}

// Would a candidate with this score enter the beam? Checked BEFORE the
// symmetry fingerprint, which costs 24 conjugations. Most children of a full
// beam are rejected, so they now skip that work entirely - and a rejected
// child is no longer marked "seen", which used to block a better-scoring
// symmetric copy of the same position arriving later from another parent.
static bool mcWouldEnter(const Beam& dst, uint16_t width, int32_t score) {
    if (dst.count < width) return true;
    int32_t worst = dst.score[0];
    for (uint16_t i = 1; i < dst.count; i++) if (dst.score[i] < worst) worst = dst.score[i];
    return score > worst;
}

static bool mcBeamOnce(const McCube& start, McSolution& out,
                       McProgress& progress, const McPolicyOptions& opts,
                       uint16_t width, uint8_t bestSoFar) {
    uint8_t mv[MC_ENDGAME_MAX_MOVES];
    int n = mcEndgameSolve(start, mv, MC_ENDGAME_MAX_MOVES);
    if (n >= 0) {
        memcpy(out.moves, mv, (size_t)n);
        out.length = (uint8_t)n;
        out.found = true;
        return true;
    }

    Beam* cur = &g_a;
    Beam* nxt = &g_b;
    cur->count = 1;
    cur->state[0] = start;
    memset(cur->path[0], 0, MC_PATH_BYTES);
    cur->score[0] = 0;

    // The path buffer holds MC_MAX_SOLUTION moves; never search deeper.
    const int maxDepth = opts.maxDepth < MC_MAX_SOLUTION ? opts.maxDepth : MC_MAX_SOLUTION - 1;
    for (int ply = 0; ply < maxDepth; ply++) {
        if (bestSoFar && ply + 1 >= bestSoFar) break;
        progress.ply = (uint8_t)ply;
        nxt->count = 0;
        if (opts.symmetryDedup) memset(g_seen, 0, sizeof(g_seen));

        for (uint16_t i = 0; i < cur->count; i++) {
            mcPolicy(cur->state[i], g_logits);
            int prev = ply > 0 ? (int)mcPathGet(cur->path[i], ply - 1) : -1;

            for (int m = 0; m < MC_N_MOVES; m++) {
                if (mcRedundant(prev, m)) continue;
                McCube child = cur->state[i];
                mcApplyMove(child, m);

                int k = mcEndgameSolve(child, mv, MC_ENDGAME_MAX_MOVES);
                if (k >= 0 && ply + 1 + k <= MC_MAX_SOLUTION
                    && (!bestSoFar || ply + 1 + k < bestSoFar)) {
                    for (int q = 0; q < ply; q++)
                        out.moves[q] = mcPathGet(cur->path[i], q);
                    out.moves[ply] = (uint8_t)m;
                    memcpy(out.moves + ply + 1, mv, (size_t)k);
                    out.length = (uint8_t)(ply + 1 + k);
                    out.found = true;
                    return true;
                }

                int32_t sc = cur->score[i] + g_logits[m];
                if (!mcWouldEnter(*nxt, width, sc)) continue;
                if (opts.symmetryDedup && mcSeenInsert(mcFingerprint(child)))
                    continue;

                mcInsert(*nxt, width, child, cur->path[i], ply, (uint8_t)m, sc);
            }
            progress.statesSeen += MC_N_MOVES;
            if (opts.yieldFn && (i & 15) == 0)
                if (!opts.yieldFn(progress, opts.yieldUser)) return false;
        }

        if (nxt->count == 0) break;
        Beam* t = cur; cur = nxt; nxt = t;
    }
    return false;
}

bool mcPolicySolve(const McCube& start, McSolution& out,
                   McProgress& progress, const McPolicyOptions& opts) {
    if (!g_ar) return false;      // arena not acquired: fail clean, never crash
    out.found = false;
    out.length = 0;
    progress.done = false;
    progress.statesSeen = 0;
    progress.bestLength = 0;

    uint16_t width = opts.beamWidth ? opts.beamWidth : MC_BEAM_W;
    if (width > MC_BEAM_W) width = MC_BEAM_W;

    int frames = opts.frames < 1 ? 1 : opts.frames;
    if (frames > MC_NSYM_PUB) frames = MC_NSYM_PUB;

    const uint8_t (*symMove)[18] = mcSymMove();

    McSolution best;
    best.found = false;
    best.length = 0;
    uint8_t bestLen = 0;

    for (int f = 0; f < frames; f++) {
        progress.phase = (uint8_t)(f + 1);

        McCube framed;
        if (f == 0) framed = start;
        else mcConjugate(start, f, framed);

        McSolution attempt;
        attempt.found = false;
        attempt.length = 0;
        if (!mcBeamOnce(framed, attempt, progress, opts, width, bestLen))
            continue;

        // A move m inside frame f is S_f * m * S_f^-1 outside it.
        McSolution mapped;
        mapped.length = attempt.length;
        mapped.found = true;
        for (int i = 0; i < attempt.length; i++)
            mapped.moves[i] = (f == 0) ? attempt.moves[i]
                                       : symMove[f][attempt.moves[i]];

        // Verify in the original frame before accepting.
        McCube check = start;
        for (int i = 0; i < mapped.length; i++)
            mcApplyMove(check, mapped.moves[i]);
        if (!mcIsSolved(check)) continue;

        if (!best.found || mapped.length < best.length) {
            best = mapped;
            bestLen = mapped.length;
            progress.bestLength = bestLen;
        }
        if (bestLen <= opts.goodEnough) break;
    }

    progress.done = true;
    if (best.found) out = best;
    return best.found;
}
