# MantisCubeSolver

A Rubik's cube solver for the LilyGo T-Display-S3, built for Mantis Crypto
Pinball OS.

The device has 512 KB of SRAM shared with 25 other apps, a 108 KB framebuffer
sprite, and **no PSRAM**. Classical cube solvers assume none of those things.
This one is designed around them.

---

## The idea

Every constant in classical cube solving encodes a 1992 machine: abundant RAM,
slow arithmetic. Kociemba's coordinates are dense integer indices because
that's what indexes an array. Pattern databases are megabytes because IDA*
requires an admissible heuristic and admissibility forces you into *product*
coordinates.

We have the inverse machine. Arithmetic is cheap — the ESP32-S3 has a 128-bit
SIMD unit built for int8 inference — and random access into a large table is
the one thing it does badly. So every one of those trades gets re-examined in
the opposite direction.

**No published table ships.** The classical tables appear in exactly one place,
`verify.py`, as an oracle for checking our own work.

| Classical choice | Why it existed | What we do instead |
|---|---|---|
| Megabyte pruning tables | IDA* needs admissibility | Learned policy, computed in registers |
| Predict distance-to-goal | Heuristic search wants a cost | Predict the *next move* — one pass ranks all 18 children |
| Two phases | Made tables tractable | A swept parameter, not a law |
| Ship the unfolded table | Symmetry recovery is slow on scalar CPUs | 24 conjugations = 24 vector passes, nearly free |
| Exact tables everywhere | Admissibility | Beam search needs ordering, not magnitude |

---

## Architecture

**Endgame tablebase.** Every state within 5 moves of solved — 621,649 of them
— folded by the 24 rotational symmetries into 26,098 classes, indexed by a
minimal perfect hash. **No class keys are stored.** Lookup is self-verifying:
follow the chain, and if you don't reach solved, the state wasn't in the ball.
That makes membership testing free and drops the index from 522 KB to 12.7 KB.

**Policy network.** Embeddings keyed on our own slot coordinates, which are
simultaneously a lookup table and the network's first layer. One forward pass
on a parent yields 18 logits, ranking every child without generating any of
them. Trained by distillation from a large teacher, then quantisation-aware
finetuned to per-layer bit depths.

**Beam search.** The policy orders candidates; the tablebase terminates
exactly. The model never has to be precise near the goal, which is where a
small model is weakest and where an earlier hand-crafted heuristic measurably
plateaued.

---

## Measured budgets

| Component | Flash | SRAM | Status |
|---|---|---|---|
| endgame tablebase (depth 5) | 33.1 KB | 0 | verified |
| symmetry tables | 1.9 KB | 0 | verified |
| policy weights (balanced preset) | 86.1 KB | 0 | needs training |
| beam arena @ W=192 | 0 | 20.5 KB | verified |
| **total** | **~121 KB** | **~21 KB** | |

Everything is statically allocated. No heap, no PSRAM, no runtime table
generation. If it links, it fits.

---

## Layout

```
mantis_cube/          host-side; runs in the tkinter tab
  geometry.py         move tables derived from 3D rotations, not transcribed
  model.py            cubie state, facelet validation, coordinates
  endgame.py          symmetry group, tablebase, self-verifying lookup
  emit.py             CHD perfect hash, bit packing, C++ emitters
  config.py           every swept parameter and the budget checker
  train.py            teacher -> distillation -> QAT -> export
  build.py            build orchestrator
  verify.py           verification suite (the only user of published tables)
  panel.py            installer panel (borrows host log, paths, theme)
  integrate.py        idempotent installer patcher
  monitor.py          training telemetry and diagnostic advisor
  tables.py           classical coordinate tables — ORACLE ONLY

firmware/             portable C++; compiles for S3 and for the host
  cube_core.{h,cpp}       state, moves, facelet validation
  cube_endgame.cpp        symmetry canonicalisation + tablebase
  cube_policy.{h,cpp}     quantised inference + beam search
  cube_*.h                generated resources
```

---

## Installing into the Pinball-S3 installer

One command does all of it:

```bash
python -m mantis_cube.integrate tools/installer.py
```

It is idempotent, writes an `installer.py.bak` first, and supports `--check`
and `--revert`. Four edits get made, and the panel is only one of them - the
other three are what actually get the app onto the device:

| Edit | Why |
|---|---|
| `MENU_MODES` entry | gives the mode a checkbox and an `ENABLE_RUBIKS` build flag |
| `flag_meta` entry | so `_write_build_config()` emits the `PINBALL_MENU_ITEMS` line that `main.cpp` expands into the device menu |
| `_build_ui()` call | mounts the panel in the left control rail, after the SDK panel |
| `_build_cube_panel()` method | the mount itself, wrapped so a missing `mantis_cube` degrades to a notice rather than stopping the installer from opening |

