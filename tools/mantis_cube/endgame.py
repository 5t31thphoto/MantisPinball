"""
Endgame tablebase: exact optimal moves for every state within N moves of solved.

Three ideas make this small enough to ship, none of which a published solver
can use:

  * Symmetry folding. States within the endgame ball collapse ~23x under the
    24 rotations (~46x with the mirror). Recovering a class representative is
    24 conjugations, which is vector arithmetic - free on the ESP32-S3 PIE
    unit, expensive on the scalar desktop CPUs these tables were designed for.

  * Store the move, not the distance. Five bits per class. Admissibility is an
    IDA* requirement and we do not run IDA*.

  * Self-verifying lookup. Membership testing normally costs a Bloom filter or
    stored keys. Instead: look up, apply, repeat up to depth times, and check
    whether you reached solved. If you did not, the state was not in the table.
    A wrong answer is impossible by construction and costs nothing to exclude.

States are the packed 20-byte firmware layout, so what this verifies is what
the device executes.
"""

from __future__ import annotations

import numpy as np

from .geometry import (AXES, CORNER_AXES, CORNER_CELL, EDGE_AXES, EDGE_CELL,
                       _clockwise_corner_frame, rot90)
from .model import BASIC_MOVES, MOVE_NAMES, CubieCube, SOLVED

# --- packed 20-byte representation, identical to McCube in cube_core.h ------
# byte 0..7   corner: perm (bits 0-2) | ori (bits 3-4)
# byte 8..19  edge:   perm (bits 0-3) | ori (bit 4)

MCP = np.array([m.cp for m in BASIC_MOVES], dtype=np.uint8)
MCO = np.array([m.co for m in BASIC_MOVES], dtype=np.uint8)
MEP = np.array([m.ep for m in BASIC_MOVES], dtype=np.uint8)
MEO = np.array([m.eo for m in BASIC_MOVES], dtype=np.uint8)


def pack(c: CubieCube) -> np.ndarray:
    out = np.empty(20, dtype=np.uint8)
    for i in range(8):
        out[i] = c.cp[i] | (c.co[i] << 3)
    for i in range(12):
        out[8 + i] = c.ep[i] | (c.eo[i] << 4)
    return out


def unpack(row: np.ndarray) -> CubieCube:
    cp = [int(row[i] & 7) for i in range(8)]
    co = [int((row[i] >> 3) & 3) for i in range(8)]
    ep = [int(row[8 + i] & 15) for i in range(12)]
    eo = [int((row[8 + i] >> 4) & 1) for i in range(12)]
    return CubieCube(cp, co, ep, eo)


SOLVED_ROW = pack(SOLVED)


def apply_move(states: np.ndarray, m: int) -> np.ndarray:
    """Apply move m to an (N, 20) batch. This is the scalar reference for the
    SoA SIMD path: same permutation, same orientation arithmetic."""
    out = np.empty_like(states)
    src_c = states[:, MCP[m]]
    ori_c = ((src_c >> 3) & 3) + MCO[m]
    ori_c = np.where(ori_c >= 3, ori_c - 3, ori_c)
    out[:, :8] = (src_c & 7) | (ori_c << 3)

    src_e = states[:, 8 + MEP[m].astype(np.intp)]
    ori_e = ((src_e >> 4) & 1) ^ MEO[m]
    out[:, 8:] = (src_e & 15) | (ori_e << 4)
    return out


# --- symmetry group --------------------------------------------------------

_CI = {c: i for i, c in enumerate(CORNER_CELL)}
_EI = {c: i for i, c in enumerate(EDGE_CELL)}


def _rotation(seq) -> CubieCube:
    cp = [0] * 8; co = [0] * 8; ep = [0] * 12; eo = [0] * 12

    def R(v):
        for ax, t in seq:
            for _ in range(t):
                v = rot90(v, AXES[ax], 1)
        return v

    for s in range(8):
        d = _CI[R(CORNER_CELL[s])]
        n = R(CORNER_AXES[s][0])
        co[d] = _clockwise_corner_frame(d).index(n)
        cp[d] = s
    for s in range(12):
        d = _EI[R(EDGE_CELL[s])]
        n = R(EDGE_AXES[s][0])
        eo[d] = 0 if n == EDGE_AXES[d][0] else 1
        ep[d] = s
    return CubieCube(cp, co, ep, eo)


