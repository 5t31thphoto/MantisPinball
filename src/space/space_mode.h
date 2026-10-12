#pragma once
// ============================================================================
//  Space Weather + Seismic Report Mode — public API
//
//  Same lifecycle as weather_mode:
//    spaceEnter()    — enter from menu
//    spaceTick()     — call every loop() frame
//    spaceDraw()     — call every loop() frame
//    spaceShutdown() — call before leaving this mode
// ============================================================================
void spaceEnter();
void spaceTick();
void spaceDraw();
void spaceShutdown();
