// moon_mode.h - Moon phase dashboard (no network; pure astronomy math).
//
// Shows the current moon phase with a drawn moon disc, phase name,
// illuminated %, the moon's age in days, and a countdown to the next full and
// new moon. Computed locally from the device clock, so it works fully offline.
#pragma once

void moonEnter();
void moonTick();
void moonDraw();
void moonShutdown();
