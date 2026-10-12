// clicker_mode.h - Dual click counter.
//
// Two independent tally counters, one per button. Tapping a button increments
// its count; long-pressing a button resets that side to zero. Counts persist
// in NVS across power cycles. The left counter uses the header's teal accent,
// the right uses the header's purple accent.
#pragma once

void clickerEnter();
void clickerTick();
void clickerDraw();
void clickerShutdown();
