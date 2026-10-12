# Trained weights - how they were made and verified

`src/rubiks/cube_model.h` was trained with `train_numpy.py` (numpy, 1 CPU core):
31.1M samples, 28 min, last 22% quantisation-aware at the device bit widths
(embed 8-bit, hidden 4-bit per row, output 8-bit per row). Exact architecture
of cube_policy.cpp: no biases (the torch PolicyNet had biases the exporter
drops - this trainer cannot have that mismatch).

Verified before shipping:
- backprop vs numerical gradients: worst relative error 3.5e-8
- host reference == compiled device code, integer logits, 60 random states
- device-integer top-1 on held-out: 32.3% (float 33.4% - QAT held)
- solve rate, real C++ beam search, 24 random 25-move scrambles (srand 7):
    shipped random weights ............ 0/24
    trained, old defaults (depth 24) .. 11/24
    trained, shipped defaults ......... 17/24  avg 28.2 moves, worst 32
  Host time 4.7 s per attempt on x86 (failures run all 8 frames).
  The ESP32-S3 is slower - measure on the board.

Tried and REJECTED: re-scaling beam scores by the renorm shift (1/24 solved).
The raw-logit sum acts as an implicit value signal; keep it.

Keep training (more data -> higher solve rate):
    python -m mantis_cube.train_numpy --resume --ckpt trained_ckpt.npz \
        --minutes 120 --qat-frac 0.2 --out ../../src/rubiks
Host tests (hosttests/) need mcPolicyArenaAcquire() - already added.
