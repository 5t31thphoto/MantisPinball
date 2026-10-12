#pragma once
// ============================================================================
//  Rabbit Hole Mode — public API
//
//  Portrait mode (170×320), reuses main.cpp fb sprite directly.
//  Button contract (handled inside rabbit mode, not by main.cpp):
//    R single click  — go deeper (follow a link in current article)
//    L single click  — new random article in same topic
//    R or L long press (1.5s) — step to next topic
//    Both buttons double-click — caught by main.cpp → opens menu (exit)
//    Both buttons 3s — caught by main.cpp → sleep
//
//  rabbitEnter()    — called from menu
//  rabbitTick()     — called every loop() frame
//  rabbitDraw()     — called every loop() frame
//  rabbitShutdown() — called before leaving this mode
// ============================================================================
void rabbitEnter();
void rabbitTick();
void rabbitDraw();
void rabbitShutdown();
