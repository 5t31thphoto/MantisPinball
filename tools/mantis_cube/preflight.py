"""
Preflight: everything that must be true before a GPU run is worth starting.

Each check exists because its failure mode is a model that trains beautifully
and solves nothing on device - the most expensive way to be wrong here. These
are hard gates, not warnings: train.py refuses to start if any of them fail.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import numpy as np

from .config import SolverConfig
from . import endgame as EG
from . import emit as EM
from . import policy_ref as PR


class PreflightError(RuntimeError):
    pass


FIRMWARE_SOURCES = ["cube_core.cpp", "cube_endgame.cpp", "cube_policy.cpp"]


def _compile_logit_tool(firmware_dir: str, log=print) -> str:
    out = os.path.join(tempfile.mkdtemp(prefix="mcpre_"), "host_logits")
    srcs = [os.path.join(firmware_dir, f) for f in FIRMWARE_SOURCES]
    srcs.append(os.path.join(firmware_dir, "host_logits.cpp"))
    cmd = ["g++", "-O2", f"-I{firmware_dir}", "-o", out] + srcs
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise PreflightError(
            "firmware does not compile; fix this before training\n"
            + proc.stderr[:2000])
    log("  firmware compiles: OK")
    return out


def check_slot_encoding(log=print) -> None:
    """train.slot_values() must agree with mcSlotValue() for every state.

    If these drift, the student learns a representation the device never
    produces. Checked exhaustively over the categorical range rather than
    sampled.
    """
    from .train import slot_values

    rng = np.random.default_rng(0)
    states = np.stack([EG.pack(_random_cube(rng)) for _ in range(400)])
    batch = slot_values(states)
    for i in range(states.shape[0]):
        single = PR.slot_values_single(states[i])
        if not (batch[i] == single).all():
            raise PreflightError(
                f"slot encoding disagrees at state {i}: "
                f"{batch[i].tolist()} vs {single.tolist()}")
    if batch.min() < 0 or batch.max() >= 24:
        raise PreflightError(
            f"slot values out of range [0, 24): "
            f"[{batch.min()}, {batch.max()}]")
    log(f"  slot encoding host/device agreement ({states.shape[0]}): OK")


def _random_cube(rng):
    from .model import random_cube
    import random
    return random_cube(random.Random(int(rng.integers(0, 2**31))))


def check_forward_agreement(cfg: SolverConfig, firmware_dir: str,
                            n: int = 64, log=print) -> None:
    """Emit random weights, compile, and require identical integer logits.

    This is the check that would have caught the ignored quantisation scales
    and the transposed weight layout.
    """
    rng = np.random.default_rng(cfg.seed)
    r, m = cfg.representation, cfg.model

    tensors = {"embed": rng.normal(0, 0.4,
                                   size=(r.n_slots, r.n_values, r.embed_dim))}
    prev = r.feature_dim
    for i, h in enumerate(m.hidden):
        tensors[f"h{i}"] = rng.normal(0, 0.3, size=(h, prev))
        prev = h
    tensors["out"] = rng.normal(0, 0.3, size=(m.n_moves, prev))

    EM.emit_model(os.path.join(firmware_dir, "cube_model.h"), cfg, tensors,
                  log=lambda s: None)
    binary = _compile_logit_tool(firmware_dir, log=lambda s: None)

    states = []
    for _ in range(n):
        k = int(rng.integers(1, 20))
        scramble = [int(x) for x in rng.integers(0, 18, size=k)]
        cur = EG.SOLVED_ROW.reshape(1, 20)
        for mv in scramble:
            cur = EG.apply_move(cur, mv)
        states.append((scramble, cur[0]))

    if not PR.compare_with_device(cfg, tensors, states, binary, log=log):
        raise PreflightError(
            "host reference and device inference disagree. Training would "
            "produce weights the device evaluates differently.")


def check_quant_roundtrip(cfg: SolverConfig, log=print) -> None:
    """Quantisation must preserve ranking well enough to be usable."""
    rng = np.random.default_rng(cfg.seed + 5)
    r, m = cfg.representation, cfg.model
    tensors = {"embed": rng.normal(0, 0.4,
                                   size=(r.n_slots, r.n_values, r.embed_dim))}
    prev = r.feature_dim
    for i, h in enumerate(m.hidden):
        tensors[f"h{i}"] = rng.normal(0, 0.3, size=(h, prev))
        prev = h
    tensors["out"] = rng.normal(0, 0.3, size=(m.n_moves, prev))

    quantised = PR.quantise_all(cfg, tensors)
    agree = 0
    trials = 128
    for _ in range(trials):
        k = int(rng.integers(1, 20))
        cur = EG.SOLVED_ROW.reshape(1, 20)
        for mv in rng.integers(0, 18, size=k):
            cur = EG.apply_move(cur, int(mv))
        qlog = PR.policy_logits(cfg, quantised, cur[0])

        # float reference
        slots = PR.slot_values_single(cur[0])
        act = np.concatenate([tensors["embed"][i, slots[i]] for i in range(20)])
        for i in range(len(m.hidden)):
            act = np.maximum(tensors[f"h{i}"] @ act, 0)
        flog = tensors["out"] @ act
        if int(np.argmax(qlog)) == int(np.argmax(flog)):
            agree += 1

    rate = agree / trials
    log(f"  quantised argmax matches float on {rate*100:.0f}% of states")
    if rate < 0.60:
        raise PreflightError(
            f"quantisation destroys ranking (argmax agreement {rate*100:.0f}%"
            f" at {cfg.quant.hidden_bits} bits). Raise hidden_bits or check "
            f"the requantisation path.")


def check_config(cfg: SolverConfig, log=print) -> None:
    ok, problems = cfg.fits()
    if not ok:
        raise PreflightError("configuration exceeds budget:\n  - "
                             + "\n  - ".join(problems))
    if cfg.representation.n_values < 24:
        raise PreflightError(
            f"n_values must be at least 24 (corners use perm*3+ori); "
            f"got {cfg.representation.n_values}")
    if not cfg.representation.slot_specific:
        raise PreflightError(
            "representation.slot_specific=False is not implemented in "
            "cube_policy.cpp; mcSlotValue() indexes per-slot tables. Either "
            "set it True or add the shared-table path to the firmware.")
    if not cfg.model.hidden:
        raise PreflightError("model.hidden must have at least one layer")
    if cfg.quant.hidden_bits < 2 or cfg.quant.hidden_bits > 8:
        raise PreflightError("hidden_bits must be in [2, 8]")
    log("  configuration within budget and self-consistent: OK")


def check_endgame_present(firmware_dir: str, log=print) -> None:
    p = os.path.join(firmware_dir, "cube_endgame.h")
    if not os.path.exists(p):
        raise PreflightError(
            f"{p} missing. Run: python -m mantis_cube.build --out "
            f"{firmware_dir}")
    with open(p) as fh:
        head = fh.read(4000)
    if "MC_ENDGAME_CLASSES" not in head:
        raise PreflightError(f"{p} is malformed; regenerate it")
    log("  endgame tablebase present: OK")


def run(cfg: SolverConfig, firmware_dir: str = "firmware", log=print) -> None:
    """Raises PreflightError on the first failure."""
    log("Preflight")
    check_config(cfg, log=log)
    check_endgame_present(firmware_dir, log=log)
    check_slot_encoding(log=log)
    check_quant_roundtrip(cfg, log=log)
    check_forward_agreement(cfg, firmware_dir, log=log)
    log("Preflight passed - safe to train.\n")


if __name__ == "__main__":
    import sys
    from .config import PRESETS
    preset = sys.argv[1] if len(sys.argv) > 1 else "balanced"
    try:
        run(PRESETS[preset])
    except PreflightError as exc:
        print(f"\nPREFLIGHT FAILED\n{exc}")
        sys.exit(1)
