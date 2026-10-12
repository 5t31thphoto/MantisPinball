"""
MantisCubeSolver — training monitor and advisor.

Watches the metric stream and says what it means. The point is to catch a bad
run in the first few minutes rather than at the end: a teacher that plateaus
low, a quantisation step that blows up, a learning rate that diverges, or a
data bug that shows up as accuracy pinned near chance.

Deliberately free of any torch dependency so the rules can be tested against
synthetic runs without a GPU.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional
import math
import time


CHANCE_ACC = 1.0 / 18.0          # 18 moves, uniform guess


@dataclass
class Sample:
    stage: str            # "teacher" | "distill" | "qat"
    step: int
    loss: float
    acc: Optional[float] = None
    extra: Dict[str, float] = field(default_factory=dict)
    t: float = field(default_factory=time.time)


@dataclass
class Advice:
    level: str            # "ok" | "info" | "warn" | "err"
    title: str
    detail: str
    action: Optional[str] = None

    def __str__(self):
        s = f"[{self.level.upper()}] {self.title}: {self.detail}"
        if self.action:
            s += f"  -> {self.action}"
        return s


class TrainingMonitor:
    """Ingests samples, tracks per-stage history, emits advice on change.

    Each rule fires at most once per stage so the log does not fill with the
    same warning; `reset_stage` clears that when a new stage begins.
    """

    def __init__(self, cfg=None, on_advice: Callable[[Advice], None] = None):
        self.cfg = cfg
        self.on_advice = on_advice
        self.history: Dict[str, List[Sample]] = {}
        self._fired: set = set()
        self.stage_start: Dict[str, float] = {}

    # ------------------------------------------------------------------ api

    def reset_stage(self, stage: str):
        self.history.setdefault(stage, []).clear()
        self._fired = {f for f in self._fired if not f.startswith(stage + ":")}
        self.stage_start[stage] = time.time()

    def add(self, stage: str, step: int, loss: float,
            acc: float = None, **extra) -> List[Advice]:
        s = Sample(stage=stage, step=step, loss=float(loss),
                   acc=None if acc is None else float(acc), extra=extra)
        self.history.setdefault(stage, []).append(s)
        out = []
        for advice in self._evaluate(stage):
            key = f"{stage}:{advice.title}"
            if key in self._fired:
                continue
            self._fired.add(key)
            out.append(advice)
            if self.on_advice:
                self.on_advice(advice)
        return out

    # -------------------------------------------------------------- summary

    def latest(self, stage: str) -> Optional[Sample]:
        h = self.history.get(stage) or []
        return h[-1] if h else None

    def trend(self, stage: str, key: str = "loss", window: int = 10):
        """Slope over the last `window` samples. Negative means improving
        for loss, positive means improving for accuracy."""
        h = self.history.get(stage) or []
        pts = [getattr(s, key) for s in h[-window:] if getattr(s, key) is not None]
        if len(pts) < 3:
            return None
        n = len(pts)
        xs = list(range(n))
        mx = sum(xs) / n
        my = sum(pts) / n
        denom = sum((x - mx) ** 2 for x in xs)
        if denom == 0:
            return None
        return sum((xs[i] - mx) * (pts[i] - my) for i in range(n)) / denom

    def eta_seconds(self, stage: str, total_steps: int) -> Optional[float]:
        h = self.history.get(stage) or []
        if len(h) < 2 or not total_steps:
            return None
        elapsed = h[-1].t - h[0].t
        done = h[-1].step - h[0].step
        if done <= 0 or elapsed <= 0:
            return None
        return (total_steps - h[-1].step) * (elapsed / done)

    def summary(self, stage: str, total_steps: int = 0) -> str:
        s = self.latest(stage)
        if not s:
            return f"{stage}: waiting"
        bits = [f"step {s.step:,}", f"loss {s.loss:.4f}"]
        if s.acc is not None:
            bits.append(f"acc {s.acc*100:.1f}%")
        eta = self.eta_seconds(stage, total_steps)
        if eta:
            bits.append(f"eta {int(eta//60)}m{int(eta%60):02d}s")
        return f"{stage}: " + "  ".join(bits)

    # ---------------------------------------------------------------- rules

    def _evaluate(self, stage: str) -> List[Advice]:
        h = self.history.get(stage) or []
        if not h:
            return []
        cur = h[-1]
        out = []

        # -- hard failures ---------------------------------------------
        if math.isnan(cur.loss) or math.isinf(cur.loss):
            out.append(Advice(
                "err", "loss is NaN",
                "the run has diverged and will not recover",
                "stop; lower the learning rate (teacher.lr) by 10x and "
                "restart this stage"))
            return out

        if len(h) >= 12 and cur.loss > h[0].loss * 3:
            out.append(Advice(
                "err", "loss exploding",
                f"loss rose from {h[0].loss:.3f} to {cur.loss:.3f}",
                "stop; lower teacher.lr, or check that gradient clipping "
                "is active"))
            return out

        # -- data / label sanity ---------------------------------------
        if cur.acc is not None and cur.step >= 4000 and cur.acc < CHANCE_ACC * 1.5:
            out.append(Advice(
                "err", "accuracy near chance",
                f"{cur.acc*100:.1f}% after {cur.step:,} steps; random guessing "
                f"is {CHANCE_ACC*100:.1f}%",
                "this is a data bug, not a capacity one. Re-run preflight and "
                "check that slot encoding and labels agree"))

        # -- stalled learning ------------------------------------------
        # Only meaningful while a stage is still supposed to be improving.
        # QAT loss is expected to sit flat once the weights settle on the grid.
        if stage in ("teacher", "distill") and len(h) >= 10:
            sl = self.trend(stage, "loss", 10)
            if sl is not None and abs(sl) < 1e-5 and cur.step >= 10_000:
                out.append(Advice(
                    "warn", "loss flat",
                    f"no measurable movement over the last {10} samples",
                    "if accuracy is also flat, the model is saturated; raise "
                    "model.hidden or representation.embed_dim"))

        # -- stage-specific --------------------------------------------
        if stage == "teacher":
            out.extend(self._teacher_rules(h))
        elif stage == "qat":
            out.extend(self._qat_rules(h))
        elif stage == "distill":
            out.extend(self._distill_rules(h))
        return out

    def _teacher_rules(self, h) -> List[Advice]:
        cur = h[-1]
        out = []
        if cur.acc is None:
            return out

        if cur.step >= 60_000 and cur.acc < 0.45:
            out.append(Advice(
                "warn", "teacher plateauing low",
                f"top-1 is {cur.acc*100:.1f}% at step {cur.step:,}; the "
                f"student cannot exceed what the teacher knows",
                "stop and raise teacher.hidden (e.g. [4096,4096,4096] -> "
                "wider or deeper) before spending time on distillation"))
        elif cur.step >= 60_000 and cur.acc >= 0.60:
            out.append(Advice(
                "ok", "teacher healthy",
                f"top-1 {cur.acc*100:.1f}% at step {cur.step:,}",
                "on track; distillation should retain most of this"))

        acc_slope = self.trend("teacher", "acc", 12)
        if (acc_slope is not None and acc_slope <= 0
                and cur.step >= 40_000 and cur.acc < 0.55):
            out.append(Advice(
                "warn", "teacher accuracy stopped improving",
                f"flat or falling near {cur.acc*100:.1f}%",
                "more steps will not help; change capacity or lower the "
                "learning rate schedule floor"))
        return out

    def _distill_rules(self, h) -> List[Advice]:
        cur = h[-1]
        out = []
        if len(h) >= 20:
            sl = self.trend("distill", "loss", 15)
            if sl is not None and sl > 0:
                out.append(Advice(
                    "warn", "distillation loss rising",
                    "the student is drifting away from the teacher",
                    "lower the student learning rate, or raise the KD "
                    "temperature so the targets are softer"))
        return out

    def _qat_rules(self, h) -> List[Advice]:
        cur = h[-1]
        out = []
        bits = getattr(getattr(self.cfg, "quant", None), "hidden_bits", None)

        base = self._pre_qat_loss()
        if base is not None and len(h) >= 5:
            ratio = cur.loss / max(base, 1e-9)
            if ratio > 2.5:
                out.append(Advice(
                    "err", "quantisation is destroying the model",
                    f"loss {cur.loss:.3f} vs {base:.3f} before QAT "
                    f"({ratio:.1f}x)",
                    f"hidden_bits={bits} is too aggressive for this "
                    f"architecture. Raise it to {min(8, (bits or 4) + 2)} "
                    f"and re-run QAT"))
            elif ratio > 1.4:
                out.append(Advice(
                    "warn", "noticeable quantisation cost",
                    f"loss rose {(ratio-1)*100:.0f}% when the quantiser "
                    f"turned on",
                    f"expected at {bits} bits, but check the final solve "
                    f"rate; raise hidden_bits if move count disappoints"))
            elif ratio >= 1.0:
                out.append(Advice(
                    "ok", "quantisation clean",
                    f"loss rose only {(ratio-1)*100:.0f}% at {bits} bits",
                    "the weights adapted to the grid; export should hold up"))
            else:
                out.append(Advice(
                    "ok", "quantisation free",
                    f"loss is {(1-ratio)*100:.0f}% below the pre-QAT average "
                    f"at {bits} bits",
                    f"no measurable cost; {bits} bits may even be more than "
                    f"needed - try fewer to buy flash back"))
        return out

    def _pre_qat_loss(self) -> Optional[float]:
        d = self.history.get("distill") or []
        if len(d) < 3:
            return None
        tail = [s.loss for s in d[-5:]]
        return sum(tail) / len(tail)


def format_report(monitor: TrainingMonitor, cfg=None) -> str:
    lines = []
    for stage in ("teacher", "distill", "qat"):
        h = monitor.history.get(stage)
        if not h:
            continue
        total = 0
        if cfg is not None:
            if stage == "teacher":
                total = cfg.teacher.train_steps
            elif stage == "qat":
                total = cfg.quant.qat_steps
        lines.append(monitor.summary(stage, total))
    return "\n".join(lines) if lines else "no samples yet"
