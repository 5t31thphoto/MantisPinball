"""
MantisCubeSolver — training pipeline.

Runs on your workstation, not the device. Three stages:

  1. Teacher. Large, unconstrained, allowed to cheat: exact tablebase labels,
     wide beam, unlimited compute. Nothing derived from it ships.
  2. Distillation. The student imitates the teacher's full 18-way soft policy,
     not just the argmax. Eighteen numbers of ranking information per example
     instead of one, which is why a student this small can work at all.
  3. Quantisation-aware training. The quantiser is simulated in the forward
     pass so weights learn to live on the grid. Post-hoc quantisation to 4
     bits falls off a cliff; QAT does not.

Data comes from scrambles branching off the goal state, the DeepCubeA /
EfficientCube formulation. No published pruning table is used as a training
signal - only as an oracle in verify.py.

    python -m mantis_cube.train --preset balanced --out firmware
"""

from __future__ import annotations

import argparse
import os
import numpy as np

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    HAVE_TORCH = True
except ImportError:  # pragma: no cover - the device build never needs torch
    HAVE_TORCH = False

from .config import SolverConfig, PRESETS
from . import endgame as EG
from .model import MOVE_NAMES


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

def slot_values(states: np.ndarray) -> np.ndarray:
    """(N, 20) packed states -> (N, 20) categorical slot values.

    Corners take perm*3+ori (24 values), edges perm*2+ori (24 values). This is
    exactly mcSlotValue() in cube_policy.cpp; if the two ever disagree the
    student is being fed something the device will never see.
    """
    out = np.empty((states.shape[0], 20), dtype=np.int64)
    c = states[:, :8]
    out[:, :8] = (c & 7) * 3 + ((c >> 3) & 3)
    e = states[:, 8:]
    out[:, 8:] = (e & 15) * 2 + ((e >> 4) & 1)
    return out


def _canonical_ok(prev: int, m: int) -> bool:
    """Same filter the beam uses. Without it, scrambles contain U U' pairs and
    the label - the inverse of the last move - is not a move toward the goal
    at all. That was silent label noise on a large fraction of samples."""
    if prev < 0:
        return True
    if prev // 3 == m // 3:
        return False
    fa, fb = prev // 3, m // 3
    if (fa + 3) % 6 == fb and fa > fb:
        return False
    return True


_INV_MOVE = np.array(
    [f * 3 + (1 if k == 0 else 0 if k == 1 else 2)
     for f in range(6) for k in range(3)], dtype=np.int64)


def generate_scrambles(n: int, max_depth: int, rng):
    """Random canonical scrambles off solved.

    Returns (states, labels, depths). The label is the inverse of the final
    scramble move: a genuine move toward the goal, guaranteed non-cancelling
    because the scramble itself is canonical.
    """
    depths = rng.integers(1, max_depth + 1, size=n)
    states = np.repeat(EG.SOLVED_ROW.reshape(1, 20), n, axis=0)
    prev = np.full(n, -1, dtype=np.int64)
    last = np.full(n, -1, dtype=np.int64)

    for step in range(1, max_depth + 1):
        active = np.nonzero(depths >= step)[0]
        if active.size == 0:
            break
        moves = rng.integers(0, 18, size=active.size)
        # Resample any move the canonical filter rejects.
        for _ in range(8):
            bad = np.array([not _canonical_ok(int(prev[i]), int(m))
                            for i, m in zip(active, moves)])
            if not bad.any():
                break
            moves[bad] = rng.integers(0, 18, size=int(bad.sum()))
        for m in range(18):
            sel = active[moves == m]
            if sel.size:
                states[sel] = EG.apply_move(states[sel], m)
        prev[active] = moves
        last[active] = moves

    return states, _INV_MOVE[last], depths


