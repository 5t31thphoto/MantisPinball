# Cube solver - honest status and game plan

## Where it stands (all numbers from the real C++ solver, host bench)
| | solved (24 random 25-move scrambles) | time/attempt (x86) |
|---|---|---|
| weights that shipped in the archive | 0/24 | 7.0 s |
| trained on CPU, old search settings | 11/24 | 6.3 s |
| trained, tuned settings | 17/24 | 4.7 s |
| + exact fast forward pass + dedup fix (SHIPPED) | 17/24 | **2.1 s** |
| same, 16 frames (not shipped: +1 is within noise) | 18/24 | 3.1 s |

Speedups are exact: the fast forward pass is bit-identical to the original
on 50,000 random states (the original is kept as mcPolicyGeneric()).

## Why it isn't a showpiece yet
The DESIGN is right for an S3: a 150K-parameter policy + exact endgame +
symmetry dedup + beam search in 19 KB of heap, no megabyte tables.
The TRAINING is starved: ~40M samples on one CPU core. Methods like this are
trained on hundreds of millions to billions of positions. Held-out accuracy
has been flat at ~32.5% for the last 20M samples, so more of the same CPU
training will not get there - it needs (a) far more data and (b) probably a
larger network, which is exactly what a GPU makes cheap to test.

## The plan
1. **GPU training (biggest lever).** `tools/mantis_cube/train_gpu.py` on the
   RTX 5050. Generates scrambles on the GPU; refuses to train until it proves
   its moves, encoding and forward pass match the numpy reference (which is
   verified against the compiled device code). Start from the CPU weights:
       python -m mantis_cube.train_gpu --init-npz mantis_cube/trained_ckpt.npz --hours 3 --out ../src/rubiks
   Then try --hidden 384,384 and 512,256: bigger = smarter but slower on the
   S3, so each size gets measured on the board.
2. **Measure on the board.** Time per solve on the S3 is the number that
   decides model size and frame count. Nothing here can measure it.
3. **S3 SIMD for the forward pass.** The ESP32-S3 has 128-bit SIMD (PIE).
   The forward pass is ~83% of solve time and is a pure int8/int4 dot
   product - the case that SIMD accelerates most.
4. **Search tuning on real data.** Frames, depth and beam width re-tuned on
   larger test sets once the GPU weights exist.

GitHub Actions runners are CPU-only (4 cores, 6-hour jobs): roughly 4x one
core, useful but the same plateau risk as CPU training here. The GPU is the
real path.
