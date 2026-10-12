"""
Cubie-level cube state, facelet conversion, validation, and coordinates.

The coordinate encoders here are the contract between the Python reference
and the firmware: both sides must produce identical integers for identical
states, or the shared distance tables are meaningless.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import List, Optional, Sequence, Tuple

from .geometry import (
    CORNER_AXES,
    CORNER_CELL,
    CORNER_MOVE,
    CORNER_NAMES,
    EDGE_AXES,
    EDGE_CELL,
    EDGE_MOVE,
    EDGE_NAMES,
    FACELETS,
    FACELET_MOVE,
    FACE_ORDER,
    _clockwise_corner_frame,
)

# All 18 half-turn-metric moves, in a fixed order both sides agree on.
MOVE_NAMES: List[str] = []
for _f in ["U", "R", "F", "D", "L", "B"]:
    for _s in ["", "'", "2"]:
        MOVE_NAMES.append(_f + _s)

MOVE_INDEX = {m: i for i, m in enumerate(MOVE_NAMES)}

# The <U,D,R2,L2,F2,B2> subgroup used in phase 2.
PHASE2_MOVES = [MOVE_INDEX[m] for m in ["U", "U'", "U2", "D", "D'", "D2",
                                        "R2", "L2", "F2", "B2"]]


class InvalidCube(ValueError):
    """Raised when a sticker layout cannot correspond to a real cube."""


@dataclass
class CubieCube:
    cp: List[int] = field(default_factory=lambda: list(range(8)))
    co: List[int] = field(default_factory=lambda: [0] * 8)
    ep: List[int] = field(default_factory=lambda: list(range(12)))
    eo: List[int] = field(default_factory=lambda: [0] * 12)

    def copy(self) -> "CubieCube":
        return CubieCube(list(self.cp), list(self.co), list(self.ep), list(self.eo))

    def is_solved(self) -> bool:
        return (
            self.cp == list(range(8))
            and self.co == [0] * 8
            and self.ep == list(range(12))
            and self.eo == [0] * 12
        )

    # -- group operations ---------------------------------------------------

    def multiply(self, other: "CubieCube") -> "CubieCube":
        cp = [self.cp[other.cp[i]] for i in range(8)]
        co = [(self.co[other.cp[i]] + other.co[i]) % 3 for i in range(8)]
        ep = [self.ep[other.ep[i]] for i in range(12)]
        eo = [(self.eo[other.ep[i]] + other.eo[i]) % 2 for i in range(12)]
        return CubieCube(cp, co, ep, eo)

    def inverse(self) -> "CubieCube":
        cp = [0] * 8
        co = [0] * 8
        ep = [0] * 12
        eo = [0] * 12
        for i in range(8):
            cp[self.cp[i]] = i
        for i in range(8):
            co[i] = (-self.co[cp[i]]) % 3
        for i in range(12):
            ep[self.ep[i]] = i
        for i in range(12):
            eo[i] = self.eo[ep[i]]
        return CubieCube(cp, co, ep, eo)

    def apply(self, move: int) -> "CubieCube":
        return self.multiply(BASIC_MOVES[move])

    def apply_sequence(self, moves: Sequence[int]) -> "CubieCube":
        c = self
        for m in moves:
            c = c.apply(m)
        return c

    # -- coordinates --------------------------------------------------------

    def twist(self) -> int:
        """Corner orientation, 0..2186."""
        t = 0
        for i in range(7):
            t = t * 3 + self.co[i]
        return t

    def flip(self) -> int:
        """Edge orientation, 0..2047."""
        f = 0
        for i in range(11):
            f = f * 2 + self.eo[i]
        return f

    def udslice(self) -> int:
        """Which 4 slots hold the FR/FL/BL/BR edges, 0..494 (position only)."""
        occupied = [self.ep[i] >= 8 for i in range(12)]
        return _combination_index(occupied, 4)

    def corner_perm(self) -> int:
        return _permutation_index(self.cp)

    def edge8_perm(self) -> int:
        """Permutation of the 8 U/D-layer edges. Only meaningful in phase 2."""
        return _permutation_index(self.ep[:8])

    def slice_perm(self) -> int:
        """Permutation of the 4 E-slice edges. Only meaningful in phase 2."""
        return _permutation_index([e - 8 for e in self.ep[8:]])

    def in_phase2(self) -> bool:
        return self.twist() == 0 and self.flip() == 0 and self.udslice() == 0


def _combination_index(flags: Sequence[bool], k: int) -> int:
    """Rank of a k-subset of 12 slots, 0..C(12,k)-1."""
    n = len(flags)
    chosen = [i for i, f in enumerate(flags) if f]
    if len(chosen) != k:
        raise ValueError(f"expected {k} set flags, got {len(chosen)}")
    rank = 0
    prev = -1
    for j, pos in enumerate(chosen):
        for p in range(prev + 1, pos):
            rank += _binom(n - 1 - p, k - 1 - j)
        prev = pos
    # Normalise so that the solved arrangement (slice edges in the slice)
    # ranks 0, which lets the firmware test `udslice() == 0` for phase 2.
    return _binom(n, k) - 1 - rank


_BINOM_CACHE = {}


def _binom(n: int, k: int) -> int:
    if k < 0 or k > n:
        return 0
    key = (n, k)
    if key not in _BINOM_CACHE:
        r = 1
        for i in range(k):
            r = r * (n - i) // (i + 1)
        _BINOM_CACHE[key] = r
    return _BINOM_CACHE[key]


def _permutation_index(perm: Sequence[int]) -> int:
    """Lehmer-code rank of a permutation, 0..n!-1."""
    p = list(perm)
    n = len(p)
    idx = 0
    for i in range(n):
        idx *= n - i
        for j in range(i + 1, n):
            if p[j] < p[i]:
                idx += 1
    return idx


# ---------------------------------------------------------------------------
# Basic moves as CubieCubes
# ---------------------------------------------------------------------------

def _basic(face: str) -> CubieCube:
    cp, co = CORNER_MOVE[face]
    ep, eo = EDGE_MOVE[face]
    return CubieCube(list(cp), list(co), list(ep), list(eo))


BASIC_MOVES: List[CubieCube] = []
for _f in ["U", "R", "F", "D", "L", "B"]:
    _q = _basic(_f)
    _c = _q
    BASIC_MOVES.append(_c)                       # X
    BASIC_MOVES.append(_c.multiply(_c).multiply(_c))  # X'
    BASIC_MOVES.append(_c.multiply(_c))          # X2

SOLVED = CubieCube()


def invert_move(m: int) -> int:
    face, kind = divmod(m, 3)
    if kind == 0:
        return face * 3 + 1
    if kind == 1:
        return face * 3 + 0
    return m


def invert_sequence(moves: Sequence[int]) -> List[int]:
    return [invert_move(m) for m in reversed(moves)]


def format_moves(moves: Sequence[int]) -> str:
    return " ".join(MOVE_NAMES[m] for m in moves)


def parse_moves(text: str) -> List[int]:
    out = []
    for tok in text.replace(",", " ").split():
        tok = tok.strip()
        if not tok:
            continue
        if tok not in MOVE_INDEX:
            raise ValueError(f"unknown move: {tok!r}")
        out.append(MOVE_INDEX[tok])
    return out


# ---------------------------------------------------------------------------
# Facelet <-> cubie
# ---------------------------------------------------------------------------
# Facelet colour values 0..5 index FACE_ORDER = U,R,F,D,L,B. The firmware's
# palette maps those to White, Red, Green, Yellow, Orange, Blue.

def _corner_facelets():
    """For each corner slot, the 3 facelet indices in clockwise order
    starting at the U/D sticker."""
    out = []
    for slot in range(8):
        frame = _clockwise_corner_frame(slot)
        cell = CORNER_CELL[slot]
        out.append([FACELETS.index((cell, n)) for n in frame])
    return out


def _edge_facelets():
    out = []
    for slot in range(12):
        cell = EDGE_CELL[slot]
        out.append([FACELETS.index((cell, n)) for n in EDGE_AXES[slot]])
    return out


CORNER_FACELET = _corner_facelets()
EDGE_FACELET = _edge_facelets()

# Colour of each solved cubie's stickers, in the same order.
CORNER_COLOR = [
    [FACE_ORDER.index(ch) for ch in _ordered_name]
    for _ordered_name in [
        [c for c in CORNER_NAMES[s]] for s in range(8)
    ]
]


def _solved_colors():
    """Colour triples/pairs of each solved cubie, matching facelet order."""
    solved = [i // 9 for i in range(54)]
    corners = [[solved[f] for f in CORNER_FACELET[s]] for s in range(8)]
    edges = [[solved[f] for f in EDGE_FACELET[s]] for s in range(12)]
    return corners, edges


SOLVED_CORNER_COLORS, SOLVED_EDGE_COLORS = _solved_colors()


def facelets_to_cubie(stickers: Sequence[int]) -> CubieCube:
    """Convert 54 colour values to a CubieCube. Raises InvalidCube."""
    if len(stickers) != 54:
        raise InvalidCube(f"expected 54 stickers, got {len(stickers)}")
    if any(not (0 <= s <= 5) for s in stickers):
        raise InvalidCube("sticker colours must be 0..5")

    counts = [0] * 6
    for s in stickers:
        counts[s] += 1
    bad = [i for i, c in enumerate(counts) if c != 9]
    if bad:
        names = ", ".join(f"{FACE_ORDER[i]}={counts[i]}" for i in bad)
        raise InvalidCube(f"each colour must appear 9 times ({names})")

    centers = [stickers[f * 9 + 4] for f in range(6)]
    if sorted(centers) != list(range(6)):
        raise InvalidCube("the six centre stickers must all be different")
    if centers != list(range(6)):
        raise InvalidCube(
            "centres are not in standard orientation; hold the cube with "
            "the first colour up and the third colour front"
        )

    cp = [-1] * 8
    co = [0] * 8
    corner_lookup = {}
    for i, cols in enumerate(SOLVED_CORNER_COLORS):
        for r in range(3):
            corner_lookup[tuple(cols[r:] + cols[:r])] = (i, (3 - r) % 3)

    for slot in range(8):
        got = tuple(stickers[f] for f in CORNER_FACELET[slot])
        if got not in corner_lookup:
            raise InvalidCube(
                f"corner {CORNER_NAMES[slot]} has an impossible colour "
                f"combination"
            )
        piece, twist = corner_lookup[got]
        cp[slot] = piece
        co[slot] = twist

    ep = [-1] * 12
    eo = [0] * 12
    edge_lookup = {}
    for i, cols in enumerate(SOLVED_EDGE_COLORS):
        edge_lookup[tuple(cols)] = (i, 0)
        edge_lookup[tuple(reversed(cols))] = (i, 1)

    for slot in range(12):
        got = tuple(stickers[f] for f in EDGE_FACELET[slot])
        if got not in edge_lookup:
            raise InvalidCube(
                f"edge {EDGE_NAMES[slot]} has an impossible colour combination"
            )
        piece, flip = edge_lookup[got]
        ep[slot] = piece
        eo[slot] = flip

    if sorted(cp) != list(range(8)):
        raise InvalidCube("a corner piece is used twice")
    if sorted(ep) != list(range(12)):
        raise InvalidCube("an edge piece is used twice")

    cube = CubieCube(cp, co, ep, eo)
    validate(cube)
    return cube


def validate(cube: CubieCube) -> None:
    """The three parity laws. Any violation means the cube was mis-entered
    or has been physically taken apart and reassembled."""
    if sum(cube.co) % 3 != 0:
        raise InvalidCube(
            "corner twist parity is wrong: one corner is rotated in place"
        )
    if sum(cube.eo) % 2 != 0:
        raise InvalidCube(
            "edge flip parity is wrong: one edge is flipped in place"
        )
    if _parity(cube.cp) != _parity(cube.ep):
        raise InvalidCube(
            "permutation parity is wrong: two pieces are swapped"
        )


def _parity(perm: Sequence[int]) -> int:
    p = list(perm)
    swaps = 0
    for i in range(len(p)):
        while p[i] != i:
            j = p[i]
            p[i], p[j] = p[j], p[i]
            swaps += 1
    return swaps % 2


def cubie_to_facelets(cube: CubieCube) -> List[int]:
    out = [0] * 54
    for f in range(6):
        out[f * 9 + 4] = f
    for slot in range(8):
        piece = cube.cp[slot]
        twist = cube.co[slot]
        cols = SOLVED_CORNER_COLORS[piece]
        for r in range(3):
            out[CORNER_FACELET[slot][(r + twist) % 3]] = cols[r]
    for slot in range(12):
        piece = cube.ep[slot]
        flip = cube.eo[slot]
        cols = SOLVED_EDGE_COLORS[piece]
        for r in range(2):
            out[EDGE_FACELET[slot][(r + flip) % 2]] = cols[r]
    return out


def random_cube(rng=None) -> CubieCube:
    import random

    rng = rng or random
    while True:
        cp = list(range(8))
        ep = list(range(12))
        rng.shuffle(cp)
        rng.shuffle(ep)
        if _parity(cp) != _parity(ep):
            continue
        co = [rng.randrange(3) for _ in range(8)]
        co[7] = (-sum(co[:7])) % 3
        eo = [rng.randrange(2) for _ in range(12)]
        eo[11] = (-sum(eo[:11])) % 2
        return CubieCube(cp, co, ep, eo)
