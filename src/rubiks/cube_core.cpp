#include "cube_core.h"
#include "cube_tables.h"

#include <string.h>
#ifdef MC_TRACE
#include <cstdio>
#endif

// --------------------------------------------------------------------------
// Basic state operations
// --------------------------------------------------------------------------

uint16_t mcDbgTwist(const McCube&);
uint16_t mcDbgFlip(const McCube&);
uint16_t mcDbgSlice(const McCube&);
void mcSolved(McCube& s) {
    for (int i = 0; i < 8; i++) s.c[i] = (uint8_t)i;
    for (int i = 0; i < 12; i++) s.e[i] = (uint8_t)i;
}

bool mcIsSolved(const McCube& s) {
    for (int i = 0; i < 8; i++) if (s.c[i] != i) return false;
    for (int i = 0; i < 12; i++) if (s.e[i] != i) return false;
    return true;
}

// new = state * move, using the same composition order as the host model:
//   cp'[i] = cp[m.cp[i]],  co'[i] = (co[m.cp[i]] + m.co[i]) % 3
void mcApplyMove(McCube& s, int move) {
    McCube n;
    const uint8_t* mcp = MC_MOVE_CP[move];
    const uint8_t* mco = MC_MOVE_CO[move];
    const uint8_t* mep = MC_MOVE_EP[move];
    const uint8_t* meo = MC_MOVE_EO[move];

    for (int i = 0; i < 8; i++) {
        uint8_t src = s.c[mcp[i]];
        uint8_t o = ((src >> 3) & 3) + mco[i];
        if (o >= 3) o -= 3;
        n.c[i] = (uint8_t)((src & 7) | (o << 3));
    }
    for (int i = 0; i < 12; i++) {
        uint8_t src = s.e[mep[i]];
        uint8_t o = (uint8_t)(((src >> 4) & 1) ^ meo[i]);
        n.e[i] = (uint8_t)((src & 15) | (o << 4));
    }
    s = n;
}

// --------------------------------------------------------------------------
// Coordinates — must match mantis_cube/model.py exactly
// --------------------------------------------------------------------------

static const uint16_t MC_BINOM[13][5] = {
    {1,0,0,0,0},{1,1,0,0,0},{1,2,1,0,0},{1,3,3,1,0},{1,4,6,4,1},
    {1,5,10,10,5},{1,6,15,20,15},{1,7,21,35,35},{1,8,28,56,70},
    {1,9,36,84,126},{1,10,45,120,210},{1,11,55,165,330},{1,12,66,220,495}
};

uint16_t mcDbgTwist(const McCube&s);
static uint16_t mcTwist(const McCube& s) {
    uint16_t t = 0;
    for (int i = 0; i < 7; i++) t = (uint16_t)(t * 3 + mcCornerOri(s, i));
    return t;
}

static uint16_t mcFlip(const McCube& s) {
    uint16_t f = 0;
    for (int i = 0; i < 11; i++) f = (uint16_t)(f * 2 + mcEdgeOri(s, i));
    return f;
}

static uint16_t mcUdSlice(const McCube& s) {
    int chosen[4], k = 0;
    for (int i = 0; i < 12 && k < 4; i++)
        if (mcEdgePerm(s, i) >= 8) chosen[k++] = i;
    if (k != 4) return 0xFFFF;
    int rank = 0, prev = -1;
    for (int j = 0; j < 4; j++) {
        for (int p = prev + 1; p < chosen[j]; p++)
            rank += MC_BINOM[11 - p][3 - j];
        prev = chosen[j];
    }
    return (uint16_t)(494 - rank);
}

static uint32_t mcPermRank(const uint8_t* p, int n) {
    uint32_t idx = 0;
    for (int i = 0; i < n; i++) {
        idx *= (uint32_t)(n - i);
        for (int j = i + 1; j < n; j++)
            if (p[j] < p[i]) idx++;
    }
    return idx;
}

static uint32_t mcCornerPermCoord(const McCube& s) {
    uint8_t p[8];
    for (int i = 0; i < 8; i++) p[i] = mcCornerPerm(s, i);
    return mcPermRank(p, 8);
}

static uint32_t mcEdge8PermCoord(const McCube& s) {
    uint8_t p[8];
    for (int i = 0; i < 8; i++) {
        uint8_t v = mcEdgePerm(s, i);
        if (v >= 8) return 0xFFFFFFFF;   // not in phase 2
        p[i] = v;
    }
    return mcPermRank(p, 8);
}

