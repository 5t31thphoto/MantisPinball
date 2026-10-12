"""
Emitters: turn generated resources into flash-resident C++.

The endgame tablebase needs a state -> class-index map. Storing the 26,098
twenty-byte class keys would cost 522 KB, which defeats the whole exercise.
Instead we use a minimal perfect hash (CHD) and store NO keys at all.

That is only safe because lookup is self-verifying. An out-of-ball state hashes
to some arbitrary index and gets a wrong move, but following the chain fails to
reach solved within `depth` steps, so it is rejected. In-ball states are always
correct because the hash is perfect on the key set. Dropping key storage is the
single biggest win here, and it is available to us only because we are not
required to answer for states outside the ball.
"""

from __future__ import annotations

import numpy as np
from typing import Dict, List, Sequence, Tuple


# ---------------------------------------------------------------------------
# CHD minimal perfect hash
# ---------------------------------------------------------------------------

MASK64 = (1 << 64) - 1
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a(key: bytes, seed: int = 0) -> int:
    h = (FNV_OFFSET ^ seed) & MASK64
    for b in key:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def _next_prime(n: int) -> int:
    def is_p(x):
        if x < 2: return False
        if x % 2 == 0: return x == 2
        i = 3
        while i * i <= x:
            if x % i == 0: return False
            i += 2
        return True
    while not is_p(n):
        n += 1
    return n


def build_chd(keys, lam: float = 4.0, slack: float = 1.05, log=print):
    """Compress-hash-displace minimal perfect hash.

    Classic formulation: slot = (h1 + d * h2) mod n, so searching a bucket's
    displacement is arithmetic rather than re-hashing. Displacements are
    uint16, which is what lets a bucket of ten keys still find a free slot
    when the table is nearly full.

    No keys are stored. Out-of-ball states collide onto arbitrary slots and
    are caught by the self-verifying chain.
    """
    n = len(keys)
    # Prime slot count with a little slack. Prime guarantees gcd(h2, n_slots)
    # is 1, so the displacement sequence h1 + d*h2 sweeps every slot; the
    # slack keeps the tail buckets from starving.
    n_slots = _next_prime(int(n * slack))
    n_buckets = max(1, int(n / lam))

    h1 = np.empty(n, dtype=np.uint64)
    h2 = np.empty(n, dtype=np.uint64)
    bucket_of = np.empty(n, dtype=np.int64)
    for i, k in enumerate(keys):
        a = fnv1a(k, 0x9E3779B97F4A7C15)
        b = fnv1a(k, 0xC2B2AE3D27D4EB4F)
        h1[i] = a
        h2[i] = b % n_slots
        if h2[i] == 0:
            h2[i] = 1
        bucket_of[i] = a % n_buckets

    order_idx = np.argsort(bucket_of, kind="stable")
    starts = np.searchsorted(bucket_of[order_idx], np.arange(n_buckets))
    ends = np.searchsorted(bucket_of[order_idx], np.arange(n_buckets),
                           side="right")
    sizes = ends - starts
    bucket_order = np.argsort(-sizes, kind="stable")

    disp = np.zeros(n_buckets, dtype=np.uint16)
    taken = np.zeros(n_slots, dtype=bool)
    slot_of = np.full(n, -1, dtype=np.int64)

    MAXD = 65535
    for bi in bucket_order:
        members = order_idx[starts[bi]:ends[bi]]
        if members.size == 0:
            continue
        m1 = h1[members]
        m2 = h2[members]
        placed = False
        for d in range(MAXD + 1):
            slots = ((m1 + np.uint64(d) * m2) % np.uint64(n_slots)).astype(np.int64)
            if np.unique(slots).size != slots.size:
                continue
            if taken[slots].any():
                continue
            taken[slots] = True
            slot_of[members] = slots
            disp[bi] = d
            placed = True
            break
        if not placed:
            if slack < 1.5:
                log(f"  CHD retry with more slack ({slack:.2f} -> {slack+0.1:.2f})")
                return build_chd(keys, lam=lam, slack=slack + 0.1, log=log)
            raise RuntimeError(f"CHD failed on bucket {bi} size {members.size}")

    assert (slot_of >= 0).all()
    log(f"  CHD: {n:,} keys -> {n_slots:,} slots, {n_buckets:,} buckets, "
        f"{disp.nbytes/1024:.1f} KB displacements, max disp {int(disp.max())}")
    return disp, n_buckets, n_slots, slot_of


# ---------------------------------------------------------------------------
# Bit packing
# ---------------------------------------------------------------------------

def pack_bits(values: Sequence[int], bits: int) -> np.ndarray:
    out = bytearray()
    acc = 0
    nacc = 0
    for v in values:
        acc |= (int(v) & ((1 << bits) - 1)) << nacc
        nacc += bits
        while nacc >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nacc -= 8
    if nacc:
        out.append(acc & 0xFF)
    return np.frombuffer(bytes(out), dtype=np.uint8)


