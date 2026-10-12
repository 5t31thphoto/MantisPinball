# Resource budget (T-Display S3, no PSRAM, 16 MB flash)

Measured from the real sources (static sizes from compiled objects; heap from
the actual structures). Free heap with the 108 KB framebuffer up is ~38.8 KB
(tools/mem_report.json).

## Static RAM (paid all the time, by everything)
| module | static RAM |
|---|---|
| Mantis Psychic | 5.1 KB (effect + beat buffers) |
| Dance Party | 0.8 KB |
| Cube app | 0.5 KB |
| Mantis Chat, Fireworks | 0.1 KB each |
| Cube solver core/policy/endgame | ~0 (arena is heap, see below) |
| **total for all new apps** | **6.5 KB** |

## Heap while an app is open (freed when you leave it)
| app | heap | left of 38.8 KB |
|---|---|---|
| Mantis Chat | 4.7 KB + ESP-NOW driver | ~34 KB before the driver |
| Dance Party | none + ESP-NOW driver | ~38.8 KB before the driver |
| Fireworks | 10.1 KB (360 particles) | ~28.8 KB |
| Cube solver | 19.1 KB arena + 8 KB task stack | ~11.1 KB |
The ESP-NOW driver cost is the same one Versus and Party already pay on
devices today.

## Flash
| item | flash |
|---|---|
| cube first-layer lookup table | ~240 KB (CI drops it automatically if the 3 MB app partition overflows) |
| cube policy weights | ~85 KB |
| cube endgame tables | ~35 KB |
| all other new app code | ~45 KB |
| Mantis Psychic pack | ~20 KB, in the content partition (LittleFS) |
Content partition: ~0.3 MB used of 9.9 MB.

## Not measurable here
Firmware total size and real heap headroom come from the first CI build
(`pio run` prints both) and from the board. Watch the first build's
"RAM:" and "Flash:" lines.
