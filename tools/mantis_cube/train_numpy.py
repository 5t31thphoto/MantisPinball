#!/usr/bin/env python3
"""Train the MantisCubeSolver policy with numpy only (no torch, no GPU).

Trains EXACTLY the network the device runs (cube_policy.cpp / policy_ref.py):
  slot-specific embeddings (20 x 24 x D) -> [Linear, ReLU] x H -> Linear(18)
with NO biases. (The torch PolicyNet uses nn.Linear's default bias=True, but
emit_model exports weights only and the device has no bias term, so a torch
model is evaluated on the board without biases it learned to rely on. This
trainer cannot have that mismatch.)

Data: the package's own canonical scrambles, labelled with the inverse of the
last move, with exact tablebase labels substituted for shallow states
(train.BatchSource). Loss: softmax cross-entropy (+ light label smoothing).

Phases:
  float  Adam, cosine LR
  qat    fake-quantised forward at the device bit widths (embed 8-bit global,
         hidden 4-bit per row, output 8-bit per row - emit.channel_quant),
         straight-through gradients into the float weights

Checkpoints (npz) let a run resume: --resume path.

    python -m mantis_cube.train_numpy --preset balanced --minutes 60 \
        --out <firmware dir> [--ckpt ckpt.npz]
"""
from __future__ import annotations
import argparse, json, os, sys, time
import numpy as np

from .config import PRESETS
from . import train as T                 # torch-free parts only
from . import emit as EM


# ------------------------------------------------------------------- model
def init_params(cfg, rng):
    r, m = cfg.representation, cfg.model
    D = r.embed_dim
    n_tab = r.n_slots if r.slot_specific else 2
    P = {"embed": rng.normal(0, 0.5, size=(n_tab, r.n_values, D))}
    prev = r.n_slots * D
    for i, h in enumerate(m.hidden):
        P[f"h{i}"] = rng.normal(0, np.sqrt(2.0 / prev), size=(h, prev))
        prev = h
    P["out"] = rng.normal(0, np.sqrt(1.0 / prev), size=(m.n_moves, prev))
    return P


def fake_quant_rows(w, bits):
    qmax = (1 << (bits - 1)) - 1
    s = np.maximum(np.abs(w).max(axis=1, keepdims=True) / qmax, 1e-12)
    return np.clip(np.round(w / s), -qmax - 1, qmax) * s


def fake_quant_global(w, bits):
    qmax = (1 << (bits - 1)) - 1
    s = max(float(np.abs(w).max()) / qmax, 1e-12)
    return np.clip(np.round(w / s), -qmax - 1, qmax) * s


def effective(P, cfg, qat):
    if not qat:
        return P
    q = cfg.quant
    E = {"embed": fake_quant_global(P["embed"], q.embed_bits),
         "out": fake_quant_rows(P["out"], q.output_bits)}
    for k in P:
        if k.startswith("h"):
            E[k] = fake_quant_rows(P[k], q.hidden_bits)
    return E


def forward(W, cfg, slots):
    """slots (B,20) -> cache, logits (B,18)."""
    r = cfg.representation
    B = slots.shape[0]
    tbl = np.arange(r.n_slots) if r.slot_specific else (np.arange(r.n_slots) >= 8).astype(int)
    x = W["embed"][tbl[None, :], slots].reshape(B, -1)
    acts = [x]
    nh = sum(1 for k in W if k.startswith("h"))
    for i in range(nh):
        x = np.maximum(x @ W[f"h{i}"].T, 0.0)
        acts.append(x)
    return acts, x @ W["out"].T, tbl


def loss_and_grads(W, cfg, slots, labels, smooth=0.05):
    acts, z, tbl = forward(W, cfg, slots)
    B, K = z.shape
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z); p = e / e.sum(axis=1, keepdims=True)
    tgt = np.full((B, K), smooth / K); tgt[np.arange(B), labels] += 1.0 - smooth
    loss = float(-(tgt * np.log(p + 1e-12)).sum(axis=1).mean())
    acc = float((p.argmax(1) == labels).mean())
    g = (p - tgt) / B
    G = {"out": g.T @ acts[-1]}
    gx = g @ W["out"]
    nh = len(acts) - 1
    for i in reversed(range(nh)):
        gx = gx * (acts[i + 1] > 0)
        G[f"h{i}"] = gx.T @ acts[i]
        gx = gx @ W[f"h{i}"]
    # embedding gradient: scatter by (slot table, value)
    r = cfg.representation
    D = r.embed_dim
    gx = gx.reshape(B, r.n_slots, D)
    gE = np.zeros_like(W["embed"])
    oh = np.zeros((B, r.n_slots, r.n_values))
    oh[np.arange(B)[:, None], np.arange(r.n_slots)[None, :], slots] = 1.0
    per_slot = np.einsum("bsv,bsd->svd", oh, gx)          # (20, 24, D)
    np.add.at(gE, tbl, per_slot)
    G["embed"] = gE
    return loss, acc, G


class Adam:
    def __init__(self, P, b1=0.9, b2=0.999, eps=1e-8):
        self.m = {k: np.zeros_like(v) for k, v in P.items()}
        self.v = {k: np.zeros_like(v) for k, v in P.items()}
        self.t = 0; self.b1, self.b2, self.eps = b1, b2, eps

    def step(self, P, G, lr):
        self.t += 1
        c1 = 1 - self.b1 ** self.t; c2 = 1 - self.b2 ** self.t
        for k in P:
            self.m[k] = self.b1 * self.m[k] + (1 - self.b1) * G[k]
            self.v[k] = self.b2 * self.v[k] + (1 - self.b2) * G[k] ** 2
            P[k] -= lr * (self.m[k] / c1) / (np.sqrt(self.v[k] / c2) + self.eps)


