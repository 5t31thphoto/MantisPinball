"""
MantisCubeSolver — parameter schema.

Everything the tkinter tab sweeps lives here. Nothing downstream hardcodes a
size, a bit-depth, or a beam width; they all read from these dataclasses so a
sweep is a loop over configs rather than an edit.
"""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import List, Optional
import json


@dataclass
class RepresentationConfig:
    """How the network sees a cube.

    The 20-byte packed state is 20 slots, each holding a small categorical
    value: corners take perm*3+ori (24 values), edges take perm*2+ori (24
    values). One embedding row per (slot, value) pair.

    This is the hybrid: the embedding table IS a lookup table and IS the
    first layer. Keyed on coordinates we chose, trained jointly.
    """
    embed_dim: int = 16
    n_slots: int = 20
    n_values: int = 24          # max categorical values per slot
    slot_specific: bool = True  # separate table per slot vs shared

    @property
    def embedding_params(self) -> int:
        tables = self.n_slots if self.slot_specific else 2
        return tables * self.n_values * self.embed_dim

    @property
    def feature_dim(self) -> int:
        return self.n_slots * self.embed_dim


@dataclass
class ModelConfig:
    """Policy network. Outputs 18 logits - one forward pass ranks every
    child, which is the whole reason this beats a value function on device."""
    hidden: List[int] = field(default_factory=lambda: [256, 256])
    n_moves: int = 18
    residual: bool = True
    dropout: float = 0.0

    def mlp_params(self, in_dim: int) -> int:
        total = 0
        prev = in_dim
        for h in self.hidden:
            total += prev * h + h
            prev = h
        total += prev * self.n_moves + self.n_moves
        return total


@dataclass
class TeacherConfig:
    """The teacher may cheat freely: exact tables, wide beam, unlimited
    compute. Nothing derived from it ships except the student's imitation."""
    hidden: List[int] = field(default_factory=lambda: [4096, 4096, 4096])
    embed_dim: int = 64
    residual: bool = True
    train_steps: int = 400_000
    batch_size: int = 4096
    lr: float = 1e-3
    max_scramble: int = 26
    use_tablebase_labels: bool = True


@dataclass
class QuantConfig:
    """We design the numeric format. PIE lanes are int8, so anything that
    decodes cheaply to int8 is fair game - including per-layer bit-depths."""
    embed_bits: int = 8         # embeddings are small; keep precision
    hidden_bits: int = 4        # bulk of the weights
    output_bits: int = 8        # small layer, precision matters
    per_channel_scale: bool = True
    ternary_hidden: bool = False
    qat_steps: int = 60_000
    qat_lr: float = 2e-4

    def bytes_for(self, params: int, bits: int) -> float:
        return params * bits / 8.0


@dataclass
class SearchConfig:
    """Beam over the policy, terminated exactly by the tablebase."""
    beam_width: int = 192
    max_depth: int = 26
    endgame_depth: int = 5
    symmetry_dedup: bool = True
    n_frames: int = 8           # multi-frame restarts, best-of
    dual_core: bool = True


@dataclass
class BudgetConfig:
    """Hard ceilings. Any config that exceeds these is rejected by the sweep
    rather than silently widened."""
    sram_bytes: int = 32 * 1024
    flash_bytes: int = 512 * 1024
    target_ms: int = 2500
    cpu_hz: int = 240_000_000
    cores: int = 2
    effective_simd_lanes: int = 12   # conservative vs the nominal 16