def build_symmetries():
    seen = {}
    for a in range(4):
        for b in range(4):
            for c in range(4):
                s = _rotation([("U", a), ("F", b), ("R", c)])
                seen[(tuple(s.cp), tuple(s.co), tuple(s.ep), tuple(s.eo))] = s
    return list(seen.values())


SYMS = build_symmetries()
SYM_INV = [s.inverse() for s in SYMS]

_S_CP = np.array([s.cp for s in SYMS], dtype=np.intp)
_S_CO = np.array([s.co for s in SYMS], dtype=np.uint8)
_S_EP = np.array([s.ep for s in SYMS], dtype=np.intp)
_S_EO = np.array([s.eo for s in SYMS], dtype=np.uint8)
_SI_CP = np.array([s.cp for s in SYM_INV], dtype=np.uint8)
_SI_CO = np.array([s.co for s in SYM_INV], dtype=np.uint8)
_SI_EP = np.array([s.ep for s in SYM_INV], dtype=np.uint8)
_SI_EO = np.array([s.eo for s in SYM_INV], dtype=np.uint8)


def conjugate(states: np.ndarray, k: int) -> np.ndarray:
    """S_k^-1 * X * S_k over an (N, 20) batch."""
    out = np.empty_like(states)

    src = states[:, _S_CP[k]]
    p = src & 7
    o = (src >> 3) & 3
    np_ = _SI_CP[k][p]
    no = (_SI_CO[k][p].astype(np.uint16) + o + _S_CO[k]) % 3
    out[:, :8] = np_ | (no.astype(np.uint8) << 3)

    src = states[:, 8 + _S_EP[k]]
    p = src & 15
    o = (src >> 4) & 1
    np_ = _SI_EP[k][p]
    no = _SI_EO[k][p] ^ o ^ _S_EO[k]
    out[:, 8:] = np_ | (no << 4)
    return out


