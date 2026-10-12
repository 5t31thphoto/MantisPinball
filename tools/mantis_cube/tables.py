"""
Coordinate move tables and exact BFS distance tables.

These distance tables are the whole reason this solver fits in flash. A
Kociemba IDA* solver needs *product* pruning tables (twist x slice is 1.08M
entries) because IDA* requires an admissible heuristic. Beam search does not,
so we take the max over individual coordinate tables instead and pay ~85 KB
total rather than ~2 MB.

Everything here runs on the host at build time. The device only ever reads
the emitted const arrays.
"""

from __future__ import annotations

import sys
from collections import deque
from typing import Callable, Dict, List, Sequence

from .model import (
    BASIC_MOVES,
    MOVE_NAMES,
    PHASE2_MOVES,
    CubieCube,
    _permutation_index,
)

ALL_MOVES = list(range(18))


def _decode_twist(idx: int) -> CubieCube:
    co = [0] * 8
    t = idx
    for i in range(6, -1, -1):
        co[i] = t % 3
        t //= 3
    co[7] = (-sum(co[:7])) % 3
    return CubieCube(co=co)


def _decode_flip(idx: int) -> CubieCube:
    eo = [0] * 12
    f = idx
    for i in range(10, -1, -1):
        eo[i] = f % 2
        f //= 2
    eo[11] = (-sum(eo[:11])) % 2
    return CubieCube(eo=eo)


def _decode_udslice(idx: int) -> CubieCube:
    """Any arrangement with the correct set of slice-occupied slots."""
    from .model import _binom

    target = _binom(12, 4) - 1 - idx
    chosen: List[int] = []
    rank = target
    prev = -1
    for j in range(4):
        pos = prev + 1
        while True:
            c = _binom(12 - 1 - pos, 4 - 1 - j)
            if rank < c:
                break
            rank -= c
            pos += 1
        chosen.append(pos)
        prev = pos
    ep = [-1] * 12
    for j, pos in enumerate(chosen):
        ep[pos] = 8 + j
    fill = [e for e in range(8)]
    k = 0
    for i in range(12):
        if ep[i] < 0:
            ep[i] = fill[k]
            k += 1
    return CubieCube(ep=ep)


def _decode_perm(idx: int, n: int) -> List[int]:
    """Inverse of the Lehmer rank used in model._permutation_index."""
    digits = [0] * n
    x = idx
    for i in range(n - 1, -1, -1):
        digits[i] = x % (n - i)
        x //= (n - i)
    avail = list(range(n))
    out = []
    for i in range(n):
        out.append(avail.pop(digits[i]))
    return out


def _decode_corner_perm(idx: int) -> CubieCube:
    return CubieCube(cp=_decode_perm(idx, 8))


def _decode_edge8_perm(idx: int) -> CubieCube:
    return CubieCube(ep=_decode_perm(idx, 8) + [8, 9, 10, 11])


def _decode_slice_perm(idx: int) -> CubieCube:
    return CubieCube(ep=list(range(8)) + [8 + v for v in _decode_perm(idx, 4)])


COORDS = {
    "twist": dict(size=2187, moves=ALL_MOVES,
                  decode=_decode_twist, encode=lambda c: c.twist()),
    "flip": dict(size=2048, moves=ALL_MOVES,
                 decode=_decode_flip, encode=lambda c: c.flip()),
    "udslice": dict(size=495, moves=ALL_MOVES,
                    decode=_decode_udslice, encode=lambda c: c.udslice()),
    "cornerperm": dict(size=40320, moves=PHASE2_MOVES,
                       decode=_decode_corner_perm,
                       encode=lambda c: c.corner_perm()),
    "edge8perm": dict(size=40320, moves=PHASE2_MOVES,
                      decode=_decode_edge8_perm,
                      encode=lambda c: c.edge8_perm()),
    "sliceperm": dict(size=24, moves=PHASE2_MOVES,
                      decode=_decode_slice_perm,
                      encode=lambda c: c.slice_perm()),
}


def build_move_table(name: str, log=None) -> List[List[int]]:
    spec = COORDS[name]
    size, moves = spec["size"], spec["moves"]
    decode, encode = spec["decode"], spec["encode"]
    table = [[0] * len(moves) for _ in range(size)]
    for i in range(size):
        base = decode(i)
        for j, m in enumerate(moves):
            table[i][j] = encode(base.multiply(BASIC_MOVES[m]))
        if log and size > 5000 and i % 8192 == 0 and i:
            log(f"    {name} move table {i}/{size}")
    return table


