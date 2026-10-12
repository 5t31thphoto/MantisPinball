"""
Bit-exact host reference for mcPolicy() in cube_policy.cpp.

This exists so the training pipeline can prove, before a GPU run starts, that
the weights it is about to produce will be evaluated on the device exactly as
they are evaluated here. Without it, a quantisation or layout mismatch shows
up only as a model that trains beautifully and solves nothing.

Every arithmetic step mirrors the C++: integer dot products, per-channel
multiplier applied as (acc * q) >> 15, ReLU, uniform renormalising shift.
"""

from __future__ import annotations

import numpy as np

from .emit import channel_quant


def slot_values_single(state: np.ndarray) -> np.ndarray:
    """Mirror of mcSlotValue() for one packed 20-byte state."""
    out = np.empty(20, dtype=np.int64)
    for i in range(8):
        out[i] = (state[i] & 7) * 3 + ((state[i] >> 3) & 3)
    for i in range(12):
        out[8 + i] = (state[8 + i] & 15) * 2 + ((state[8 + i] >> 4) & 1)
    return out


def quantise_all(cfg, tensors):
    """Quantise exactly as emit_model does, so the reference and the emitted
    header are derived from one code path."""
    q = cfg.quant
    emb = np.asarray(tensors["embed"], dtype=np.float64)
    eqmax = (1 << (q.embed_bits - 1)) - 1
    escale = max(float(np.abs(emb).max()) / eqmax, 1e-12)
    qemb = np.clip(np.round(emb / escale), -eqmax - 1, eqmax).astype(np.int64)

    hidden = []
    for name in sorted([k for k in tensors if k.startswith("h")],
                       key=lambda k: int(k[1:])):
        w = np.asarray(tensors[name], dtype=np.float64)
        qw, qm = channel_quant(w, q.hidden_bits)
        hidden.append((qw.astype(np.int64), qm.astype(np.int64)))

    ow, om = channel_quant(np.asarray(tensors["out"], dtype=np.float64),
                           q.output_bits)
    return qemb, hidden, (ow.astype(np.int64), om.astype(np.int64))


def renorm_shift(v: np.ndarray) -> int:
    mx = int(v.max()) if v.size else 0
    if mx <= 0:
        return 0
    sh = 0
    while (mx >> sh) > 32767:
        sh += 1
    return sh


def policy_logits(cfg, quantised, state: np.ndarray) -> np.ndarray:
    """Integer forward pass identical to mcPolicy()."""
    qemb, hidden, (ow, om) = quantised
    r = cfg.representation

    slots = slot_values_single(state)
    tbl = ([i for i in range(r.n_slots)] if r.slot_specific
           else [0 if i < 8 else 1 for i in range(r.n_slots)])
    act = np.concatenate([qemb[tbl[i], slots[i]] for i in range(r.n_slots)])
    act = act.astype(np.int64)

    for qw, qm in hidden:
        acc = qw @ act                      # (out, in) @ (in,)
        y = (acc * qm) >> 15
        y = np.maximum(y, 0)
        sh = renorm_shift(y)
        if sh:
            y = y >> sh
        act = y

    acc = ow @ act
    return (acc * om) >> 15


def compare_with_device(cfg, tensors, states, binary: str, log=print) -> bool:
    """Run the host reference and the compiled device code on the same states
    and require identical logits."""
    import subprocess

    quantised = quantise_all(cfg, tensors)
    bad = 0
    for scramble, state in states:
        ref = policy_logits(cfg, quantised, state)
        proc = subprocess.run([binary] + [str(m) for m in scramble],
                              capture_output=True, text=True)
        got = np.array([int(x) for x in proc.stdout.split()], dtype=np.int64)
        if got.shape != ref.shape or not (got == ref).all():
            bad += 1
            if bad == 1:
                log(f"  MISMATCH on scramble {scramble}")
                log(f"    reference: {ref.tolist()}")
                log(f"    device   : {got.tolist()}")
    if bad:
        log(f"  {bad}/{len(states)} states disagree")
        return False
    log(f"  host reference matches device on {len(states)} states: OK")
    return True
