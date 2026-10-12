// backlight.h - Display backlight brightness control (LEDC PWM on TFT_BL).
// Levels: 0=off, 1=dim, 2=mid, 3=bright. Defaults to bright.
#pragma once
#include <stdint.h>

void    backlightBegin();           // attach PWM to TFT_BL (call once at boot)
void    backlightSetLevel(uint8_t level);   // 0..3
uint8_t backlightLevel();
void    backlightCycle();           // off->dim->mid->bright->off ...