def _c_array(name: str, data: np.ndarray, per_line: int = 20) -> str:
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        lines.append("  " + ",".join(str(int(v)) for v in data[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Endgame header
# ---------------------------------------------------------------------------

ENDGAME_PREAMBLE = """// GENERATED - do not edit. MantisCubeSolver endgame tablebase.
//
// Exact optimal moves for every state within {depth} moves of solved,
// folded by the 24 rotational symmetries and indexed by a minimal perfect
// hash. No class keys are stored: lookup is self-verifying, so an
// out-of-ball state simply fails to reach solved and is rejected.
#pragma once
#include <stdint.h>

#define MC_ENDGAME_DEPTH   {depth}
#define MC_ENDGAME_CLASSES {classes}
#define MC_ENDGAME_BUCKETS {buckets}
#define MC_ENDGAME_SLOTS   {slots}

"""


def emit_endgame(path: str, class_keys: np.ndarray, moves: np.ndarray,
                 sym_move: np.ndarray, depth: int, log=print) -> int:
    keys = [k.tobytes() for k in class_keys]
    disp, n_buckets, n_slots, slot_of = build_chd(keys, log=log)

    slot_moves = np.full(n_slots, 31, dtype=np.uint8)
    for i, s in enumerate(slot_of):
        slot_moves[s] = moves[i] if moves[i] < 18 else 31

    packed = pack_bits(slot_moves, 5)

    text = ENDGAME_PREAMBLE.format(depth=depth, classes=len(keys),
                                   buckets=n_buckets, slots=n_slots)
    text += f"static const uint16_t MC_ENDGAME_DISP[{len(disp)}] = {{\n"
    for i in range(0, len(disp), 16):
        text += "  " + ",".join(str(int(v)) for v in disp[i:i+16]) + ",\n"
    text += "};\n\n"
    text += _c_array("MC_ENDGAME_MOVES", packed) + "\n\n"

    text += f"static const uint8_t MC_SYM_MOVE[{sym_move.shape[0]}][18] = {{\n"
    for row in sym_move:
        text += "  {" + ",".join(str(int(v)) for v in row) + "},\n"
    text += "};\n\n"

    with open(path, "w") as fh:
        fh.write(text)

    total = disp.nbytes + packed.nbytes + sym_move.size
    log(f"  wrote {path}: {total/1024:.1f} KB "
        f"({disp.nbytes/1024:.1f} hash + {packed.nbytes/1024:.1f} moves)")
    return total


# ---------------------------------------------------------------------------
# Symmetry conjugation header (needed to canonicalise on device)
# ---------------------------------------------------------------------------

def emit_symmetry(path: str, syms, sym_inv, log=print) -> int:
    def rows(objs, field, n):
        return np.array([getattr(o, field) for o in objs], dtype=np.uint8)

    text = ("// GENERATED - do not edit. MantisCubeSolver symmetry tables.\n"
            "#pragma once\n#include <stdint.h>\n\n")
    text += f"#define MC_NSYM {len(syms)}\n\n"
    total = 0
    for nm, objs in (("S", syms), ("SINV", sym_inv)):
        for field, n in (("cp", 8), ("co", 8), ("ep", 12), ("eo", 12)):
            arr = rows(objs, field, n)
            text += (f"static const uint8_t MC_{nm}_{field.upper()}"
                     f"[{len(objs)}][{n}] = {{\n")
            for row in arr:
                text += "  {" + ",".join(str(int(v)) for v in row) + "},\n"
            text += "};\n\n"
            total += arr.size
    with open(path, "w") as fh:
        fh.write(text)
    log(f"  wrote {path}: {total/1024:.1f} KB")
    return total


# ---------------------------------------------------------------------------
# Model weights
# ---------------------------------------------------------------------------

MODEL_PREAMBLE = """// GENERATED - do not edit. MantisCubeSolver policy network.
//
// Quantised per-layer at the bit depths chosen in SolverConfig.quant.
// Weights are streamed sequentially on every forward pass, which is the
// access pattern flash is good at - unlike the single random lookup into a
// megabyte-scale pruning table that this design exists to avoid.
#pragma once
#include <stdint.h>

#define MC_EMBED_DIM   {embed_dim}
#define MC_N_SLOTS     {n_slots}
#define MC_N_VALUES    {n_values}
#define MC_FEATURE_DIM {feature_dim}
#define MC_N_MOVES     {n_moves}
#define MC_N_HIDDEN    {n_hidden}
#define MC_EMBED_BITS  {embed_bits}
#define MC_HIDDEN_BITS {hidden_bits}
#define MC_OUTPUT_BITS {output_bits}

"""


def channel_quant(w: np.ndarray, bits: int):
    """Per-output-channel symmetric quantisation.

    Returns (int weights, relative channel multipliers as uint16).

    The multipliers are stored relative to the largest channel scale, so
    inference stays in integer arithmetic: y[o] = (acc[o] * q[o]) >> 15.
    That keeps per-channel accuracy - worth real points at 4 bits - without
    ever needing a float multiply or an activation calibration pass that
    could drift between the host and the device.
    """
    qmax = (1 << (bits - 1)) - 1
    scale = np.abs(w).max(axis=1) / qmax
    scale = np.maximum(scale, 1e-12)
    qw = np.clip(np.round(w / scale[:, None]), -qmax - 1, qmax).astype(np.int32)
    rel = scale / scale.max()
    q = np.clip(np.round(rel * 32767.0), 1, 32767).astype(np.uint16)
    return qw, q


def emit_model(path: str, cfg, tensors, scales=None, log=print) -> int:
    """Emit the quantised policy.

    `tensors` holds float weights; quantisation happens here so the host
    reference and the device read identical integers. Any caller passing
    pre-quantised integers is a bug - preflight checks for it.
    """
    r, m, q = cfg.representation, cfg.model, cfg.quant
    text = MODEL_PREAMBLE.format(
        embed_dim=r.embed_dim, n_slots=r.n_slots, n_values=r.n_values,
        feature_dim=r.feature_dim, n_moves=m.n_moves, n_hidden=len(m.hidden),
        embed_bits=q.embed_bits, hidden_bits=q.hidden_bits,
        output_bits=q.output_bits)

    total = 0

    # Embeddings: single tensor scale. They feed a linear layer, so a uniform
    # scale cancels exactly under ReLU homogeneity and needs no multiplier.
    emb = np.asarray(tensors["embed"], dtype=np.float64)
    eqmax = (1 << (q.embed_bits - 1)) - 1
    escale = max(float(np.abs(emb).max()) / eqmax, 1e-12)
    qemb = np.clip(np.round(emb / escale), -eqmax - 1, eqmax).astype(np.int32)
    vals = (qemb.reshape(-1) + (1 << (q.embed_bits - 1))) & ((1 << q.embed_bits) - 1)
    packed = pack_bits(vals, q.embed_bits)
    text += f"// embed: {qemb.size} values at {q.embed_bits} bits\n"
    text += _c_array("MC_W_EMBED", packed) + "\n\n"
    total += packed.nbytes

    hidden_names = sorted([k for k in tensors if k.startswith("h")],
                          key=lambda k: int(k[1:]))
    shapes = []
    for name in hidden_names:
        w = np.asarray(tensors[name], dtype=np.float64)
        qw, qmul = channel_quant(w, q.hidden_bits)
        shapes.append(qw.shape)
        vals = (qw.reshape(-1) + (1 << (q.hidden_bits - 1))) & ((1 << q.hidden_bits) - 1)
        packed = pack_bits(vals, q.hidden_bits)
        text += f"// {name}: {qw.shape} at {q.hidden_bits} bits\n"
        text += _c_array(f"MC_W_{name.upper()}", packed) + "\n\n"
        text += f"static const uint16_t MC_Q_{name.upper()}[{qmul.size}] = {{\n"
        for i in range(0, qmul.size, 16):
            text += "  " + ",".join(str(int(v)) for v in qmul[i:i+16]) + ",\n"
        text += "};\n\n"
        total += packed.nbytes + qmul.nbytes

    w = np.asarray(tensors["out"], dtype=np.float64)
    qw, qmul = channel_quant(w, q.output_bits)
    vals = (qw.reshape(-1) + (1 << (q.output_bits - 1))) & ((1 << q.output_bits) - 1)
    packed = pack_bits(vals, q.output_bits)
    text += f"// out: {qw.shape} at {q.output_bits} bits\n"
    text += _c_array("MC_W_OUT", packed) + "\n\n"
    text += f"static const uint16_t MC_Q_OUT[{qmul.size}] = {{\n"
    text += "  " + ",".join(str(int(v)) for v in qmul) + ",\n};\n\n"
    total += packed.nbytes + qmul.nbytes

    if hidden_names:
        text += ("static const uint8_t* const MC_W_HIDDEN[%d] = {%s};\n\n"
                 % (len(hidden_names),
                    ",".join("MC_W_" + n.upper() for n in hidden_names)))
        text += ("static const uint16_t* const MC_Q_HIDDEN[%d] = {%s};\n\n"
                 % (len(hidden_names),
                    ",".join("MC_Q_" + n.upper() for n in hidden_names)))
        text += ("static const uint16_t MC_HIDDEN_IN[%d] = {%s};\n\n"
                 % (len(hidden_names), ",".join(str(s[1]) for s in shapes)))
        text += ("static const uint16_t MC_HIDDEN_OUT[%d] = {%s};\n\n"
                 % (len(hidden_names), ",".join(str(s[0]) for s in shapes)))

    with open(path, "w") as fh:
        fh.write(text)
    log(f"  wrote {path}: {total/1024:.1f} KB")
    return total
