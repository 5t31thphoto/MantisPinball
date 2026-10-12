#!/usr/bin/env python3
"""GPU trainer for the MantisCubeSolver policy (e.g. an RTX 5050, 8 GB).

Same network as the device and as train_numpy.py: slot embeddings -> bias-free
[Linear, ReLU] x H -> Linear(18). Scrambles are generated ON THE GPU (a move
is a gather by a position permutation plus an orientation add), so the GPU is
never waiting on Python.

IT WILL NOT TRAIN UNTIL IT HAS PROVEN ITSELF on your machine. On start it
checks, against the numpy reference (itself verified against the compiled
device code):
  1. GPU move application == endgame.apply_move, every move, random states
  2. GPU slot encoding     == train.slot_values (== mcSlotValue on device)
  3. GPU forward pass      == train_numpy.forward, float and fake-quantised
Any mismatch aborts before a single step.

Typical runs
  # continue from the CPU-trained weights, same size as shipped:
  python -m mantis_cube.train_gpu --init-npz mantis_cube/trained_ckpt.npz \
      --hours 3 --out ../src/rubiks
  # try a bigger network (slower on the S3 - measure it there):
  python -m mantis_cube.train_gpu --hidden 384,384 --hours 4 --out ../src/rubiks

Requires: pip install torch numpy   (CUDA build of torch for the GPU)
"""
from __future__ import annotations
import argparse, dataclasses, os, sys, time
import numpy as np

from .config import PRESETS
from . import endgame as EG
from . import train as T
from . import train_numpy as TN
from . import emit as EM


def _torch():
    try:
        import torch
        return torch
    except ImportError:
        sys.exit("torch is not installed: pip install torch (CUDA build for the GPU)")


# --------------------------------------------------------------- cube on GPU
class GpuCube:
    def __init__(self, torch, dev):
        self.t, self.dev = torch, dev
        L = lambda a: torch.tensor(np.asarray(a, dtype=np.int64), device=dev)
        self.MCP, self.MCO = L(EG.MCP), L(EG.MCO)
        self.MEP, self.MEO = L(EG.MEP), L(EG.MEO)
        self.SOLVED = L(EG.SOLVED_ROW.reshape(1, 20))
        self.INV = L(T._INV_MOVE)

    def apply(self, s, m):
        """s (B,20) int64, m (B,) int64 - exactly endgame.apply_move."""
        t = self.t
        c, e = s[:, :8], s[:, 8:]
        src_c = t.gather(c, 1, self.MCP[m])
        oc = ((src_c >> 3) & 3) + self.MCO[m]
        oc = t.where(oc >= 3, oc - 3, oc)
        src_e = t.gather(e, 1, self.MEP[m])
        oe = ((src_e >> 4) & 1) ^ self.MEO[m]
        return t.cat([(src_c & 7) | (oc << 3), (src_e & 15) | (oe << 4)], 1)

    def canonical_ok(self, prev, m):
        """train._canonical_ok, vectorised."""
        fa, fb = prev // 3, m // 3
        bad = (prev >= 0) & ((fa == fb) | (((fa + 3) % 6 == fb) & (fa > fb)))
        return ~bad

    def scrambles(self, n, max_depth, gen):
        """train.generate_scrambles on the GPU: (states, labels)."""
        t = self.t
        depths = t.randint(1, max_depth + 1, (n,), device=self.dev, generator=gen)
        s = self.SOLVED.repeat(n, 1)
        prev = t.full((n,), -1, device=self.dev, dtype=t.int64)
        last = prev.clone()
        for step in range(1, max_depth + 1):
            active = depths >= step
            m = t.randint(0, 18, (n,), device=self.dev, generator=gen)
            for _ in range(8):
                bad = active & ~self.canonical_ok(prev, m)
                if not bool(bad.any()):
                    break
                m = t.where(bad, t.randint(0, 18, (n,), device=self.dev, generator=gen), m)
            s = t.where(active[:, None], self.apply(s, m), s)
            prev = t.where(active, m, prev)
            last = t.where(active, m, last)
        return s, self.INV[last]

    def slots(self, s):
        c, e = s[:, :8], s[:, 8:]
        return self.t.cat([(c & 7) * 3 + ((c >> 3) & 3), (e & 15) * 2 + ((e >> 4) & 1)], 1)