It also cross-checks that every `MENU_MODES` flag has a `flag_meta` entry,
since a missing one is a `KeyError` at upload time - after the user has
already configured everything.

Firmware side still needs, by hand:

* `STATE_RUBIKS` in the mode enum
* the mode registration guarded with `#if ENABLE_RUBIKS`
* `src/rubiks/*.cpp` in the build

To mount the panel manually instead:

```python
from mantis_cube.panel import CubeSolverPanel
self.cube_panel = CubeSolverPanel.attach(self)
```

`attach()` picks up the installer's `_section()` styling, its `_log()` with
the existing `err` / `ok` / `info` tags, its root window, its theme constants,
and `SRC_DIR`. Build output goes to the same BUILD / UPLOAD LOG as everything
else - there is no second log to watch.

For a container that is not a `_section()` - a tab body, or any frame - place
it explicitly:

```python
CubeSolverPanel(parent_frame, log=self._log, root=self.root,
                out_dir=SRC_DIR / "rubiks")
```

Only `parent` is required; `log`, `root`, `out_dir` and `theme` each fall back
to a local default, so the panel also runs standalone while iterating:

```bash
python -m mantis_cube.panel
```

**Workflow, left to right across the button row:**

| Control | What it does |
|---|---|
| (live readout) | Params, flash, SRAM and estimated solve time update as you change any setting. Turns red and names the problem when a setting exceeds budget. |
| Build Tables | Generates the endgame tablebase and symmetry tables into the output directory, streaming progress to the installer log. |
| Preflight | Runs the full safety gate. |
| TRAIN MODEL | Runs preflight first and **refuses to train if it fails**, then teacher, distillation, QAT, and export. |
| (telemetry) | Live step, loss, accuracy and ETA per stage, refreshed once a second. |
| (advisor) | Interprets the numbers as they arrive and says what to change. |
| Verify | Full verification suite against the reference model. |


Changing any setting invalidates a previous preflight pass, so a stale
configuration cannot slip through.

Changing any setting invalidates a previous preflight pass, so a stale
configuration cannot reach training.

The same operations exist as module entry points (`mantis_cube.build`,
`mantis_cube.preflight`, `mantis_cube.train`, `mantis_cube.verify`) for
scripting and CI, and enforce identical gates.

Host-compile the solver exactly as the firmware builds it:

```bash
g++ -O2 -Ifirmware firmware/cube_core.cpp firmware/cube_endgame.cpp \
    firmware/cube_policy.cpp your_test.cpp -o solver
```

---

## What the advisor watches

Training failures are cheap to catch in the first few minutes and expensive to
discover at the end. The monitor ingests loss and accuracy from all three
stages and raises the following, each at most once per stage:

| Condition | Verdict | Suggested action |
|---|---|---|
| loss is NaN or infinite | error | stop; drop `teacher.lr` 10x |
| loss more than 3x its starting value | error | stop; lower LR or check gradient clipping |
| accuracy near 1/18 after 4k steps | error | data bug, not capacity — re-run preflight |
| teacher below 45% top-1 at 60k steps | warning | raise `teacher.hidden` before distilling |
| teacher accuracy flat and under 55% | warning | more steps will not help; change capacity |
| teacher at or above 60% top-1 | ok | on track |
| loss slope flat past 10k steps | warning | model saturated; raise hidden or embed_dim |
| distillation loss rising | warning | lower student LR or raise KD temperature |
| QAT loss more than 2.5x pre-QAT | error | `hidden_bits` too aggressive; raise it |
| QAT loss 1.4x to 2.5x pre-QAT | warning | expected, but watch the final solve rate |
| QAT loss at or below pre-QAT | ok | quantisation is free; fewer bits may work |

The rules carry no torch dependency and are tested against synthetic runs, so
each one is verified to fire on the condition it describes.

---

## Verification status

| Check | Result |
|---|---|
| move tables, group laws + superflip | pass |
| vectorised moves vs reference | pass |
| facelet round-trip, 500 random cubes | pass |
| symmetry conjugation, all 24 elements | pass |
| coordinates vs classical definitions | pass |
| endgame depth 4, 400 samples | 400/400 optimal, 0 wrong |
| firmware endgame, 5000 scrambles | 5000/5000 optimal, 0 wrong |
| firmware endgame, 1000 random cubes | 1000/1000 correctly rejected |
| end-to-end beam + endgame (random weights) | 30/30 at scramble depth 6 |

---

## What is not done

The policy weights are **random**. `build.py` emits a stub so the firmware
compiles and the search plumbing can be exercised; solve rates with random
weights are meaningless beyond proving the plumbing. Training requires a GPU
and has not been run.