def tablebase_labels(states: np.ndarray, class_keys, moves, log=print):
    """Exact optimal moves for any state inside the endgame ball.

    Free perfect supervision on the region where a small policy is weakest -
    and available only because we generated the tablebase ourselves.
    """
    key_index = {k.tobytes(): i for i, k in enumerate(class_keys)}
    reps = EG.class_representative(states)
    rv = EG._rows_view(reps)
    out = np.full(states.shape[0], -1, dtype=np.int64)
    for i in range(states.shape[0]):
        idx = key_index.get(rv[i].tobytes())
        if idx is None or moves[idx] >= 18:
            continue
        k = EG.which_symmetry(states[i], reps[i])
        if k >= 0:
            out[i] = int(EG.SYM_MOVE[k, moves[idx]])
    log(f"  exact labels for {(out >= 0).sum():,}/{len(out):,} states")
    return out


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

if HAVE_TORCH:

    class FakeQuant(torch.autograd.Function):
        """Straight-through estimator: round in the forward pass, pass the
        gradient unchanged backward."""

        @staticmethod
        def forward(ctx, x, bits, scale):
            q = torch.clamp(torch.round(x / scale),
                            -(2 ** (bits - 1)), 2 ** (bits - 1) - 1)
            return q * scale

        @staticmethod
        def backward(ctx, g):
            return g, None, None

    def fake_quant(x, bits, scale):
        return FakeQuant.apply(x, bits, scale)

    class PolicyNet(nn.Module):
        def __init__(self, cfg: SolverConfig, teacher: bool = False):
            super().__init__()
            r, m, t = cfg.representation, cfg.model, cfg.teacher
            dim = t.embed_dim if teacher else r.embed_dim
            hidden = t.hidden if teacher else m.hidden

            self.n_slots = r.n_slots
            self.slot_specific = r.slot_specific
            # The embedding table IS the first layer and IS a lookup table,
            # keyed on coordinates we chose: corners perm*3+ori, edges
            # perm*2+ori. Trained jointly rather than precomputed.
            n_tables = r.n_slots if r.slot_specific else 2
            self.embed = nn.Parameter(
                torch.randn(n_tables, r.n_values, dim) * 0.05)
            self.teacher = teacher
            self.cfg = cfg

            layers = []
            prev = r.n_slots * dim
            for h in hidden:
                layers.append(nn.Linear(prev, h))
                prev = h
            self.hidden_layers = nn.ModuleList(layers)
            self.out = nn.Linear(prev, m.n_moves)
            self.quantised = False

        def features(self, slots):
            # slots: (B, 20) long
            emb = self.embed
            if self.quantised:
                # Embeddings are quantised too. Leaving them out meant they
                # were post-hoc quantised at export - exactly the accuracy
                # cliff QAT exists to avoid.
                q = self.cfg.quant
                scale = emb.abs().amax().clamp(min=1e-8) \
                        / (2 ** (q.embed_bits - 1) - 1)
                emb = fake_quant(emb, q.embed_bits, scale)
            if self.slot_specific:
                idx = torch.arange(self.n_slots, device=slots.device)
            else:
                # Two shared tables: one for corners, one for edges.
                idx = (torch.arange(self.n_slots, device=slots.device) >= 8
                       ).long()
            e = emb[idx.unsqueeze(0), slots]             # (B, 20, D)
            return e.reshape(slots.shape[0], -1)

        def forward(self, slots):
            x = self.features(slots)
            q = self.cfg.quant
            for lin in self.hidden_layers:
                w = lin.weight
                if self.quantised:
                    scale = w.abs().amax(dim=1, keepdim=True).clamp(min=1e-8) \
                            / (2 ** (q.hidden_bits - 1) - 1)
                    w = fake_quant(w, q.hidden_bits, scale)
                x = F.relu(F.linear(x, w, lin.bias))
            w = self.out.weight
            if self.quantised:
                scale = w.abs().amax(dim=1, keepdim=True).clamp(min=1e-8) \
                        / (2 ** (q.output_bits - 1) - 1)
                w = fake_quant(w, q.output_bits, scale)
            return F.linear(x, w, self.out.bias)