# --------------------------------------------------------------- the network
def build_net(torch, cfg, dev):
    nn = torch.nn
    r, m, q = cfg.representation, cfg.model, cfg.quant

    def fq_rows(w, bits):
        qmax = (1 << (bits - 1)) - 1
        sc = (w.abs().amax(dim=1, keepdim=True) / qmax).clamp_min(1e-12)
        return w + (torch.clamp(torch.round(w / sc), -qmax - 1, qmax) * sc - w).detach()

    def fq_global(w, bits):
        qmax = (1 << (bits - 1)) - 1
        sc = (w.abs().max() / qmax).clamp_min(1e-12)
        return w + (torch.clamp(torch.round(w / sc), -qmax - 1, qmax) * sc - w).detach()

    class Net(nn.Module):
        def __init__(self):
            super().__init__()
            D = r.embed_dim
            self.embed = nn.Parameter(torch.randn(r.n_slots, r.n_values, D) * 0.5)
            dims, prev = [], r.n_slots * D
            self.hidden = nn.ParameterList()
            for h in m.hidden:
                self.hidden.append(nn.Parameter(torch.randn(h, prev) * (2.0 / prev) ** 0.5))
                prev = h
            self.out = nn.Parameter(torch.randn(m.n_moves, prev) * (1.0 / prev) ** 0.5)
            self.register_buffer("slot_idx", torch.arange(r.n_slots))
            self.qat = False

        def forward(self, slots):
            E = fq_global(self.embed, q.embed_bits) if self.qat else self.embed
            x = E[self.slot_idx[None, :], slots].reshape(slots.shape[0], -1)
            for W in self.hidden:
                Wq = fq_rows(W, q.hidden_bits) if self.qat else W
                x = torch.relu(x @ Wq.t())
            Wo = fq_rows(self.out, q.output_bits) if self.qat else self.out
            return x @ Wo.t()

        def load_numpy(self, P):
            with torch.no_grad():
                self.embed.copy_(torch.tensor(P["embed"]))
                for i, W in enumerate(self.hidden):
                    W.copy_(torch.tensor(P[f"h{i}"]))
                self.out.copy_(torch.tensor(P["out"]))

        def to_numpy(self):
            P = {"embed": self.embed.detach().double().cpu().numpy(),
                 "out": self.out.detach().double().cpu().numpy()}
            for i, W in enumerate(self.hidden):
                P[f"h{i}"] = W.detach().double().cpu().numpy()
            return P

    return Net().to(dev)


# --------------------------------------------------------------- self-checks
def self_check(torch, cfg, dev):
    print("[check] proving the GPU pipeline against the numpy reference ...")
    gc = GpuCube(torch, dev)
    rng = np.random.default_rng(7)
    st, _, _ = T.generate_scrambles(2000, 20, rng)
    for mv in range(18):
        ref = EG.apply_move(st, mv)
        got = gc.apply(torch.tensor(st.astype(np.int64), device=dev),
                       torch.full((len(st),), mv, device=dev, dtype=torch.int64)).cpu().numpy()
        if not np.array_equal(ref.astype(np.int64), got):
            sys.exit(f"[check] FAILED: GPU move {mv} != endgame.apply_move")
    print("  move application: 18/18 moves identical")
    if not np.array_equal(T.slot_values(st), gc.slots(torch.tensor(st.astype(np.int64), device=dev)).cpu().numpy()):
        sys.exit("[check] FAILED: slot encoding")
    print("  slot encoding: identical")
    net = build_net(torch, cfg, dev).double()
    P = TN.init_params(cfg, np.random.default_rng(3))
    net.load_numpy(P)
    sl = T.slot_values(st[:256])
    for qat in (False, True):
        net.qat = qat
        _, ref, _ = TN.forward(TN.effective(P, cfg, qat), cfg, sl)
        got = net(torch.tensor(sl, device=dev)).detach().cpu().numpy()
        err = float(np.abs(ref - got).max() / (np.abs(ref).max() + 1e-12))
        if err > 1e-6:
            sys.exit(f"[check] FAILED: forward ({'qat' if qat else 'float'}) rel err {err:.2e}")
        print(f"  forward pass ({'fake-quant' if qat else 'float'}): rel err {err:.1e}")
    # GPU scrambles: canonical, non-trivial, labels in range
    s, lab = gc.scrambles(4096, cfg.teacher.max_scramble, torch.Generator(device=dev).manual_seed(1))
    assert int(lab.min()) >= 0 and int(lab.max()) < 18
    print("[check] all passed")