static uint32_t mcSlicePermCoord(const McCube& s) {
    uint8_t p[4];
    for (int i = 0; i < 4; i++) {
        uint8_t v = mcEdgePerm(s, 8 + i);
        if (v < 8) return 0xFFFFFFFF;
        p[i] = (uint8_t)(v - 8);
    }
    return mcPermRank(p, 4);
}

// Heuristics: max over independent coordinate tables. Not admissible, which
// is precisely what lets these tables be 83 KB instead of two megabytes —
// beam search does not require admissibility the way IDA* does.

static uint8_t mcH1(const McCube& s) {
    uint8_t a = MC_DIST_TWIST[mcTwist(s)];
    uint8_t b = MC_DIST_FLIP[mcFlip(s)];
    uint8_t c = MC_DIST_UDSLICE[mcUdSlice(s)];
    // Sum, not max. A max is the admissible choice an IDA* solver is forced
    // into; beam search has no such requirement, and the sum gives a far
    // finer gradient so the frontier does not plateau with a thousand states
    // sharing one score and no way to rank them.
    return (uint8_t)(a + b + c);
}

static uint8_t mcH2(const McCube& s) {
    uint8_t a = MC_DIST_CORNERPERM[mcCornerPermCoord(s)];
    uint8_t b = MC_DIST_EDGE8PERM[mcEdge8PermCoord(s)];
    uint8_t c = MC_DIST_SLICEPERM[mcSlicePermCoord(s)];
    return (uint8_t)(a + b + c);
}

// --------------------------------------------------------------------------
// Facelet conversion and validation
// --------------------------------------------------------------------------

static int g_badFacelet = -1;

int mcValidationFacelet() { return g_badFacelet; }

const char* mcValidationMessage(McValidation v) {
    switch (v) {
        case MC_OK: return "Cube looks good";
        case MC_ERR_COLOR_COUNT:
            return "A colour is used the wrong number of times - each colour "
                   "must appear exactly 9 times";
        case MC_ERR_CENTERS:
            return "Two centre squares are the same colour - centres never "
                   "move, so check you turned the cube the right way";
        case MC_ERR_BAD_CORNER:
            return "That corner has three colours that never meet on a real "
                   "cube - check the highlighted square";
        case MC_ERR_BAD_EDGE:
            return "That edge has two colours that are on opposite sides - "
                   "check the highlighted square";
        case MC_ERR_DUP_PIECE:
            return "The same piece was entered twice - one of the two is "
                   "mis-coloured";
        case MC_ERR_CORNER_TWIST:
            return "One corner is twisted in place. If the cube was taken "
                   "apart, twist one corner and re-enter it";
        case MC_ERR_EDGE_FLIP:
            return "One edge is flipped in place. If the cube was taken "
                   "apart, flip one edge and re-enter it";
        case MC_ERR_PARITY:
            return "Two pieces are swapped. Re-check the faces - this is "
                   "usually one mistyped square";
    }
    return "Unknown problem";
}

