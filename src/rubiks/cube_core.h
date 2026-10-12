// Mantis Crypto Pinball — cube solver core.
//
// Portable C++. No Arduino, no TFT_eSPI, no millis(), no heap, no PSRAM.
// This exact translation unit compiles for the ESP32-S3 under PlatformIO and
// as a host shared library for the tkinter tab, so the tab exercises the
// firmware solver rather than a lookalike.
//
// All working memory is a single static arena sized at compile time. If it
// links, it fits: there is no allocation that can fail at runtime and nothing
// that can fragment after hours of uptime.
#pragma once

#include <stdint.h>
#include <stddef.h>

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------
// 20 bytes: 8 corners then 12 edges, one byte each.
//   corner byte = permutation (bits 0-2) | orientation (bits 3-4)
//   edge byte   = permutation (bits 0-3) | orientation (bit 4)
// Byte-per-cubie rather than bit-packed because that is the layout the
// structure-of-arrays SIMD path needs; packing would force a shuffle the
// ESP32-S3 PIE unit cannot do.

struct McCube {
    uint8_t c[8];
    uint8_t e[12];
};

static inline uint8_t mcCornerPerm(const McCube& s, int i) { return s.c[i] & 7; }
static inline uint8_t mcCornerOri (const McCube& s, int i) { return (s.c[i] >> 3) & 3; }
static inline uint8_t mcEdgePerm  (const McCube& s, int i) { return s.e[i] & 15; }
static inline uint8_t mcEdgeOri   (const McCube& s, int i) { return (s.e[i] >> 4) & 1; }

void mcSolved(McCube& s);
bool mcIsSolved(const McCube& s);
void mcApplyMove(McCube& s, int move);

// --------------------------------------------------------------------------
// Facelet input
// --------------------------------------------------------------------------
// 54 colour values, URFDLB face order, row-major within each face — the same
// layout as g_cube[54] in rubiks_mode.cpp.

enum McValidation {
    MC_OK = 0,
    MC_ERR_COLOR_COUNT,      // a colour does not appear exactly 9 times
    MC_ERR_CENTERS,          // centres duplicated or misoriented
    MC_ERR_BAD_CORNER,       // impossible colour triple
    MC_ERR_BAD_EDGE,         // impossible colour pair
    MC_ERR_DUP_PIECE,        // same physical piece entered twice
    MC_ERR_CORNER_TWIST,     // one corner rotated in place
    MC_ERR_EDGE_FLIP,        // one edge flipped in place
    MC_ERR_PARITY,           // two pieces swapped
};

// Human-readable message, including which piece is at fault where known.
const char* mcValidationMessage(McValidation v);

// Index of the offending facelet, or -1. Lets the UI jump the cursor there.
int mcValidationFacelet();

McValidation mcFaceletsToCube(const uint8_t stickers[54], McCube& out);
void mcCubeToFacelets(const McCube& s, uint8_t stickers[54]);

// --------------------------------------------------------------------------
// Solver
// --------------------------------------------------------------------------

#define MC_MAX_SOLUTION 32

struct McSolution {
    uint8_t moves[MC_MAX_SOLUTION];
    uint8_t length;
    bool found;
};

// Progress is polled by the UI thread; the solver only ever writes it.
struct McProgress {
    volatile uint8_t phase;        // 1 or 2
    volatile uint8_t ply;          // current search depth
    volatile uint8_t bestLength;   // best complete solution so far, 0 if none
    volatile uint32_t statesSeen;
    volatile bool done;
};

// Cooperative yield, called every few thousand states. On the device this
// feeds the watchdog and lets the UI task render; on the host it is a no-op.
// Return false to abort the search early.
typedef bool (*McYieldFn)(McProgress& p, void* user);

// Solver options live in cube_policy.h; the search itself is no longer part
// of the core. This file is state, moves, validation, and the shared types.

// --------------------------------------------------------------------------
// Teaching mode (layer-by-layer)
// --------------------------------------------------------------------------
// Produces the same solve a human would perform, staged, so the app can show
// the real next move for this cube at each stage rather than a fixed string.

enum McStage {
    MC_STAGE_CROSS = 0,
    MC_STAGE_F2L_CORNERS,
    MC_STAGE_F2L_EDGES,
    MC_STAGE_OLL_CROSS,
    MC_STAGE_OLL_CORNERS,
    MC_STAGE_PLL_CORNERS,
    MC_STAGE_PLL_EDGES,
    MC_STAGE_COUNT
};

struct McLessonStep {
    uint8_t stage;
    uint8_t moves[MC_MAX_SOLUTION];
    uint8_t length;
    const char* algName;   // e.g. "sexy move", or nullptr for setup turns
};

const char* mcStageName(McStage s);

// Fills up to maxSteps lesson steps. Returns how many were produced.
int mcTeach(const McCube& start, McLessonStep* steps, int maxSteps);

// --------------------------------------------------------------------------
// Arena accounting, so the build can assert headroom.
// --------------------------------------------------------------------------
size_t mcArenaBytes();
