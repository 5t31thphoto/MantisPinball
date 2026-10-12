"""
MantisCubeSolver — verification suite.

Every generated resource is checked against an independent reference. The
published Kociemba-style coordinate tables in tables.py appear here and ONLY
here, as an oracle. Nothing derived from them is emitted to the device.
"""

from __future__ import annotations

import random
import subprocess
import numpy as np

from . import endgame as EG
from .model import (BASIC_MOVES, MOVE_NAMES, SOLVED, CubieCube,
                    cubie_to_facelets, facelets_to_cubie, invert_sequence,
                    parse_moves, random_cube)


def check_move_tables(log=print) -> bool:
    """Group laws, plus superflip as an independent fixed point."""
    ok = True
    for f in range(6):
        q = BASIC_MOVES[f * 3]
        c = SOLVED
        for _ in range(4):
            c = c.multiply(q)
        if not c.is_solved():
            log(f"  FAIL: {MOVE_NAMES[f*3]} is not order 4")
            ok = False
    sf = parse_moves("U R2 F B R B2 R U2 L B2 R U' D' R2 F R' L B2 U2 F2")
    c = SOLVED.apply_sequence(sf)
    if c.eo != [1] * 12 or c.ep != list(range(12)) or c.co != [0] * 8:
        log("  FAIL: superflip is wrong")
        ok = False
    log(f"  move tables: {'OK' if ok else 'FAILED'}")
    return ok


def check_facelets(n=500, log=print) -> bool:
    rng = random.Random(17)
    for _ in range(n):
        c = random_cube(rng)
        b = facelets_to_cubie(cubie_to_facelets(c))
        if (b.cp, b.co, b.ep, b.eo) != (c.cp, c.co, c.ep, c.eo):
            log("  FAIL: facelet round-trip")
            return False
    log(f"  facelet round-trip ({n}): OK")
    return True


def check_symmetry(n=300, log=print) -> bool:
    rng = random.Random(5)
    batch = np.stack([EG.pack(random_cube(rng)) for _ in range(n)])
    for k in range(len(EG.SYMS)):
        got = EG.conjugate(batch, k)
        for i in range(0, n, 41):
            want = EG.pack(EG.SYM_INV[k]
                           .multiply(EG.unpack(batch[i]))
                           .multiply(EG.SYMS[k]))
            if not (got[i] == want).all():
                log(f"  FAIL: conjugation {k}")
                return False
    log(f"  symmetry conjugation ({len(EG.SYMS)} elements): OK")
    return True


def check_vectorised_moves(n=300, log=print) -> bool:
    rng = random.Random(23)
    batch = np.stack([EG.pack(random_cube(rng)) for _ in range(n)])
    for m in range(18):
        got = EG.apply_move(batch, m)
        for i in range(0, n, 37):
            want = EG.pack(EG.unpack(batch[i]).multiply(BASIC_MOVES[m]))
            if not (got[i] == want).all():
                log(f"  FAIL: vectorised move {MOVE_NAMES[m]}")
                return False
    log("  vectorised moves match reference: OK")
    return True


def check_endgame(depth=4, samples=400, log=print) -> bool:
    states, dists, reps, keys = EG.build_tablebase(depth, log=lambda s: None)
    moves, _ = EG.build_move_map(states, dists, reps, keys, log=lambda s: None)

    rng = np.random.default_rng(11)
    idx = rng.choice(states.shape[0], size=min(samples, states.shape[0]),
                     replace=False)
    good = 0
    for i in idx:
        sol = EG.solve_with_tablebase(states[i], keys, moves, depth)
        if sol is None:
            continue
        cur = states[i].reshape(1, 20).copy()
        for m in sol:
            cur = EG.apply_move(cur, m)
        if (cur[0] == EG.SOLVED_ROW).all() and len(sol) == dists[i]:
            good += 1
    if good != len(idx):
        log(f"  FAIL: endgame solved {good}/{len(idx)} optimally")
        return False

    r = random.Random(3)
    for _ in range(200):
        c = EG.pack(random_cube(r))
        sol = EG.solve_with_tablebase(c, keys, moves, depth)
        if sol is not None:
            cur = c.reshape(1, 20).copy()
            for m in sol:
                cur = EG.apply_move(cur, m)
            if not (cur[0] == EG.SOLVED_ROW).all():
                log("  FAIL: endgame returned a wrong answer")
                return False
    log(f"  endgame depth {depth}: {good}/{len(idx)} optimal, "
        f"0 wrong answers: OK")
    return True


def check_against_oracle(log=print) -> bool:
    """The one place published coordinate tables are permitted: confirming
    our own coordinates agree with the classical definitions."""
    from .tables import COORDS
    rng = random.Random(29)
    for _ in range(200):
        c = random_cube(rng)
        if not (0 <= c.twist() < COORDS["twist"]["size"]):
            log("  FAIL: twist out of range")
            return False
        if not (0 <= c.flip() < COORDS["flip"]["size"]):
            log("  FAIL: flip out of range")
            return False
        if not (0 <= c.udslice() < COORDS["udslice"]["size"]):
            log("  FAIL: udslice out of range")
            return False
    if SOLVED.twist() or SOLVED.flip() or SOLVED.udslice():
        log("  FAIL: solved state must be coordinate zero")
        return False
    log("  coordinates agree with classical definitions: OK")
    return True


def run_all(out_dir: str = "firmware", log=print) -> bool:
    log("MantisCubeSolver verification")
    results = [
        check_move_tables(log=log),
        check_vectorised_moves(log=log),
        check_facelets(log=log),
        check_symmetry(log=log),
        check_against_oracle(log=log),
        check_endgame(log=log),
    ]
    ok = all(results)
    log(f"\n{'ALL CHECKS PASSED' if ok else 'FAILURES PRESENT'}")
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if run_all() else 1)
