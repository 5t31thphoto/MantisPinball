"""
MantisCubeSolver — installer panel.

Designed to drop into the Pinball-S3 installer rather than stand alone. It
borrows the host app's log, paths, root window, and theme instead of creating
its own, so build output lands in the same BUILD / UPLOAD LOG the kids are
already watching.

Attach in one line from Installer._build_ui(), alongside the other panels:

    from mantis_cube.panel import CubeSolverPanel
    self.cube_panel = CubeSolverPanel.attach(self)

Or place it explicitly in any container (a tab body, a frame, anything):

    CubeSolverPanel(parent_frame,
                    log=self._log,
                    root=self.root,
                    out_dir=SRC_DIR / "rubiks")

Nothing here owns a Tk root, a log widget, or a path constant. That is the
whole point: the panel is a component, not a second application.
"""

from __future__ import annotations

import copy
import os
import queue
import threading
from pathlib import Path

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from .config import PRESETS, SolverConfig

# Installer theme. Imported from the host when available so a theme change in
# the installer carries over; these are only fallbacks for standalone use.
DARK = "#1b1b1f"
PANEL = "#26262c"
ACCENT = "#5D005D"
TEXT = "#e8e8e8"
OKGREEN = "#9fe89f"


class CubeSolverPanel:
    """Cube solver build controls as an installer panel."""

    TITLE = "CUBE SOLVER"

    # ------------------------------------------------------------------ setup

    def __init__(self, parent, log=None, root=None, out_dir=None,
                 theme=None):
        self.parent = parent
        self.root = root or parent.winfo_toplevel()
        self._host_log = log
        self.out_dir = Path(out_dir) if out_dir else Path("firmware")

        t = theme or {}
        self.DARK = t.get("DARK", DARK)
        self.PANEL = t.get("PANEL", PANEL)
        self.ACCENT = t.get("ACCENT", ACCENT)
        self.TEXT = t.get("TEXT", TEXT)

        # Own queue so the worker never touches Tk from off-thread, drained on
        # the host's root. Same discipline as the installer's upload worker.
        self.log_q: "queue.Queue[tuple]" = queue.Queue()
        self.ui_q: "queue.Queue[tuple]" = queue.Queue()
        self._worker = None
        self._preflight_ok = False
        self.monitor = None

        self.preset = tk.StringVar(value="balanced")
        self.beam_width = tk.IntVar(value=192)
        self.embed_dim = tk.IntVar(value=16)
        self.hidden_bits = tk.IntVar(value=4)
        self.endgame_depth = tk.IntVar(value=5)
        self.frames = tk.IntVar(value=8)

        self.box = self._build(parent)
        self._sync_from_preset()
        self.root.after(80, self._drain)

    @classmethod
    def attach(cls, installer, out_dir=None):
        """Wire to a Pinball-S3 Installer using its own conventions.

        Pulls SRC_DIR and the theme constants from whichever module defines
        the Installer, so it works whether installer.py is run directly
        (__main__) or imported.
        """
        import sys as _sys
        section = installer._section(cls.TITLE)
        mod = _sys.modules.get(type(installer).__module__)
        src = getattr(mod, "SRC_DIR", None) if mod else None
        theme = ({k: getattr(mod, k)
                  for k in ("DARK", "PANEL", "ACCENT", "TEXT")
                  if hasattr(mod, k)} if mod else {})
        return cls(section,
                   log=getattr(installer, "_log", None),
                   root=installer.root,
                   out_dir=out_dir or (Path(src) / "rubiks" if src else None),
                   theme=theme)

    # ------------------------------------------------------------------- UI

    def _btn(self, parent, text, cmd, accent=False):
        return tk.Button(parent, text=text, command=cmd,
                         bg=self.ACCENT if accent else self.PANEL,
                         fg="white" if accent else self.TEXT,
                         relief="flat", padx=10)

    def _build(self, parent):
        box = parent

        tk.Label(box, text="Neural policy solver \u2014 tables, training, weights",
                 fg=self.TEXT, bg=self.DARK,
                 font=("TkDefaultFont", 8)).pack(anchor="w", padx=4)

        # --- settings grid ---
        grid = tk.Frame(box, bg=self.DARK)
        grid.pack(fill="x", padx=4, pady=4)

        tk.Label(grid, text="Preset", fg=self.TEXT, bg=self.DARK,
                 font=("TkDefaultFont", 8)).grid(row=0, column=0, sticky="w")
        cb = ttk.Combobox(grid, textvariable=self.preset, state="readonly",
                          values=list(PRESETS), width=10)
        cb.grid(row=0, column=1, sticky="w", padx=(2, 10))
        cb.bind("<<ComboboxSelected>>", lambda e: self._sync_from_preset())

        self._spin(grid, "Beam", self.beam_width, 32, 512, 0, 2)
        self._spin(grid, "Frames", self.frames, 1, 24, 0, 4)
        self._spin(grid, "Endgame", self.endgame_depth, 3, 6, 1, 0)
        self._spin(grid, "Embed", self.embed_dim, 4, 48, 1, 2)
        self._spin(grid, "Bits", self.hidden_bits, 2, 8, 1, 4)

        # --- live budget readout ---
        self.summary = tk.Label(box, text="", fg=OKGREEN, bg=self.DARK,
                                justify="left", anchor="w",
                                font=("TkDefaultFont", 8))
        self.summary.pack(anchor="w", fill="x", padx=4, pady=2)

        # --- actions ---
        row = tk.Frame(box, bg=self.DARK)
        row.pack(fill="x", pady=4)
        self._btn(row, "Build Tables", self._on_build).pack(side="left", padx=2)
        self._btn(row, "Preflight", self._on_preflight).pack(side="left", padx=2)
        self._btn(row, "Verify", self._on_verify).pack(side="left", padx=2)

        row2 = tk.Frame(box, bg=self.DARK)
        row2.pack(fill="x", pady=2)
        self.train_btn = self._btn(row2, "\U0001F9E0  TRAIN MODEL",
                                   self._on_train, accent=True)
        self.train_btn.pack(fill="x", padx=2)

        self.progress = ttk.Progressbar(box, mode="indeterminate")
        self.progress.pack(fill="x", padx=4, pady=(4, 2))

        # --- live training telemetry ---
        self.metrics = tk.Label(box, text="", fg=self.TEXT, bg=self.DARK,
                                anchor="w", justify="left",
                                font=("Consolas", 8))
        self.metrics.pack(fill="x", padx=4)

        # --- advisor: what the numbers mean and what to do about it ---
        self.advice = tk.Label(box, text="", bg=self.DARK, anchor="w",
                               justify="left", wraplength=360,
                               font=("TkDefaultFont", 8))
        self.advice.pack(fill="x", padx=4, pady=(2, 0))

        self.status = tk.Label(box, text="idle", fg=self.TEXT, bg=self.DARK,
                               anchor="w", font=("TkDefaultFont", 8))
        self.status.pack(fill="x", padx=4)
        return box

    def _spin(self, parent, label, var, lo, hi, row, col):
        tk.Label(parent, text=label, fg=self.TEXT, bg=self.DARK,
                 font=("TkDefaultFont", 8)).grid(row=row, column=col,
                                                 sticky="e", padx=(4, 2))
        sp = ttk.Spinbox(parent, from_=lo, to=hi, textvariable=var, width=6,
                         command=self._on_setting_change)
        sp.grid(row=row, column=col + 1, sticky="w")
        var.trace_add("write", lambda *_: self._on_setting_change())

    # -------------------------------------------------------------- config

    def _sync_from_preset(self):
        c = PRESETS[self.preset.get()]
        self.beam_width.set(c.search.beam_width)
        self.frames.set(c.search.n_frames)
        self.embed_dim.set(c.representation.embed_dim)
        self.hidden_bits.set(c.quant.hidden_bits)
        self.endgame_depth.set(c.search.endgame_depth)
        self._on_setting_change()

    def current_config(self) -> SolverConfig:
        c = copy.deepcopy(PRESETS[self.preset.get()])
        try:
            c.search.beam_width = int(self.beam_width.get())
            c.search.n_frames = int(self.frames.get())
            c.search.endgame_depth = int(self.endgame_depth.get())
            c.representation.embed_dim = int(self.embed_dim.get())
            c.quant.hidden_bits = int(self.hidden_bits.get())
        except (tk.TclError, ValueError):
            pass                      # mid-edit spinbox; keep the last good value
        return c

    def _on_setting_change(self):
        # Any change invalidates a previous pass, so a stale configuration
        # cannot reach training.
        self._preflight_ok = False
        c = self.current_config()
        ok, problems = c.fits()
        self.summary.config(
            text=(f"{c.student_param_count():,} params   "
                  f"flash {c.flash_bytes_used()/1024:.0f} KB   "
                  f"SRAM {c.sram_bytes_used()/1024:.1f} KB   "
                  f"~{c.estimated_ms():.0f} ms"
                  + ("" if ok else "\n" + "; ".join(problems))),
            fg=OKGREEN if ok else "#ff8080")

    # -------------------------------------------------------------- logging

    def log(self, text, tag=None):
        """Thread-safe; queued and drained onto the host log."""
        if not text.endswith("\n"):
            text += "\n"
        self.log_q.put((tag, text))

    def _drain(self):
        try:
            while True:
                tag, payload = self.log_q.get_nowait()
                if self._host_log:
                    self._host_log(payload, tag)
                else:
                    print(payload, end="")
        except queue.Empty:
            pass

        # Widget updates are queued from the worker and applied here, on the
        # Tk thread, the same discipline the installer's upload worker uses.
        try:
            while True:
                kind, payload = self.ui_q.get_nowait()
                if kind == "metrics":
                    self.metrics.config(text=payload)
                elif kind == "advice":
                    level, text = payload
                    colour = {"ok": OKGREEN, "info": "#80c0ff",
                              "warn": "#ffcc70", "err": "#ff8080"}.get(
                                  level, self.TEXT)
                    self.advice.config(text=text, fg=colour)
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _on_advice(self, advice):
        """Called from the training thread; queued for the Tk thread."""
        tag = {"ok": "ok", "info": "info",
               "warn": "err", "err": "err"}.get(advice.level)
        self.log(str(advice), tag)
        text = f"{advice.title} \u2014 {advice.detail}"
        if advice.action:
            text += f"\n\u2192 {advice.action}"
        self.ui_q.put(("advice", (advice.level, text)))

    def _push_metrics(self):
        if not self.monitor:
            return
        from .monitor import format_report
        self.ui_q.put(("metrics", format_report(self.monitor,
                                                self.current_config())))

    def _run(self, name, fn):
        if self._worker and self._worker.is_alive():
            self.log("A cube solver job is already running.", "err")
            return
        self.status.config(text=f"{name}\u2026")
        self.progress.start(12)

        def wrapper():
            try:
                fn()
                self.log(f"=== cube solver: {name} complete ===", "ok")
            except Exception as exc:                       # noqa: BLE001
                self.log(f"=== cube solver: {name} FAILED ===", "err")
                self.log(str(exc), "err")
            finally:
                self.root.after(0, self._job_done)

        self._worker = threading.Thread(target=wrapper, daemon=True)
        self._worker.start()

    def _job_done(self):
        self.progress.stop()
        self.status.config(text="idle")

    # -------------------------------------------------------------- actions

    def _on_build(self):
        def job():
            from . import build
            out = str(self.out_dir)
            os.makedirs(out, exist_ok=True)
            self.log(f"=== cube solver: generating tables into {out} ===", "info")
            build.build_endgame(out, int(self.endgame_depth.get()),
                                log=self.log)
            build.build_model_stub(out, self.current_config(), log=self.log)
        self._run("build tables", job)

    def _on_preflight(self):
        def job():
            from .preflight import run as preflight_run, PreflightError
            self._preflight_ok = False
            self.log("=== cube solver: preflight ===", "info")
            try:
                preflight_run(self.current_config(), str(self.out_dir),
                              log=self.log)
                self._preflight_ok = True
            except PreflightError as exc:
                self.log("PREFLIGHT FAILED", "err")
                self.log(str(exc), "err")
                self.log("Training is blocked until this is fixed.", "err")
        self._run("preflight", job)

    def _on_verify(self):
        def job():
            from . import verify
            self.log("=== cube solver: verification ===", "info")
            verify.run_all(str(self.out_dir), log=self.log)
        self._run("verify", job)

    def _on_train(self):
        def job():
            from . import train
            from .preflight import run as preflight_run, PreflightError

            cfg = self.current_config()

            # Gate runs regardless of whether the button was clicked. Every
            # check guards a failure whose only symptom is a model that trains
            # well and solves nothing on device.
            self.log("=== cube solver: preflight before training ===", "info")
            try:
                preflight_run(cfg, str(self.out_dir), log=self.log)
                self._preflight_ok = True
            except PreflightError as exc:
                self._preflight_ok = False
                self.log("PREFLIGHT FAILED \u2014 refusing to train", "err")
                self.log(str(exc), "err")
                return

            if not train.HAVE_TORCH:
                self.log("PyTorch not found. Install torch on this machine "
                         "to train; the device never needs it.", "err")
                return

            from .monitor import TrainingMonitor
            self.monitor = TrainingMonitor(cfg, on_advice=self._on_advice)

            # Refresh the readout on a timer rather than per sample, so a fast
            # training loop cannot flood the UI queue.
            def tick():
                if self._worker and self._worker.is_alive():
                    self._push_metrics()
                    self.root.after(1000, tick)
            self.root.after(1000, tick)

            self.log("=== cube solver: teacher ===", "info")
            teacher = train.train_teacher(cfg, None, log=self.log,
                                          monitor=self.monitor)
            self.log("=== cube solver: distilling student ===", "info")
            student = train.distill_student(cfg, teacher, log=self.log,
                                            monitor=self.monitor)
            self.log("=== cube solver: quantisation-aware finetune ===", "info")
            student = train.qat_finetune(cfg, student, teacher, log=self.log,
                                         monitor=self.monitor)
            n = train.export_student(cfg, student, str(self.out_dir),
                                     log=self.log)
            self.log(f"exported {n/1024:.1f} KB of weights", "ok")
        self._run("train", job)


# Standalone harness for iterating on the panel without the installer.
def demo():
    root = tk.Tk()
    root.title("MantisCubeSolver panel")
    root.configure(bg=DARK)
    root.geometry("460x760")

    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass

    box = tk.LabelFrame(root, text=CubeSolverPanel.TITLE, fg=TEXT, bg=DARK,
                        font=("TkDefaultFont", 9, "bold"), bd=1,
                        relief="groove", labelanchor="nw")
    box.pack(fill="x", padx=10, pady=8)

    logbox = tk.Text(root, bg="#0c0c10", fg="#9fe89f", wrap="word",
                     font=("Consolas", 9), relief="flat", state="disabled")
    logbox.pack(fill="both", expand=True, padx=10, pady=8)
    for tag, col in (("err", "#ff8080"), ("ok", "#80ff80"), ("info", "#80c0ff")):
        logbox.tag_config(tag, foreground=col)

    def log(text, tag=None):
        logbox.config(state="normal")
        logbox.insert("end", text, tag) if tag else logbox.insert("end", text)
        logbox.see("end")
        logbox.config(state="disabled")

    CubeSolverPanel(box, log=log, root=root, out_dir="firmware")
    root.mainloop()


if __name__ == "__main__":
    demo()
