"""
Geometric ground truth for the 3x3 cube.

Nothing in this module is transcribed from a reference implementation.
Every permutation table is derived from 3D rotations of unit vectors, so the
tables are correct by construction and verifiable by the group-theory
property tests in tests/test_model.py.

Coordinate frame (right-handed):
    +x = R (right)      -x = L
    +y = U (up)         -y = D
    +z = F (front)      -z = B

A clockwise face turn, viewed from outside that face, is a rotation of
-90 degrees about the outward normal under the right-hand rule.
"""

from __future__ import annotations

import itertools

AXES = {
    "U": (0, 1, 0),
    "D": (0, -1, 0),
    "R": (1, 0, 0),
    "L": (-1, 0, 0),
    "F": (0, 0, 1),
    "B": (0, 0, -1),
}

FACE_ORDER = ["U", "R", "F", "D", "L", "B"]


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def rot90(v, axis, turns):
    """Rotate v by turns * -90 degrees about axis (right-hand rule)."""
    x, y, z = v
    ax, ay, az = axis
    for _ in range(turns % 4):
        if (ax, ay, az) == (0, 1, 0):
            x, z = -z, x
        elif (ax, ay, az) == (0, -1, 0):
            x, z = z, -x
        elif (ax, ay, az) == (1, 0, 0):
            y, z = z, -y
        elif (ax, ay, az) == (-1, 0, 0):
            y, z = -z, y
        elif (ax, ay, az) == (0, 0, 1):
            x, y = y, -x
        elif (ax, ay, az) == (0, 0, -1):
            x, y = -y, x
        else:
            raise ValueError(f"not a face axis: {axis}")
    return (x, y, z)


# ---------------------------------------------------------------------------
# Cubies
# ---------------------------------------------------------------------------

CELLS = [c for c in itertools.product((-1, 0, 1), repeat=3)]
CORNER_CELLS = [c for c in CELLS if sum(abs(t) for t in c) == 3]
EDGE_CELLS = [c for c in CELLS if sum(abs(t) for t in c) == 2]


def _cubie_name(cell):
    """Canonical name, ordered U/D first then F/B then R/L (URF, UF, FR...)."""
    parts = []
    if cell[1] > 0:
        parts.append("U")
    elif cell[1] < 0:
        parts.append("D")
    if cell[2] > 0:
        parts.append("F")
    elif cell[2] < 0:
        parts.append("B")
    if cell[0] > 0:
        parts.append("R")
    elif cell[0] < 0:
        parts.append("L")
    return "".join(parts)


# Canonical solved orderings. Corners are listed U-layer clockwise then
# D-layer clockwise; edges are U-layer, D-layer, then the E-slice.
CORNER_NAMES = ["URF", "UFL", "ULB", "UBR", "DFR", "DLF", "DBL", "DRB"]
EDGE_NAMES = ["UR", "UF", "UL", "UB", "DR", "DF", "DL", "DB", "FR", "FL", "BL", "BR"]


def _cell_for(name):
    v = [0, 0, 0]
    for ch in name:
        a = AXES[ch]
        v = [v[i] + a[i] for i in range(3)]
    return tuple(v)


CORNER_CELL = [_cell_for(n) for n in CORNER_NAMES]
EDGE_CELL = [_cell_for(n) for n in EDGE_NAMES]

CORNER_INDEX = {c: i for i, c in enumerate(CORNER_CELL)}
EDGE_INDEX = {c: i for i, c in enumerate(EDGE_CELL)}


def _corner_axes(name):
    """Sticker normals of a solved corner, ordered so that index 0 is the U/D
    sticker. Orientation counts clockwise twists away from that reference."""
    axes = [AXES[ch] for ch in name]
    # name is built U/D first by construction of CORNER_NAMES
    return axes


def _edge_axes(name):
    return [AXES[ch] for ch in name]


CORNER_AXES = [_corner_axes(n) for n in CORNER_NAMES]
EDGE_AXES = [_edge_axes(n) for n in EDGE_NAMES]


# ---------------------------------------------------------------------------
# Move derivation
# ---------------------------------------------------------------------------

BASE_FACES = ["U", "R", "F", "D", "L", "B"]


