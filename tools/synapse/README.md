# Synapse: Dance Party sender — drop-in files

Copy into the Synapse repo's `src/` (overwrite main.cpp and mantis.cpp):
- `dance_net.h`, `dance_net.cpp` — NEW: the ESP-NOW DANCE sender
- `mantis.cpp` — your file + the move table and two one-line hooks
- `main.cpp`  — your file + the hold-C party toggle and one line in loop()
Nothing of yours is changed or removed.

## Turn it on
The sender is OPT-IN. Add to `platformio.ini`:
    build_flags =
        ...
        -DMANTIS_DANCE_NET
Without that flag the radio code compiles to nothing and the build is the
same as today.

## Using it
Mantis mode -> HOLD C -> Dance Party on (haptic). Hold C again -> off.
It is off every time you enter mantis mode, and leaving mantis mode turns it
off and releases the radio. C now uses the same edge-detect as B: a tap
counts on RELEASE and a long press never counts as a tap, so a quick C still
changes mode exactly as before (counted on release). All B gestures -
dance/sing/echo cave, hold B in the cave - are untouched.

## Why it won't hurt frame rates
- No <WiFi.h>: raw ESP-IDF radio driver only (small buffers, no NVS, no
  AMPDU, send-only). The earlier version overflowed DRAM by 15 KB.
- Radio up/down (100-300 ms) runs in a one-shot task on core 0 at priority 1,
  below the display (2) and FX worker (3) - it only uses idle time; the main
  loop on core 1 never waits for it.
- A beat is one non-blocking esp_now_send, only after the radio is ready.

## Timing
Beats come from the music: aud::bpm() (60000 / the beat tracker's period),
sent on each real beat of aud::beatPos, and only while grooving - never the
idle counter.

## If the build with -DMANTIS_DANCE_NET overflows DRAM
Send me the "overflowed by N bytes" line; N is exactly what's left to find.