def build_distance_table(name: str, log=None) -> List[int]:
    """Exact BFS distance from the solved coordinate, in half-turn metric."""
    spec = COORDS[name]
    size, moves = spec["size"], spec["moves"]
    move_table = build_move_table(name, log=log)

    dist = [255] * size
    dist[0] = 0
    frontier = deque([0])
    ply = 0
    reached = 1
    counts = [1]
    while frontier:
        ply += 1
        nxt = deque()
        while frontier:
            s = frontier.popleft()
            row = move_table[s]
            for j in range(len(moves)):
                t = row[j]
                if dist[t] == 255:
                    dist[t] = ply
                    nxt.append(t)
        if nxt:
            reached += len(nxt)
            counts.append(len(nxt))
            if log:
                log(f"    {name}: ply {ply}, {reached}/{size} states reached")
        frontier = nxt

    unreachable = sum(1 for d in dist if d == 255)
    if unreachable:
        raise RuntimeError(
            f"{name}: {unreachable} unreachable states — coordinate is wrong"
        )
    return dist


# ---------------------------------------------------------------------------
# C++ emission
# ---------------------------------------------------------------------------

HEADER_PREAMBLE = """// GENERATED FILE — do not edit by hand.
// Produced by mantis_cube.tables for Mantis Crypto Pinball OS.
//
// All tables are const and live in flash (PROGMEM/.rodata). Nothing here is
// copied to SRAM at runtime, and nothing here touches PSRAM.
#pragma once
#include <stdint.h>

"""


def emit_header(path: str, tables: Dict[str, List[int]],
                beam_width: int = 1024, log=None) -> int:
    from .model import BASIC_MOVES, MOVE_NAMES, PHASE2_MOVES

    total = 0
    lines = [HEADER_PREAMBLE]
    lines.append(f"// Beam width is the single tuning dial. Raising it costs\n"
                 f"// roughly {20 * 3 + 24} bytes of SRAM per unit of width.\n")
    lines.append(f"#define MC_BEAM_WIDTH {beam_width}\n\n")

    # Move names, for UI display and for the host test harness.
    lines.append("static const char* const MC_MOVE_NAMES[18] = {\n  ")
    lines.append(", ".join(f'"{m}"' for m in MOVE_NAMES))
    lines.append("\n};\n\n")

    lines.append(f"#define MC_PHASE2_MOVE_COUNT {len(PHASE2_MOVES)}\n")
    lines.append(
        f"static const uint8_t MC_PHASE2_MOVES[{len(PHASE2_MOVES)}] = {{"
        + ",".join(str(m) for m in PHASE2_MOVES) + "};\n\n"
    )

    # Basic move tables, derived geometrically and property-tested on the host.
    for field, n in (("cp", 8), ("co", 8), ("ep", 12), ("eo", 12)):
        lines.append(
            f"static const uint8_t MC_MOVE_{field.upper()}[18][{n}] = {{\n"
        )
        for m in range(18):
            vals = getattr(BASIC_MOVES[m], field)
            lines.append("  {" + ",".join(str(v) for v in vals)
                         + f"}}, // {MOVE_NAMES[m]}\n")
        lines.append("};\n\n")
        total += 18 * n

    # Facelet <-> cubie mapping, so the firmware's validator and the host
    # model decode identical sticker layouts.
    from .model import (CORNER_FACELET, EDGE_FACELET,
                        SOLVED_CORNER_COLORS, SOLVED_EDGE_COLORS)

    for nm, tbl, w in (("CORNER_FACELET", CORNER_FACELET, 3),
                       ("EDGE_FACELET", EDGE_FACELET, 2),
                       ("CORNER_COLOR", SOLVED_CORNER_COLORS, 3),
                       ("EDGE_COLOR", SOLVED_EDGE_COLORS, 2)):
        rows = len(tbl)
        lines.append(f"static const uint8_t MC_{nm}[{rows}][{w}] = {{\n")
        for r in tbl:
            lines.append("  {" + ",".join(str(v) for v in r) + "},\n")
        lines.append("};\n\n")
        total += rows * w

    for name, dist in tables.items():
        maxd = max(dist)
        lines.append(f"// {name}: {len(dist)} entries, max depth {maxd}\n")
        lines.append(f"#define MC_{name.upper()}_SIZE {len(dist)}\n")
        lines.append(f"#define MC_{name.upper()}_MAXDEPTH {maxd}\n")
        lines.append(
            f"static const uint8_t MC_DIST_{name.upper()}[{len(dist)}] = {{\n"
        )
        for i in range(0, len(dist), 24):
            chunk = ",".join(str(v) for v in dist[i:i + 24])
            lines.append(f"  {chunk},\n")
        lines.append("};\n\n")
        total += len(dist)

    with open(path, "w") as fh:
        fh.write("".join(lines))
    if log:
        log(f"  wrote {path} ({total} table bytes)")
    return total


def build_all(log: Callable[[str], None] = print) -> Dict[str, List[int]]:
    out = {}
    for name in COORDS:
        log(f"  building {name} ({COORDS[name]['size']} states)")
        out[name] = build_distance_table(name, log=log)
        log(f"    {name}: max depth {max(out[name])}")
    return out


if __name__ == "__main__":
    tables = build_all()
    total = emit_header(
        sys.argv[1] if len(sys.argv) > 1 else "cube_tables.h", tables
    )
    print(f"\ntotal flash cost: {total} bytes ({total / 1024:.1f} KB)")
