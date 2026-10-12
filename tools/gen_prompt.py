#!/usr/bin/env python3
"""gen_prompt.py - Generate a context-injected prompt to one-shot a device app.

This produces a single, copy-pasteable prompt that gives an LLM EXACTLY the
constraints, conventions, API surface, memory budget, and output format needed
to write a Pinball-S3 app that compiles and runs on the real device the first
time, and imports cleanly into the installer.

It injects live context from:
  - tools/firmware_api.json   (apps, API surface, device facts)   <- gen_api.py
  - tools/mem_report.json     (real heap budget)                  <- memory_report.py
  - the hard-won gotchas encoded below (the rules that actually bite)

Usage:
  python3 tools/gen_prompt.py "MOON PHASE" "shows the current moon phase"
  python3 tools/gen_prompt.py "MOON PHASE" "..." --out tools/app_prompt.txt
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = os.path.join(ROOT, "tools", "firmware_api.json")
MEM = os.path.join(ROOT, "tools", "mem_report.json")


def load(path):
    try:
        return json.load(open(path, encoding="utf-8"))
    except Exception:
        return {}


def api_section(api):
    lines = []
    for group, calls in api.items():
        lines.append(f"  {group}.")
        for c in calls:
            lines.append(f"    {c['sig']}")
            lines.append(f"        # {c['doc']}")
    return "\n".join(lines)


def app_list(apps):
    return "\n".join(
        f"  - {a['name']} (id {a['id']}, {a['category']})"
        + ("  [built-in core game]" if a.get("inlineDispatch") else "")
        for a in apps
    )


def build_prompt(name, desc):
    api = load(API)
    mem = load(MEM)
    apps = api.get("apps", [])
    surface = api.get("api", {})
    dev = api.get("device", {})
    budget = mem.get("budget", {})

    appid = name.strip().lower().replace(" ", "")
    appid = "".join(ch for ch in appid if ch.isalnum() or ch == "_") or "newapp"

    free = budget.get("estFreeHeapBytes", "unknown")
    safe_net = budget.get("safeAppAllocIfNetworking", "unknown")
    safe_non = budget.get("safeAppAllocNoNetworking", "unknown")
    largest = budget.get("largestFreeBlockBytes")

    P = f"""You are writing ONE app ("mode") for a custom ESP32-S3 firmware called
Pinball-S3. Output must compile under PlatformIO (Arduino core 2.0.14) and run
on the real LilyGo T-Display-S3 the FIRST time, with no iteration. Follow every
constraint below exactly. When unsure, choose the smaller, simpler, safer option.

================================================================================
THE APP TO BUILD
================================================================================
Name (menu label):  {name}
Id (lowercase):      {appid}
What it does:        {desc}

Deliver these TWO files, complete, no placeholders:
  src/{appid}/{appid}_mode.h
  src/{appid}/{appid}_mode.cpp

================================================================================
DEVICE (HARD HARDWARE LIMITS)
================================================================================
Board:    {dev.get('board','LilyGo T-Display-S3')}
MCU:      {dev.get('mcu','ESP32-S3')}, Arduino core 2.0.14, PlatformIO espressif32@6.5.0
Display:  {dev.get('display','170x320')} ST7789, portrait. Screen W=170, H=320.
PSRAM:    NONE. This is the single most important constraint.
Flash:    {dev.get('flash','16MB')}

================================================================================
MEMORY BUDGET (REAL NUMBERS — DO NOT EXCEED)
================================================================================
Estimated free heap at runtime:        {free} bytes
Largest single allocatable block:      {largest if largest is not None else 'unknown (assume ~= free heap)'}
Safe to allocate IF your app uses TLS networking:   {safe_net} bytes
Safe to allocate if your app does NOT network:      {safe_non} bytes