~~`cube_core.cpp` still contains the older heuristic beam search~~ — already
removed; `cube_core.cpp` now exports only state, moves and facelet validation.

Expected solution length once trained is **22–27 moves**, based on the
published scaling behaviour of comparable policies and the fact that the
endgame contributes optimal final segments. That estimate is not yet measured.

~~Multi-frame restarts and symmetry dedup are designed but not implemented~~ —
both are implemented (`opts.frames`, `opts.symmetryDedup`, used in
`mcBeamOnce`). Dual-core search is in place: the solve runs on its own
FreeRTOS task pinned to core 0 with `vTaskDelay` yields, so the 16 ms UI frame
loop is never blocked. Solve-during-input is still not implemented.

**Memory, corrected for this device.** The search arena was 24.6 KB of static
BSS. `mem_report.json` is explicit that big buffers are malloc'd on Enter and
freed on Shutdown and never left in BSS — static BSS is permanent and would
have cut the free heap for all 25 other apps for the whole runtime. The arena
is now one heap block taken in `rubiksEnter()` and released in
`rubiksShutdown()`; static BSS is down to 8 bytes. The shipped beam width is
128 (19.1 KB arena) rather than 192 (24.6 KB), because the solver also takes an
8 KB task stack and the heap has ~38.8 KB free with the framebuffer up — 128
leaves ~11 KB of headroom, 192 left ~6 KB. Build with `-DMC_BEAM_W=192` if a
device measurement shows more room.

---

## References

The approach draws on DeepCubeA (McAleer et al., *Nature Machine
Intelligence*, 2019) for self-supervised training from scrambles off the goal
state, and EfficientCube (Takano, *TMLR*, 2023) for the policy-over-value
formulation and beam search. Kociemba's group decomposition informs the
coordinate design. None of their tables are used at runtime.

---

## Changelog — implementation pass 2

Added since the first package:

* **Multi-frame restarts.** Sequential, early-exit, with the solution verified
  in the original frame before acceptance. Solve rate on shallow scrambles rose
  from 36/40 to 40/40 with frames alone.
* **Symmetry dedup of the frontier.** Canonical-representative fingerprints in
  a 4 KB open-addressed set, cleared per ply.
* **Dual-core execution.** `cube_task.h` pins the solver to core 0 via
  FreeRTOS, yields for the watchdog, and publishes monotonic progress. The
  host build runs the identical solver synchronously.
* **Solve-during-input.** The search starts the instant VERIFY opens, so the
  answer is normally ready before the user finishes checking six faces.
* **UI integration.** `rubiks_mode.cpp` rewritten against the new solver, with
  a progress bar, live frame/depth/best-length telemetry, backface culling in
  the 3D preview, and validation errors that name the offending square and
  jump the cursor there.

Fixed:

* **Hidden-layer weight pointers** were hardcoded to two layers with null
  entries beyond; any config with three or more layers would have dereferenced
  null at runtime. Now emitted as a generated pointer table.
* **Weight tensor layout** disagreed between the stub emitter `(in, out)` and
  the PyTorch export `(out, in)`. Unified on PyTorch's layout.
* **Quantisation bit depths** were a manual edit that regeneration erased.
  Now emitted from `SolverConfig.quant`.
* **Dead heuristic beam** removed from `cube_core.cpp` (8.7 KB of source).

---

## Changelog — implementation pass 3 (training readiness)

Fixed, each of which would have produced weights that train well and solve
nothing on device:

* **Quantisation scales were emitted and ignored.** Inference ran in raw
  integer space with a hardcoded shift, so per-channel scales - the default -
  were silently discarded. Replaced with integer per-channel requantisation:
  `y[o] = (acc[o] * q[o]) >> 15`, where `q[o]` is the channel scale relative
  to the layer maximum. No floats, no activation calibration, nothing that can
  drift between host and device. A uniform renormalising shift after ReLU
  keeps the next matmul inside int32 without disturbing ranking.
* **QAT did not cover the embeddings.** They were post-hoc quantised at export,
  which is the exact cliff QAT exists to avoid. Now simulated in the forward
  pass alongside the hidden and output weights.
* **Scrambles allowed cancellations.** Moves were drawn uniformly with no
  same-face filter, so sequences contained `U U'` pairs and the label - the
  inverse of the last move - was frequently not a move toward the goal.
  Generation now uses the same canonical filter as the beam. Degenerate
  already-solved states in a batch dropped from nonzero to zero.
* **Exact tablebase supervision was unused.** `BatchSource` now substitutes
  known-optimal moves for any state inside the endgame ball, covering the
  region where a small policy is weakest.
* **Two quantisation code paths.** Export quantised independently of the
  emitter. Now the emitter owns it, `policy_ref.py` shares that path, and
  preflight proves it matches compiled device code.