@dataclass
class SolverConfig:
    representation: RepresentationConfig = field(default_factory=RepresentationConfig)
    model: ModelConfig = field(default_factory=ModelConfig)
    teacher: TeacherConfig = field(default_factory=TeacherConfig)
    quant: QuantConfig = field(default_factory=QuantConfig)
    search: SearchConfig = field(default_factory=SearchConfig)
    budget: BudgetConfig = field(default_factory=BudgetConfig)
    seed: int = 1337

    # -- derived accounting -------------------------------------------------

    def student_param_count(self) -> int:
        return (self.representation.embedding_params
                + self.model.mlp_params(self.representation.feature_dim))

    def flash_bytes_used(self, endgame_classes: int = 26_098) -> float:
        r, m, q = self.representation, self.model, self.quant
        emb = q.bytes_for(r.embedding_params, q.embed_bits)
        mlp = m.mlp_params(r.feature_dim)
        out_p = m.hidden[-1] * m.n_moves + m.n_moves if m.hidden else 0
        hidden_p = mlp - out_p
        weights = (q.bytes_for(hidden_p, q.hidden_bits)
                   + q.bytes_for(out_p, q.output_bits))
        endgame = endgame_classes * 5 / 8.0
        return emb + weights + endgame + 512  # + conjugation table

    def sram_bytes_used(self) -> float:
        s = self.search
        # 20 B state + packed path + score, double buffered
        per_state = 20 + (s.max_depth * 5 + 7) // 8 + 2
        return s.beam_width * per_state * 2 + 2048

    def cycles_per_node(self) -> float:
        macs = self.student_param_count()
        return macs / self.budget.effective_simd_lanes + 60

    def estimated_ms(self) -> float:
        s, b = self.search, self.budget
        nodes = s.beam_width * s.max_depth * s.n_frames
        cycles = nodes * self.cycles_per_node()
        return cycles / (b.cpu_hz * (b.cores if s.dual_core else 1)) * 1000

    def fits(self, endgame_classes: int = 26_098):
        problems = []
        if self.sram_bytes_used() > self.budget.sram_bytes:
            problems.append(
                f"SRAM {self.sram_bytes_used()/1024:.1f} KB > "
                f"{self.budget.sram_bytes/1024:.0f} KB")
        if self.flash_bytes_used(endgame_classes) > self.budget.flash_bytes:
            problems.append(
                f"flash {self.flash_bytes_used(endgame_classes)/1024:.1f} KB > "
                f"{self.budget.flash_bytes/1024:.0f} KB")
        if self.estimated_ms() > self.budget.target_ms:
            problems.append(
                f"time {self.estimated_ms():.0f} ms > {self.budget.target_ms} ms")
        return (len(problems) == 0), problems

    def report(self) -> str:
        ok, problems = self.fits()
        lines = [
            "MantisCubeSolver configuration",
            f"  student params   : {self.student_param_count():,}",
            f"  flash used       : {self.flash_bytes_used()/1024:.1f} KB "
            f"/ {self.budget.flash_bytes/1024:.0f} KB",
            f"  SRAM used        : {self.sram_bytes_used()/1024:.1f} KB "
            f"/ {self.budget.sram_bytes/1024:.0f} KB",
            f"  cycles per node  : {self.cycles_per_node():,.0f}",
            f"  estimated solve  : {self.estimated_ms():.0f} ms "
            f"(beam {self.search.beam_width}, {self.search.n_frames} frames)",
            f"  verdict          : {'FITS' if ok else 'DOES NOT FIT'}",
        ]
        for p in problems:
            lines.append(f"    - {p}")
        return "\n".join(lines)

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2)

    @staticmethod
    def from_json(text: str) -> "SolverConfig":
        d = json.loads(text)
        return SolverConfig(
            representation=RepresentationConfig(**d["representation"]),
            model=ModelConfig(**d["model"]),
            teacher=TeacherConfig(**d["teacher"]),
            quant=QuantConfig(**d["quant"]),
            search=SearchConfig(**d["search"]),
            budget=BudgetConfig(**d["budget"]),
            seed=d.get("seed", 1337),
        )


# Presets the tab exposes as a dropdown.
PRESETS = {
    "tiny": SolverConfig(
        representation=RepresentationConfig(embed_dim=8),
        model=ModelConfig(hidden=[128]),
        search=SearchConfig(beam_width=128, n_frames=4),
    ),
    "balanced": SolverConfig(),
    "quality": SolverConfig(
        representation=RepresentationConfig(embed_dim=24),
        model=ModelConfig(hidden=[384, 384]),
        search=SearchConfig(beam_width=256, n_frames=16),
        quant=QuantConfig(hidden_bits=4),
    ),
}
