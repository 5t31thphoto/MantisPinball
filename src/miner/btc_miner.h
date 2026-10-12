// ============================================================================
//  btc_miner.h
//
//  Public interface for the BTC Solo Mining mode.
//
//  ALL mining work happens inside this module. From main.cpp you only ever
//  call:
//      minerEnter()       - start mining mode  (called when user picks BTC MINER)
//      minerTick()        - per-frame logic    (input handling, state updates)
//      minerDraw()        - per-frame render   (fills the framebuffer + pushes)
//      minerShutdown()    - clean stop         (called on exit via double-click)
//      minerIsRunning()   - bool query
//
//  The module owns its own WiFi connection, captive portal, NVS-backed
//  configuration, FreeRTOS hash task, and Stratum client. It restores the
//  display rotation on shutdown so the rest of the firmware behaves
//  exactly as it did before mining mode was added.
//
//  Pinball, Corner Pocket, and Slots NEVER touch any of this code. The
//  whole module is additive - if the user never opens MINER mode, no
//  WiFi, no portal, no extra CPU usage.
// ============================================================================
#pragma once

#include <stdint.h>

void minerEnter();
void minerTick();
void minerDraw();
void minerShutdown();
bool minerIsRunning();