class BatchSource:
    """Scramble batches, with exact tablebase labels substituted wherever the
    state lies inside the endgame ball.

    That region is where a small policy is weakest and where beam search most
    needs correct ranking, and the labels are exact and free because we
    generated the tablebase ourselves.
    """

    def __init__(self, cfg: SolverConfig, depth: int = 4, log=print):
        self.cfg = cfg
        self.exact = {}
        if cfg.teacher.use_tablebase_labels:
            log(f"[data] indexing endgame ball (depth {depth}) for exact labels")
            states, dists, reps, keys = EG.build_tablebase(depth,
                                                           log=lambda s: None)
            moves, _ = EG.build_move_map(states, dists, reps, keys,
                                         log=lambda s: None)
            key_index = {k.tobytes(): i for i, k in enumerate(keys)}
            rv = EG._rows_view(reps)
            for i in range(states.shape[0]):
                idx = key_index.get(rv[i].tobytes())
                if idx is None or moves[idx] >= 18:
                    continue
                k = EG.which_symmetry(states[i], reps[i])
                if k >= 0:
                    self.exact[states[i].tobytes()] = int(
                        EG.SYM_MOVE[k, moves[idx]])
            log(f"[data] {len(self.exact):,} states carry exact optimal moves")

    def batch(self, n: int, rng):
        states, labels, depths = generate_scrambles(
            n, self.cfg.teacher.max_scramble, rng)
        if self.exact:
            for i in range(states.shape[0]):
                ex = self.exact.get(states[i].tobytes())
                if ex is not None:
                    labels[i] = ex
        return states, labels, depths


def train_teacher(cfg: SolverConfig, eg, device="cuda", log=print,
                  monitor=None):
    assert HAVE_TORCH, "PyTorch required for training"
    t = cfg.teacher
    net = PolicyNet(cfg, teacher=True).to(device)
    opt = torch.optim.AdamW(net.parameters(), lr=t.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, t.train_steps)
    rng = np.random.default_rng(cfg.seed)
    source = BatchSource(cfg, log=log)

    if monitor is not None:
        monitor.reset_stage("teacher")
    log(f"[teacher] {sum(p.numel() for p in net.parameters()):,} params")
    for step in range(t.train_steps):
        states, labels, depths = source.batch(t.batch_size, rng)
        slots = torch.from_numpy(slot_values(states)).to(device)
        y = torch.from_numpy(labels).to(device)

        logits = net(slots)
        loss = F.cross_entropy(logits, y)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step()
        sched.step()

        if step % 2000 == 0:
            acc = (logits.argmax(1) == y).float().mean().item()
            log(f"  step {step:>7,}  loss {loss.item():.4f}  acc {acc:.3f}")
            if monitor is not None:
                monitor.add("teacher", step, loss.item(), acc=acc)
    return net


def distill_student(cfg: SolverConfig, teacher, device="cuda",
                    steps: int = 200_000, temperature: float = 2.0, log=print,
                    monitor=None):
    assert HAVE_TORCH, "PyTorch required for training"
    student = PolicyNet(cfg, teacher=False).to(device)
    opt = torch.optim.AdamW(student.parameters(), lr=1e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, steps)
    rng = np.random.default_rng(cfg.seed + 1)
    source = BatchSource(cfg, log=log)
    teacher.eval()

    if monitor is not None:
        monitor.reset_stage("distill")
    log(f"[student] {sum(p.numel() for p in student.parameters()):,} params")
    for step in range(steps):
        states, labels, _ = source.batch(cfg.teacher.batch_size, rng)
        slots = torch.from_numpy(slot_values(states)).to(device)
        y = torch.from_numpy(labels).to(device)

        with torch.no_grad():
            soft = F.log_softmax(teacher(slots) / temperature, dim=1)
        logits = student(slots)
        # Ranking information from the teacher, plus the hard label as an anchor.
        kd = F.kl_div(F.log_softmax(logits / temperature, dim=1), soft,
                      log_target=True, reduction="batchmean")
        loss = kd * (temperature ** 2) + 0.3 * F.cross_entropy(logits, y)

        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        sched.step()
        if step % 2000 == 0:
            acc = (logits.argmax(1) == y).float().mean().item()
            log(f"  step {step:>7,}  kd {kd.item():.4f}  acc {acc:.3f}")
            if monitor is not None:
                monitor.add("distill", step, kd.item(), acc=acc)
    return student