# ------------------------------------------------------------------- eval
def quantised_accuracy(cfg, P, slots_states, labels):
    """Top-1 accuracy of the INTEGER device forward pass (policy_ref)."""
    from . import policy_ref as PR
    qt = PR.quantise_all(cfg, P)
    hit = 0
    for st, lab in zip(slots_states, labels):
        hit += int(PR.policy_logits(cfg, qt, st).argmax() == lab)
    return hit / len(labels)


def save(path, P, opt, meta):
    # write-then-rename: a run cut off mid-save must never corrupt the checkpoint
    tmp = path + ".tmp.npz"
    np.savez(tmp, **{f"P_{k}": v for k, v in P.items()},
             **{f"m_{k}": v for k, v in opt.m.items()},
             **{f"v_{k}": v for k, v in opt.v.items()},
             meta=json.dumps(meta), t=opt.t)
    os.replace(tmp, path)


def load(path, cfg):
    d = np.load(path, allow_pickle=False)
    P = {k[2:]: d[k] for k in d.files if k.startswith("P_")}
    opt = Adam(P)
    for k in P:
        opt.m[k] = d["m_" + k]; opt.v[k] = d["v_" + k]
    opt.t = int(d["t"])
    return P, opt, json.loads(str(d["meta"]))


# ------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="balanced")
    ap.add_argument("--minutes", type=float, default=60.0, help="total wall time")
    ap.add_argument("--qat-frac", type=float, default=0.25, help="fraction spent in QAT")
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--chunk", type=int, default=65536, help="fresh samples per data chunk")
    ap.add_argument("--passes", type=int, default=2, help="passes over each chunk")
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--ckpt", default="cube_ckpt.npz")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--out", default=None, help="dir to write cube_model.h at the end")
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args(argv)

    cfg = PRESETS[a.preset]
    rng = np.random.default_rng(a.seed)
    log = lambda s: print(s, flush=True)

    if a.resume and os.path.exists(a.ckpt):
        P, opt, meta = load(a.ckpt, cfg)
        log(f"resumed {a.ckpt}: {meta}")
    else:
        P = init_params(cfg, rng); opt = Adam(P)
        meta = {"elapsed": 0.0, "samples": 0, "steps": 0}

    log("[data] building tablebase labels ...")
    src = T.BatchSource(cfg, depth=4, log=lambda m: None)
    vs, vl, vd = src.batch(4000, np.random.default_rng(99))       # fixed held-out set
    vslots = T.slot_values(vs)

    budget = a.minutes * 60.0
    qat_start = budget * (1.0 - a.qat_frac)
    t0 = time.time() - meta["elapsed"]
    last_rep = 0.0
    while True:
        el = time.time() - t0
        if el >= budget:
            break
        states, labels, _ = src.batch(a.chunk, rng)
        slots = T.slot_values(states)
        for _ in range(a.passes):
            perm = rng.permutation(len(labels))
            for b in range(0, len(perm) - a.batch + 1, a.batch):
                el = time.time() - t0
                if el >= budget:
                    break
                qat = el >= qat_start
                if qat:
                    frac = (el - qat_start) / max(budget - qat_start, 1)
                    lr = 3e-4 * (0.1 ** frac)
                else:
                    frac = el / max(qat_start, 1)
                    lr = 1e-4 + 0.5 * (a.lr - 1e-4) * (1 + np.cos(np.pi * frac))
                idx = perm[b:b + a.batch]
                W = effective(P, cfg, qat)
                loss, acc, G = loss_and_grads(W, cfg, slots[idx], labels[idx])
                opt.step(P, G, lr)
                meta["steps"] += 1; meta["samples"] += len(idx)
                if el - last_rep > 60:
                    last_rep = el
                    _, z, _ = forward(effective(P, cfg, qat), cfg, vslots)
                    va = float((z.argmax(1) == vl).mean())
                    deep = vd >= 12
                    vdeep = float((z.argmax(1)[deep] == vl[deep]).mean())
                    meta["elapsed"] = el
                    log(f"  {el/60:5.1f}m {'QAT ' if qat else 'FLT '} step {meta['steps']:>7} "
                        f"samples {meta['samples']/1e6:6.2f}M  lr {lr:.2e}  "
                        f"loss {loss:.3f} acc {acc:.3f} | held-out {va:.3f} (depth>=12: {vdeep:.3f})")
                    save(a.ckpt, P, opt, meta)
            else:
                continue
            break

    meta["elapsed"] = time.time() - t0
    save(a.ckpt, P, opt, meta)
    qa = quantised_accuracy(cfg, P, vs[:1000], vl[:1000])
    log(f"[done] {meta['samples']/1e6:.1f}M samples, {meta['steps']} steps. "
        f"Device-integer top-1 on held-out: {qa:.3f}")
    if a.out:
        tensors = {k: P[k].astype(np.float64) for k in P}
        EM.emit_model(os.path.join(a.out, "cube_model.h"), cfg, tensors, log=log)
        log(f"[emit] wrote {os.path.join(a.out, 'cube_model.h')}")


if __name__ == "__main__":
    main()