def _rows_view(a: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(a).view([('', a.dtype)] * a.shape[1]).ravel()


def class_representative(states: np.ndarray, chunk: int = 200_000) -> np.ndarray:
    """Lexicographically smallest conjugate, as a canonical class key."""
    n = states.shape[0]
    best = np.empty_like(states)
    for lo in range(0, n, chunk):
        hi = min(lo + chunk, n)
        blk = states[lo:hi]
        cur = conjugate(blk, 0)
        for k in range(1, len(SYMS)):
            cand = conjugate(blk, k)
            worse = np.zeros(cur.shape[0], dtype=bool)
            better = np.zeros(cur.shape[0], dtype=bool)
            for col in range(20):
                undecided = ~(worse | better)
                if not undecided.any():
                    break
                better |= undecided & (cand[:, col] < cur[:, col])
                worse |= undecided & (cand[:, col] > cur[:, col])
            cur[better] = cand[better]
        best[lo:hi] = cur
    return best


# --- tablebase construction ------------------------------------------------

def build_tablebase(depth: int, log=print):
    """BFS the ball of radius `depth` around solved, fold by symmetry, and
    record for each class one move that strictly decreases distance."""
    states = SOLVED_ROW.reshape(1, 20)
    all_states = [states]
    all_dist = [np.zeros(1, dtype=np.uint8)]
    seen = _rows_view(states).copy()
    seen.sort()

    frontier = states
    for d in range(1, depth + 1):
        kids = np.concatenate([apply_move(frontier, m) for m in range(18)])
        kv = _rows_view(kids)
        kv_sorted = np.unique(kv)
        fresh = kv_sorted[~np.isin(kv_sorted, seen)]
        kids = fresh.view(np.uint8).reshape(-1, 20)

        all_states.append(kids)
        all_dist.append(np.full(kids.shape[0], d, dtype=np.uint8))
        seen = np.union1d(seen, fresh)
        frontier = kids
        log(f"    depth {d}: {kids.shape[0]:,} new, {seen.shape[0]:,} total")

    states = np.concatenate(all_states)
    dists = np.concatenate(all_dist)
    log(f"  ball size: {states.shape[0]:,}")

    reps = class_representative(states)
    rv = _rows_view(reps)
    order = np.argsort(rv, kind="stable")
    rv_sorted = rv[order]
    keep = np.ones(len(rv_sorted), dtype=bool)
    keep[1:] = rv_sorted[1:] != rv_sorted[:-1]
    class_keys = rv_sorted[keep]
    log(f"  symmetry classes: {len(class_keys):,} "
        f"({states.shape[0] / len(class_keys):.1f}x reduction)")
    return states, dists, reps, class_keys


def build_move_map(states, dists, reps, class_keys, log=print):
    """For each symmetry class, one move that strictly decreases distance.

    Stored against the class representative, so the device canonicalises a
    state first and then reads a single 5-bit move.
    """
    rv = _rows_view(reps)
    key_index = {k.tobytes(): i for i, k in enumerate(class_keys)}

    dist_of = {}
    for i, k in enumerate(_rows_view(states)):
        dist_of[k.tobytes()] = dists[i]

    moves = np.full(len(class_keys), 255, dtype=np.uint8)
    # One canonical member per class is enough: any move that decreases
    # distance for the representative decreases it for every conjugate.
    seen_class = set()
    for i in range(states.shape[0]):
        ck = rv[i].tobytes()
        if ck in seen_class:
            continue
        seen_class.add(ck)
        d = dists[i]
        if d == 0:
            moves[key_index[ck]] = 254   # already solved
            continue
        row = states[i:i + 1]
        for m in range(18):
            child = apply_move(row, m)
            cd = dist_of.get(_rows_view(child)[0].tobytes())
            if cd is not None and cd == d - 1:
                moves[key_index[ck]] = m
                break
    unresolved = int((moves == 255).sum())
    log(f"  classes with a decreasing move: "
        f"{len(class_keys) - unresolved:,}/{len(class_keys):,}")
    return moves, key_index


# --- move conjugation ------------------------------------------------------
# If R = S^-1 X S and move m decreases R's distance, the move to apply to X
# is S m S^-1. Symmetries map face turns to face turns, so this is a 24x18
# table of move indices.

def build_move_conjugation():
    lookup = {}
    for m in range(18):
        c = BASIC_MOVES[m]
        lookup[(tuple(c.cp), tuple(c.co), tuple(c.ep), tuple(c.eo))] = m
    tbl = np.zeros((len(SYMS), 18), dtype=np.uint8)
    for k in range(len(SYMS)):
        for m in range(18):
            r = SYMS[k].multiply(BASIC_MOVES[m]).multiply(SYM_INV[k])
            tbl[k, m] = lookup[(tuple(r.cp), tuple(r.co),
                                tuple(r.ep), tuple(r.eo))]
    return tbl


SYM_MOVE = build_move_conjugation()


def which_symmetry(state_row, rep_row):
    """Index k with S_k^-1 * state * S_k == rep."""
    s = state_row.reshape(1, 20)
    for k in range(len(SYMS)):
        if (conjugate(s, k)[0] == rep_row).all():
            return k
    return -1


def solve_with_tablebase(state_row, class_keys, moves, depth):
    """Self-verifying lookup: follow the chain and check we reach solved.
    Returns the move list, or None if the state was outside the ball. No
    membership structure is needed - execution is the membership test."""
    key_index = {k.tobytes(): i for i, k in enumerate(class_keys)}
    cur = state_row.reshape(1, 20).copy()
    out = []
    for _ in range(depth + 1):
        if (cur[0] == SOLVED_ROW).all():
            return out
        rep = class_representative(cur)[0]
        idx = key_index.get(_rows_view(rep.reshape(1, 20))[0].tobytes())
        if idx is None or moves[idx] >= 254:
            return None
        k = which_symmetry(cur[0], rep)
        if k < 0:
            return None
        m = int(SYM_MOVE[k, moves[idx]])
        out.append(m)
        cur = apply_move(cur, m)
    return None