RULES THAT ACTUALLY BITE (learned the hard way):
  * The 170x320 frame buffer sprite (~108KB) is ALREADY allocated on the heap.
    You do not create it; you draw into the shared `fb` sprite.
  * NEVER put a buffer larger than ~2KB in static/global scope (BSS). Two 60KB
    static buffers once overflowed internal SRAM by 60,880 bytes. Big buffers
    MUST be malloc'd in your Enter() and free()'d in your Shutdown().
  * A TLS connection (WiFiClientSecure) needs ~40KB of transient heap for its
    handshake. If you network, keep your own allocations small and free the
    big stuff before connecting. Only ONE TLS client alive at a time.
  * NEVER place a large array (>2-3KB) on the stack inside a function that also
    opens a WiFiClientSecure — it overflows the task stack and crashes. Use a
    static or heap buffer instead.
  * For JPEG images, a shared 60KB buffer `g_jpegBuf` exists (extern uint8_t*,
    with g_jpegBufLen). Acquire it on Enter, release on Shutdown. TJpgDec only
    decodes BASELINE JPEG (no progressive/PNG/WebP). When rendering decoded
    blocks into the sprite, call fb.setSwapBytes(true) ON THE SPRITE before
    pushImage, and do NOT also set TJpgDec.setSwapBytes(true) (double-swap =
    broken colors). drawJpg(0,0,...) — the callback applies the origin, so
    passing the origin to drawJpg too double-offsets the image off-screen.

================================================================================
THE APP CONTRACT (what you must implement)
================================================================================
Your .cpp defines exactly these four functions (all `void f()`), declared in
your .h. The firmware calls them:

  void {appid}Enter();      // once when the app opens: init state, malloc buffers
  void {appid}Tick();       // every frame (~60fps): read input, update state
  void {appid}Draw();       // every frame: render into fb, then fb.pushSprite(0,0)
  void {appid}Shutdown();   // once when leaving: free buffers, save state, net off

Skeleton your code MUST match (this is the proven shape):

  // {appid}_mode.h
  #pragma once
  void {appid}Enter();
  void {appid}Tick();
  void {appid}Draw();
  void {appid}Shutdown();

  // {appid}_mode.cpp
  #include <Arduino.h>
  #include <TFT_eSPI.h>
  #include "{appid}_mode.h"
  #include "../theme_colors.h"
  extern TFT_eSprite fb;                       // the shared 170x320 sprite
  extern bool btnL, btnR, btnL_prev, btnR_prev;
  static constexpr int W = 170;
  static constexpr int H = 320;
  // ... your static state ...
  void {appid}Enter()    {{ fb.setRotation(0); /* init */ }}
  void {appid}Tick()     {{ bool lEdge=btnL&&!btnL_prev; bool rEdge=btnR&&!btnR_prev; /* ... */ }}
  void {appid}Draw()     {{ fb.fillSprite(TEAL_DEEP); /* draw */ fb.pushSprite(0,0); }}
  void {appid}Shutdown() {{ /* free/save */ }}

================================================================================
INPUT & GLOBAL GESTURES (NEVER VIOLATE)
================================================================================
Two buttons only: btnL (left), btnR (right). Use EDGE detection:
    bool lEdge = btnL && !btnL_prev;   // rising edge = a tap
    bool rEdge = btnR && !btnR_prev;
Convention used by every app: LEFT = scroll/advance/cycle, RIGHT = select/confirm.
RESERVED, handled globally — your app must NEVER read or act on these combos:
    hold BOTH buttons ~3s  = sleep
    double-click BOTH      = return to menu
So: never use `btnL && btnR` together for anything. Single-button long-press is
OK for a secondary action (e.g. hold-L) if you need one.