def _corner_move(face):
    """Return (perm, ori) for one clockwise turn of `face`.

    perm[i] = index of the corner that ENDS UP at slot i.
    ori[i]  = orientation added, mod 3.
    """
    axis = AXES[face]
    perm = list(range(8))
    ori = [0] * 8
    for src in range(8):
        cell = CORNER_CELL[src]
        if dot(cell, axis) != 1:
            continue
        new_cell = rot90(cell, axis, 1)
        dst = CORNER_INDEX[new_cell]

        # Where does the source cubie's U/D sticker end up?
        ud_normal_after = rot90(CORNER_AXES[src][0], axis, 1)
        # Reference frame of the destination slot, clockwise from its U/D axis.
        ref = _clockwise_corner_frame(dst)
        ori[dst] = ref.index(ud_normal_after)
        perm[dst] = src
    return perm, ori


def _clockwise_corner_frame(slot):
    """The three sticker normals of a corner slot, ordered clockwise when
    viewed from outside the corner, starting at the U/D normal."""
    cell = CORNER_CELL[slot]
    ud = (0, 1, 0) if cell[1] > 0 else (0, -1, 0)
    others = [AXES[ch] for ch in CORNER_NAMES[slot] if AXES[ch] != ud]
    a, b = others
    # Clockwise seen from outside means (ud, a, b) is a left-handed triple.
    cross = (
        ud[1] * a[2] - ud[2] * a[1],
        ud[2] * a[0] - ud[0] * a[2],
        ud[0] * a[1] - ud[1] * a[0],
    )
    if dot(cross, b) > 0:
        return [ud, a, b]
    return [ud, b, a]


def _edge_move(face):
    """Return (perm, ori) for one clockwise turn of `face`."""
    axis = AXES[face]
    perm = list(range(12))
    ori = [0] * 12
    for src in range(12):
        cell = EDGE_CELL[src]
        if dot(cell, axis) != 1:
            continue
        new_cell = rot90(cell, axis, 1)
        dst = EDGE_INDEX[new_cell]

        primary_after = rot90(EDGE_AXES[src][0], axis, 1)
        ref = EDGE_AXES[dst]
        ori[dst] = 0 if primary_after == ref[0] else 1
        perm[dst] = src
    return perm, ori


CORNER_MOVE = {f: _corner_move(f) for f in BASE_FACES}
EDGE_MOVE = {f: _edge_move(f) for f in BASE_FACES}


# ---------------------------------------------------------------------------
# Facelets
# ---------------------------------------------------------------------------
# Facelet index = face_index * 9 + row * 3 + col, matching the firmware's
# g_cube[54] layout and the standard URFDLB reading order.

def _facelet_table():
    """List of (cell, normal) for all 54 stickers in URFDLB order."""
    out = []
    for face in FACE_ORDER:
        n = AXES[face]
        # Build the 3x3 reading frame for this face.
        if face == "U":
            row_dir, col_dir = (0, 0, 1), (1, 0, 0)
        elif face == "D":
            row_dir, col_dir = (0, 0, -1), (1, 0, 0)
        elif face == "F":
            row_dir, col_dir = (0, -1, 0), (1, 0, 0)
        elif face == "B":
            row_dir, col_dir = (0, -1, 0), (-1, 0, 0)
        elif face == "R":
            row_dir, col_dir = (0, -1, 0), (0, 0, -1)
        else:  # L
            row_dir, col_dir = (0, -1, 0), (0, 0, 1)
        for r in (-1, 0, 1):
            for c in (-1, 0, 1):
                cell = tuple(
                    n[i] + r * row_dir[i] + c * col_dir[i] for i in range(3)
                )
                out.append((cell, n))
    return out


FACELETS = _facelet_table()
FACELET_INDEX = {fl: i for i, fl in enumerate(FACELETS)}

assert len(FACELETS) == 54, len(FACELETS)
assert len(FACELET_INDEX) == 54


def facelet_permutation(face):
    """perm[i] = facelet index whose sticker moves INTO slot i on a CW turn."""
    axis = AXES[face]
    perm = list(range(54))
    for src, (cell, normal) in enumerate(FACELETS):
        if dot(cell, axis) != 1:
            continue
        dst = FACELET_INDEX[(rot90(cell, axis, 1), rot90(normal, axis, 1))]
        perm[dst] = src
    return perm


FACELET_MOVE = {f: facelet_permutation(f) for f in BASE_FACES}