def qat_finetune(cfg: SolverConfig, student, teacher, device="cuda",
                 log=print, monitor=None):
    assert HAVE_TORCH, "PyTorch required for training"
    q = cfg.quant
    student.quantised = True
    opt = torch.optim.AdamW(student.parameters(), lr=q.qat_lr)
    rng = np.random.default_rng(cfg.seed + 2)
    source = BatchSource(cfg, log=log)
    if monitor is not None:
        monitor.reset_stage("qat")
    log(f"[qat] embed {q.embed_bits}b, hidden {q.hidden_bits}b, "
        f"out {q.output_bits}b")

    for step in range(q.qat_steps):
        states, labels, _ = source.batch(cfg.teacher.batch_size, rng)
        slots = torch.from_numpy(slot_values(states)).to(device)
        y = torch.from_numpy(labels).to(device)
        with torch.no_grad():
            soft = F.log_softmax(teacher(slots) / 2.0, dim=1)
        logits = student(slots)
        loss = F.kl_div(F.log_softmax(logits / 2.0, dim=1), soft,
                        log_target=True, reduction="batchmean") * 4.0 \
             + 0.3 * F.cross_entropy(logits, y)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        if step % 2000 == 0:
            acc = (logits.argmax(1) == y).float().mean().item()
            log(f"  step {step:>7,}  loss {loss.item():.4f}  acc {acc:.3f}")
            if monitor is not None:
                monitor.add("qat", step, loss.item(), acc=acc)
    return student


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def export_student(cfg: SolverConfig, student, out_dir: str, log=print):
    """Hand float weights to the emitter, which owns quantisation.

    Quantising here as well would give two code paths that must agree; the
    emitter and policy_ref share one, and preflight proves it matches the
    device.
    """
    from . import emit as EM
    tensors = {"embed": student.embed.detach().cpu().numpy().astype(np.float64)}
    for i, lin in enumerate(student.hidden_layers):
        tensors[f"h{i}"] = lin.weight.detach().cpu().numpy().astype(np.float64)
    tensors["out"] = student.out.weight.detach().cpu().numpy().astype(np.float64)
    return EM.emit_model(os.path.join(out_dir, "cube_model.h"), cfg,
                         tensors, log=log)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mantis_cube.train")
    ap.add_argument("--preset", default="balanced", choices=list(PRESETS))
    ap.add_argument("--out", default="firmware")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--teacher-steps", type=int, default=None)
    ap.add_argument("--distill-steps", type=int, default=200_000)
    ap.add_argument("--skip-preflight", action="store_true",
                    help="override the safety gate (not recommended)")
    args = ap.parse_args(argv)

    if not HAVE_TORCH:
        raise SystemExit("PyTorch not installed. This stage runs on your "
                         "workstation, not the device.")

    cfg = PRESETS[args.preset]
    if args.teacher_steps:
        cfg.teacher.train_steps = args.teacher_steps
    print(cfg.report())
    print()

    # Hard gate. Every check here guards a failure mode whose symptom is a
    # model that trains well and solves nothing on device.
    from .preflight import run as preflight_run, PreflightError
    if not args.skip_preflight:
        try:
            preflight_run(cfg, args.out)
        except PreflightError as exc:
            raise SystemExit(f"\nPREFLIGHT FAILED\n{exc}\n\n"
                             f"Refusing to train. Fix the above, or pass "
                             f"--skip-preflight to override.")

    from .monitor import TrainingMonitor
    mon = TrainingMonitor(cfg, on_advice=lambda a: print(f"  {a}"))

    teacher = train_teacher(cfg, None, device=args.device, monitor=mon)
    torch.save(teacher.state_dict(), os.path.join(args.out, "teacher.pt"))

    student = distill_student(cfg, teacher, device=args.device,
                              steps=args.distill_steps, monitor=mon)
    student = qat_finetune(cfg, student, teacher, device=args.device,
                           monitor=mon)
    torch.save(student.state_dict(), os.path.join(args.out, "student.pt"))

    n = export_student(cfg, student, args.out)
    print(f"exported {n/1024:.1f} KB of weights to {args.out}/cube_model.h")


if __name__ == "__main__":
    main()
