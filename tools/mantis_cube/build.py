"""
MantisCubeSolver build pipeline.

Run from the tkinter tab or the command line. Generates every flash-resident
resource the firmware needs, verifies each one against the reference model,
and reports the budget.

    python -m mantis_cube.build --out firmware --endgame-depth 5

Nothing generated here is derived from a published pruning table. The tables in
tables.py exist solely as an oracle for verify.py.
"""

from __future__ import annotations

import argparse
import os
import numpy as np

from . import endgame as EG
from . import emit as EM
from .config import SolverConfig, PRESETS


def build_endgame(out_dir: str, depth: int, log=print):
    log(f"[endgame] building ball of radius {depth}")
    states, dists, reps, class_keys = EG.build_tablebase(depth, log=log)
    moves, key_index = EG.build_move_map(states, dists, reps, class_keys,
                                         log=log)

    log("[endgame] emitting")
    keys_arr = class_keys.view(np.uint8).reshape(len(class_keys), 20)
    n = EM.emit_endgame(os.path.join(out_dir, "cube_endgame.h"),
                        keys_arr, moves, EG.SYM_MOVE, depth, log=log)
    m = EM.emit_symmetry(os.path.join(out_dir, "cube_symmetry.h"),
                         EG.SYMS, EG.SYM_INV, log=log)
    return dict(states=states, dists=dists, reps=reps,
                class_keys=class_keys, moves=moves,
                flash=n + m, classes=len(class_keys))


def build_model_stub(out_dir: str, cfg: SolverConfig, log=print):
    """Emit a random-weight model so the firmware compiles and the search
    plumbing can be exercised before any training has happened. train.py
    overwrites this with the distilled student."""
    rng = np.random.default_rng(cfg.seed)
    r, m = cfg.representation, cfg.model

    tensors = {}
    tensors["embed"] = rng.integers(-8, 8,
                                    size=(r.n_slots, r.n_values, r.embed_dim),
                                    dtype=np.int8)
    prev = r.feature_dim
    for i, h in enumerate(m.hidden):
        # (out, in) to match PyTorch's nn.Linear.weight layout, which is what
        # train.py exports and what cube_policy.cpp indexes.
        tensors[f"h{i}"] = rng.integers(-7, 8, size=(h, prev), dtype=np.int8)
        prev = h
    tensors["out"] = rng.integers(-8, 8, size=(m.n_moves, prev), dtype=np.int8)

    scales = {"embed": np.full(1, 0.05, dtype=np.float32)}
    for i, h in enumerate(m.hidden):
        scales[f"h{i}"] = np.full(h, 0.02, dtype=np.float32)
    scales["out"] = np.full(m.n_moves, 0.02, dtype=np.float32)

    n = EM.emit_model(os.path.join(out_dir, "cube_model.h"), cfg,
                      tensors, scales, log=log)
    log("[model] NOTE: random weights. Run train.py to replace.")
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mantis_cube.build")
    ap.add_argument("--out", default="firmware")
    ap.add_argument("--endgame-depth", type=int, default=5)
    ap.add_argument("--preset", default="balanced", choices=list(PRESETS))
    ap.add_argument("--skip-model", action="store_true")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    cfg = PRESETS[args.preset]

    eg = build_endgame(args.out, args.endgame_depth)
    model_flash = 0 if args.skip_model else build_model_stub(args.out, cfg)

    print()
    print(cfg.report())
    print(f"  endgame classes  : {eg['classes']:,}")
    print(f"  measured flash   : "
          f"{(eg['flash'] + model_flash)/1024:.1f} KB")
    return cfg, eg


if __name__ == "__main__":
    main()