================================================================================
DRAWING API (draw into `fb`, a TFT_eSprite; then fb.pushSprite(0,0))
================================================================================
Colors come from theme_colors.h — USE THESE NAMES (don't invent RGB):
    TEAL_DEEP (background), TEAL_DARK, TEAL_LIGHT, TEAL_BRIGHT,
    PURPLE, PURPLE_DARK, PURPLE_LIGHT, LIME, LIME_DARK, LIME_PALE
Common calls:
    fb.fillSprite(TEAL_DEEP);
    fb.fillRect(x,y,w,h,color); fb.fillRoundRect(x,y,w,h,r,color); fb.drawRect(...)
    fb.drawString(text, x, y, font);   // font 1 small, 2 medium, 4 large
    fb.setTextColor(color); fb.setTextDatum(ML_DATUM|MC_DATUM|MR_DATUM|TL_DATUM|TR_DATUM);
    fb.drawLine(...); fb.fillCircle(...); fb.fillTriangle(...);
A typical screen: header bar fillRect(0,0,W,18,PURPLE_DARK) with a title; body;
footer fillRect(0,H-14,W,14,PURPLE_DARK) with "L: ..."/"R: ..." hints.

FIRMWARE API SURFACE available to apps:
{api_section(surface)}

================================================================================
PERSISTENCE & PROFILE (optional, only if your app needs it)
================================================================================
NVS via Preferences: Preferences p; p.begin("yourns", false); p.putBytes(...);
  p.getBytes(...); p.end();  — keep your own namespace, don't touch others.
LittleFS for content packs: fs::File f = LittleFS.open("/content/x.pak","r");
  (note the fs:: qualifier — required on core 2.0.x).
Shared RPG profile (only if relevant): qpAddXp(n), qpAddItem(id,n),
  qpUseItem(id), qpItemCount(id), qpGrantPetXp(n), qpGrantPerk(PERK_*).
  Include "../quests/quest_profile.h" if you use these.

================================================================================
NETWORKING (only if your app needs the internet)
================================================================================
WiFi creds live in NVS namespace "btcminer" keys "ssid"/"pass" (set up by the
miner/weather portals). Read them, WiFi.mode(WIFI_STA); WiFi.begin(ssid,pass);
wait for WL_CONNECTED with a ~15s timeout. Use WiFiClientSecure for https with
c.setInsecure(); follow redirects ITERATIVELY (one TLS client per hop, never two
alive at once). Disconnect + WiFi.mode(WIFI_OFF) in Shutdown(). Browser-style
User-Agent; many CDNs reject generic UAs.
ESP-NOW for device-to-device: WiFi.mode(WIFI_STA), esp_now_init(),
esp_now_register_recv_cb(esp_now_recv_cb_t(yourCb)), broadcast peer FF:FF:FF:FF:FF:FF.
To talk to OTHER Mantis devices (Mantis Chat mesh, Core2 Synapse), speak MantisNow v1
exactly as MANTISNOW.md defines it: lock channel 1 after WiFi.mode(WIFI_STA)
(esp_wifi_set_promiscuous(true); esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
esp_wifi_set_promiscuous(false)), 28-byte packed little-endian header (magic 0x4D,
version 1, kind, flags, srcNode, dstNode, pktId, seq, ttl 0, chanOrSub, callsign[8]),
nodeId = ESP.getEfuseMac() & 0xFFFFFFFF, dedup on (srcNode, pktId) BEFORE dispatch,
drop unknown kinds silently. The receive callback ONLY copies the frame into a ring;
parse in your tick. Copy the bring-up/teardown from src/mantischat/mantischat_mode.cpp
(radioStart/radioStop). Pick a new kind number and add it to MANTISNOW.md.

================================================================================
EXISTING APPS (for style reference; don't duplicate)
================================================================================
{app_list(apps)}

================================================================================
OUTPUT FORMAT (so the installer can import it in one shot)
================================================================================
Output ONLY the two files, each in its own fenced block, with the exact path as
the first comment line. No prose between or after. Example:

```cpp
// FILE: src/{appid}/{appid}_mode.h
#pragma once
void {appid}Enter();
...
```

```cpp
// FILE: src/{appid}/{appid}_mode.cpp
#include <Arduino.h>
...
```

After these two files, output ONE json block describing the app for the manifest:

```json
{{ "id": "{appid}", "name": "{name}", "category": "system" }}
```
(category: one of game, livedata, creative, guide, system)

Do not output anything else. The installer parses the FILE: markers and the json
block to create the files and register the app automatically.
"""
    return P, appid


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    name = sys.argv[1]
    desc = sys.argv[2]
    out = None
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    prompt, appid = build_prompt(name, desc)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(prompt)
        print(f"wrote {out}  (app id: {appid})")
    else:
        print(prompt)


if __name__ == "__main__":
    main()