# --------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="balanced")
    ap.add_argument("--hidden", default=None, help="e.g. 256,256 (shipped) or 384,384")
    ap.add_argument("--hours", type=float, default=2.0)
    ap.add_argument("--qat-frac", type=float, default=0.2)
    ap.add_argument("--batch", type=int, default=8192)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--init-npz", default=None, help="start from a train_numpy checkpoint")
    ap.add_argument("--ckpt", default="cube_gpu_ckpt.npz", help="saved every few minutes (numpy format)")
    ap.add_argument("--out", default=None, help="write cube_model.h here at the end")
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args(argv)

    torch = _torch()
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"[gpu] device: {dev}" + (f" ({torch.cuda.get_device_name(0)})" if dev == "cuda" else
                                    "  - no CUDA: this will be slow"))
    cfg = PRESETS[a.preset]
    if a.hidden:
        hid = [int(x) for x in a.hidden.split(",")]
        if max(hid) > 512:
            sys.exit("device scratch buffers hold 512 activations; keep layers <= 512")
        cfg = dataclasses.replace(cfg, model=dataclasses.replace(cfg.model, hidden=hid))
    self_check(torch, cfg, dev)

    net = build_net(torch, cfg, dev)
    if a.init_npz:
        P, _, meta = TN.load(a.init_npz, cfg)
        net.load_numpy(P)
        print(f"[gpu] initialised from {a.init_npz} ({meta.get('samples', 0)/1e6:.1f}M samples)")
    opt = torch.optim.Adam(net.parameters(), lr=a.lr)
    gen = torch.Generator(device=dev).manual_seed(a.seed)
    gc = GpuCube(torch, dev)

    # fixed held-out set
    vs, vl = gc.scrambles(16384, cfg.teacher.max_scramble, torch.Generator(device=dev).manual_seed(99))
    vslots = gc.slots(vs)

    budget = a.hours * 3600.0
    qat_start = budget * (1 - a.qat_frac)
    t0 = last = time.time()
    step = samples = 0
    while True:
        el = time.time() - t0
        if el >= budget:
            break
        net.qat = el >= qat_start
        frac = (el - qat_start) / max(budget - qat_start, 1) if net.qat else el / max(qat_start, 1)
        lr = (3e-4 * 0.1 ** frac) if net.qat else (1e-5 + 0.5 * (a.lr - 1e-5) * (1 + np.cos(np.pi * frac)))
        for g in opt.param_groups:
            g["lr"] = lr
        s, lab = gc.scrambles(a.batch, cfg.teacher.max_scramble, gen)
        logits = net(gc.slots(s))
        loss = torch.nn.functional.cross_entropy(logits, lab, label_smoothing=0.05)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        step += 1; samples += a.batch
        if time.time() - last > 120:
            last = time.time()
            with torch.no_grad():
                acc = float((net(vslots).argmax(1) == vl).float().mean())
            print(f"  {el/60:6.1f}m {'QAT' if net.qat else 'FLT'} step {step:>8} "
                  f"samples {samples/1e9:6.3f}B lr {lr:.2e} loss {float(loss):.3f} held-out {acc:.3f}",
                  flush=True)
            P = net.to_numpy()
            TN.save(a.ckpt, P, TN.Adam(P), {"elapsed": el, "samples": samples, "steps": step,
                                             "hidden": list(cfg.model.hidden)})
    P = net.to_numpy()
    TN.save(a.ckpt, P, TN.Adam(P), {"elapsed": time.time() - t0, "samples": samples,
                                     "steps": step, "hidden": list(cfg.model.hidden)})
    print(f"[done] {samples/1e9:.2f}B samples")
    if a.out:
        EM.emit_model(os.path.join(a.out, "cube_model.h"), cfg, P, log=print)
        print(f"[emit] {os.path.join(a.out, 'cube_model.h')} - build, then measure on the board")


if __name__ == "__main__":
    main()