McValidation mcFaceletsToCube(const uint8_t stickers[54], McCube& out) {
    g_badFacelet = -1;

    int counts[6] = {0,0,0,0,0,0};
    for (int i = 0; i < 54; i++) {
        if (stickers[i] > 5) { g_badFacelet = i; return MC_ERR_COLOR_COUNT; }
        counts[stickers[i]]++;
    }
    for (int c = 0; c < 6; c++)
        if (counts[c] != 9) return MC_ERR_COLOR_COUNT;

    for (int f = 0; f < 6; f++)
        if (stickers[f * 9 + 4] != f) { g_badFacelet = f * 9 + 4;
                                        return MC_ERR_CENTERS; }

    bool usedC[8] = {false};
    for (int slot = 0; slot < 8; slot++) {
        uint8_t got[3];
        for (int k = 0; k < 3; k++) got[k] = stickers[MC_CORNER_FACELET[slot][k]];

        int piece = -1, twist = 0;
        for (int p = 0; p < 8 && piece < 0; p++) {
            for (int r = 0; r < 3; r++) {
                if (got[0] == MC_CORNER_COLOR[p][r] &&
                    got[1] == MC_CORNER_COLOR[p][(r + 1) % 3] &&
                    got[2] == MC_CORNER_COLOR[p][(r + 2) % 3]) {
                    piece = p; twist = (3 - r) % 3; break;
                }
            }
        }
        if (piece < 0) { g_badFacelet = MC_CORNER_FACELET[slot][0];
                         return MC_ERR_BAD_CORNER; }
        if (usedC[piece]) { g_badFacelet = MC_CORNER_FACELET[slot][0];
                            return MC_ERR_DUP_PIECE; }
        usedC[piece] = true;
        out.c[slot] = (uint8_t)(piece | (twist << 3));
    }

    bool usedE[12] = {false};
    for (int slot = 0; slot < 12; slot++) {
        uint8_t a = stickers[MC_EDGE_FACELET[slot][0]];
        uint8_t b = stickers[MC_EDGE_FACELET[slot][1]];

        int piece = -1, flip = 0;
        for (int p = 0; p < 12 && piece < 0; p++) {
            if (a == MC_EDGE_COLOR[p][0] && b == MC_EDGE_COLOR[p][1]) {
                piece = p; flip = 0;
            } else if (a == MC_EDGE_COLOR[p][1] && b == MC_EDGE_COLOR[p][0]) {
                piece = p; flip = 1;
            }
        }
        if (piece < 0) { g_badFacelet = MC_EDGE_FACELET[slot][0];
                         return MC_ERR_BAD_EDGE; }
        if (usedE[piece]) { g_badFacelet = MC_EDGE_FACELET[slot][0];
                            return MC_ERR_DUP_PIECE; }
        usedE[piece] = true;
        out.e[slot] = (uint8_t)(piece | (flip << 4));
    }

    int twistSum = 0;
    for (int i = 0; i < 8; i++) twistSum += mcCornerOri(out, i);
    if (twistSum % 3) return MC_ERR_CORNER_TWIST;

    int flipSum = 0;
    for (int i = 0; i < 12; i++) flipSum += mcEdgeOri(out, i);
    if (flipSum % 2) return MC_ERR_EDGE_FLIP;

    uint8_t cp[8], ep[12];
    for (int i = 0; i < 8; i++) cp[i] = mcCornerPerm(out, i);
    for (int i = 0; i < 12; i++) ep[i] = mcEdgePerm(out, i);
    int swaps = 0;
    for (int i = 0; i < 8; i++)
        while (cp[i] != i) { uint8_t j = cp[i]; uint8_t t = cp[i];
                             cp[i] = cp[j]; cp[j] = t; swaps++; }
    for (int i = 0; i < 12; i++)
        while (ep[i] != i) { uint8_t j = ep[i]; uint8_t t = ep[i];
                             ep[i] = ep[j]; ep[j] = t; swaps++; }
    if (swaps % 2) return MC_ERR_PARITY;

    return MC_OK;
}

void mcCubeToFacelets(const McCube& s, uint8_t stickers[54]) {
    for (int f = 0; f < 6; f++) stickers[f * 9 + 4] = (uint8_t)f;
    for (int slot = 0; slot < 8; slot++) {
        int piece = mcCornerPerm(s, slot), twist = mcCornerOri(s, slot);
        for (int r = 0; r < 3; r++)
            stickers[MC_CORNER_FACELET[slot][(r + twist) % 3]] =
                MC_CORNER_COLOR[piece][r];
    }
    for (int slot = 0; slot < 12; slot++) {
        int piece = mcEdgePerm(s, slot), flip = mcEdgeOri(s, slot);
        for (int r = 0; r < 2; r++)
            stickers[MC_EDGE_FACELET[slot][(r + flip) % 2]] =
                MC_EDGE_COLOR[piece][r];
    }
}

const char* mcStageName(McStage s) {
    switch (s) {
        case MC_STAGE_CROSS:        return "WHITE CROSS";
        case MC_STAGE_F2L_CORNERS:  return "WHITE CORNERS";
        case MC_STAGE_F2L_EDGES:    return "MIDDLE LAYER";
        case MC_STAGE_OLL_CROSS:    return "YELLOW CROSS";
        case MC_STAGE_OLL_CORNERS:  return "YELLOW FACE";
        case MC_STAGE_PLL_CORNERS:  return "CORNER POSITIONS";
        case MC_STAGE_PLL_EDGES:    return "FINAL EDGES";
        default:                    return "";
    }
}

uint16_t mcDbgTwist(const McCube& s){ return mcTwist(s); }
uint16_t mcDbgFlip(const McCube& s){ return mcFlip(s); }
uint16_t mcDbgSlice(const McCube& s){ return mcUdSlice(s); }
