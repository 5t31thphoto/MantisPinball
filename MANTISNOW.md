# MantisNow v1 — decisions both ends implement

This is the contract between the T-Display (Mantis Pinball), the Core2
(Mantis Synapse) and anything else on the Mantis mesh. Where the original
spec left room for interpretation, the decision is recorded here. **Both ends
follow this file.**

## Transport
- ESP-NOW broadcast to `FF:FF:FF:FF:FF:FF`, no encryption, **channel 1**.
- Lock the channel after `WiFi.mode(WIFI_STA)`:
  `esp_wifi_set_promiscuous(true); esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); esp_wifi_set_promiscuous(false);`
- Max frame 250 bytes. Header 28 bytes. Payload ≤ 222 bytes unfragmented.

## Header (28 bytes, little-endian, packed)
| off | size | field |
|---|---|---|
| 0 | 1 | magic `0x4D` |
| 1 | 1 | version `0x01` |
| 2 | 1 | kind |
| 3 | 1 | flags (bit0 fragmented, bit1 ack-requested) |
| 4 | 4 | srcNode |
| 8 | 4 | dstNode (`0xFFFFFFFF` = everyone) |
| 12 | 4 | pktId (random per packet) |
| 16 | 2 | seq (per-sender, +1 per packet) |
| 18 | 1 | ttl (always 0 — no relaying) |
| 19 | 1 | chanOrSub |
| 20 | 8 | callsign (UTF-8, null-padded, no terminator required) |

## DECISION 1 — nodeId
`nodeId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF)`

`getEfuseMac()` returns the factory MAC with MAC byte 0 in the
least-significant byte, so this is `mac[0] | mac[1]<<8 | mac[2]<<16 | mac[3]<<24`.
Same call on every ESP32 Arduino target, so every device agrees without
having to reason about byte order.

## DECISION 2 — PONG token
PONG (kind `0x03`) payload is 8 bytes: **`uint32 pingPktId` then `uint32 pingerNode`**,
both little-endian. PONG is addressed (`dstNode`) to the pinger.

## DECISION 3 — who answers what
- Answer every PING with a PONG.
- ACK (kind `0x05`, payload `uint32 ackedPktId`) **only** when flags bit1 is set
  **and** `dstNode` equals our own nodeId. Never ACK broadcasts.
- No retries at this layer; the sender decides.

## DECISION 4 — receiving
1. Drop if `len < 28`, magic ≠ `0x4D`, or version ≠ 1.
2. Drop frames from our own nodeId (our echo).
3. Dedup on `(srcNode, pktId)` over the last 32 frames, **before** dispatch.
4. Unknown kinds are dropped silently.
5. Fragments (`0x06`): devices that don't need >222-byte messages may drop
   them. Chat text is capped at 191 bytes, so chat never fragments.

## DECISION 5 — chat
- TEXT (`0x01`) payload is 1–191 bytes of text. Channel in `chanOrSub`
  (0 = general, 1–15 app-defined). A DM sets `dstNode` to the recipient and
  `chanOrSub` is ignored.
- A device shows: broadcasts on its current channel, and DMs to itself.

## DECISION 6 — DANCE (new kind 0x30)
For Dance Party, Synapse broadcasts kind **`0x30`**, `dstNode = 0xFFFFFFFF`:

| chanOrSub | meaning | payload |
|---|---|---|
| 0 BEAT | beat tick | `uint16 bpm`, `uint8 beatInBar` (0–3), `uint32 msSinceStart` |
| 1 MOVE | dance move | `uint8 moveId`, `uint8 intensity` (0–255), `uint16 durationMs` |
| 2 START | party started | `uint16 bpm` |
| 3 STOP | party over | — |

Move ids: 0 idle-bob, 1 sway, 2 arms-up, 3 spin, 4 jump, 5 wave, 6 robot,
7 shuffle, 8 head-bang, 9 pose. Unknown ids → the receiver does its closest
move. A receiver that hears no BEAT for 3 s free-runs at the last bpm.

## Radio ownership on the T-Display
Only one app owns the radio at a time. Apps that use MantisNow bring it up
in their Enter and tear it down in their Shutdown, using the same sequence as
Versus (delay 20, unregister callback, deinit, `WiFi.disconnect(true)`,
`WIFI_OFF`). The receive callback only copies the frame into a ring; all
parsing happens in the app's tick.

## Verified
`tools/tests/test_mantischat.cpp` builds frames field-by-field from this file
and checks the chat app against them (header offsets, dedup, channels, DMs,
ACK rules, PING→PONG, unknown kinds, wrong version, send format,
teardown) — 15/15 pass.

## Legacy watches (MantisOS <= 1.5.8, "MNW1") — matches the Core2 MantisChat
197-byte packet: `char magic[4]="MNW1"`, `u8 type` (0 beacon, 1 group,
2 direct), `char callsign[12]`, `char text[180]` — byte-identical to the 1337
watch's mantis_chat.cpp. Sender identity = source MAC (`nodeId` = its low 32
bits, same as v1). T-Display Mantis Chat receives all three types, beacons
MNW1 every 5 s, and sends each group message as v1 TEXT **and** MNW1. The
formats can't collide: MNW1 byte 1 is `'N'`, never version 1. Text stays
<= 178 chars so it fits the legacy packet.

**Duplicate rule (T-Display):** a dual-format sender's group message arrives
twice; the same sender + same text within 3 s is shown once. The Core2
reference (mc_chat.cpp `chat_on_text`) does not do this yet, so two Core2s
show each other's group messages twice.

## Whiteboard (kind 0x40)
See WHITEBOARD.md (copied from the Core2 MantisChat repo). The T-Display is a
view-only participant: it sends SYNC when you open the board (BD key on the
symbols page), keeps strokes in an 8 KB pool only while the board is open,
and applies the reference's ownership, tombstone and CLEAR-watermark rules.
It never sends HELLO, SEG, DEL or CLEAR.