Added:

* **`policy_ref.py`** — bit-exact numpy reimplementation of `mcPolicy()`.
* **`preflight.py`** — hard gate run automatically by `train.py`. Checks
  budget, tablebase presence, slot-encoding agreement, quantisation ranking
  survival, and bit-exact host/device logit agreement over random states and
  weights. Training refuses to start if any check fails.

Preflight has been negative-tested against a transposed weight layout, a
dropped per-channel multiplier, and a slot-encoding drift. All three are
caught.

---

## Changelog — implementation pass 4

* **Preflight was only enforced on the command line.** The tkinter tab - the
  path actually used - called the training functions directly, so every check
  added in pass 3 could be bypassed by clicking the button. The tab now runs
  the gate itself before training and refuses to proceed on failure, and
  changing any setting invalidates a prior pass.
* **`representation.slot_specific` was a dead config knob.** It appeared in
  the schema and in `embedding_params`, but the model always built per-slot
  tables regardless. It is now real (per-slot, or two shared tables for
  corners and edges), honoured by the host reference, and preflight rejects
  the shared-table setting because `cube_policy.cpp` does not implement it -
  rather than silently training a model the firmware cannot evaluate.
* Added a **Save config** button so a parameter set can be recorded alongside
  the weights it produced.

Confirmed: the specialised embedding is what trains. `PolicyNet.embed` is a
`(20, 24, embed_dim)` parameter - twenty per-slot tables keyed on the
coordinates we designed - learned jointly with the network and quantised under
QAT. At the balanced preset that is 7,680 of the student's 160,274 parameters.

---

## Changelog — implementation pass 5 (installer integration)

The previous `tab.py` was a standalone application wearing a `Frame` costume.
It created its own log widget, its own path entry field, its own worker thread
and queue, and shipped a `demo()` with its own `Tk()` root — none of which
belong in something mounted inside another program. Output would have been
split across two logs, and the installer could not have tracked or cancelled
a build.

Replaced with `panel.py`, built against the installer's actual conventions:

* `CubeSolverPanel.attach(installer)` mounts it in one line using the host's
  `_section()`, `_log()` (with the existing `err` / `ok` / `info` tags), root
  window, theme constants and `SRC_DIR`.
* Every dependency is injected with a local fallback, so the same class works
  inside a `_section()`, inside a tab body, or standalone.
* Widgets follow the installer's styling: `tk.Button(bg=PANEL, fg=TEXT,
  relief="flat")`, `tk.Label(fg=TEXT, bg=DARK, font=("TkDefaultFont", 8))`,
  an accent-coloured primary action, and a `ttk.Progressbar`.
* Worker output is queued and drained on the host root, matching the upload
  worker's discipline — no Tk calls from a background thread.
* The budget readout is live rather than a button: params, flash, SRAM and
  estimated solve time update as settings change, and turn red naming the
  problem when a limit is exceeded.

---

## Changelog — implementation pass 6

* **`integrate.py`** — patches `installer.py` automatically. Idempotent, makes
  a backup, supports `--check` and `--revert`. Tested against the real
  installer: applies cleanly, second run is a no-op, patched file parses.
* **Registration was missing entirely.** The panel alone would have rendered
  controls that generate headers for a mode with no build flag and no menu
  entry — nothing would have appeared on the device. `MENU_MODES` and
  `flag_meta` edits are now part of the integration.
* **flag_meta consistency check.** Verifies every `MENU_MODES` flag has a
  `flag_meta` entry before patching, since a gap there is a `KeyError` raised
  during upload rather than at edit time.
* **`attach()` hardened** to resolve `SRC_DIR` and the theme from whichever
  module defines the Installer, so it works whether `installer.py` runs
  directly as `__main__` or is imported.

---

## Changelog — implementation pass 7 (training telemetry)

* **`monitor.py`** — ingests loss and accuracy from all three training stages,
  tracks per-stage history, computes trend slopes and ETA, and raises
  diagnostic advice with a concrete suggested action. No torch dependency.
* **All three stages now emit metrics.** `train_teacher`, `distill_student`
  and `qat_finetune` take an optional `monitor` and report every 2,000 steps;
  distillation and QAT now also report accuracy, which they previously did not.
* **Panel shows it live.** A telemetry line per stage refreshed once a second,
  plus a colour-coded advisor line. Widget updates are queued from the training
  thread and applied on the Tk thread, matching the installer's upload-worker
  discipline, and the refresh is timer-driven so a fast loop cannot flood the
  queue.
* **Rules verified against synthetic runs** — healthy teacher, low plateau,
  NaN, exploding loss, accuracy pinned at chance, QAT blowup, and QAT clean
  each confirmed to fire on their own condition and no other.
