// cyoa_mode.h - Choose-Your-Own-Adventure with the collectible-dice engine.
//
// A branching story where some choices are skill checks resolved by rolling
// the player's equipped dice (from the quest profile). Success/failure routes to
// different pages. Finishing arcs grants XP and can unlock dice/perks shared
// across the whole device.
//
// Buttons follow the device convention:
//   - Normal page: L = first choice, R = second choice.
//   - Roll page:   R = roll the dice, then L or R = continue per the outcome.
//   - Dice/level HUD is shown on a banner so progression is always visible.
#pragma once

void cyoaEnter();
void cyoaTick();
void cyoaDraw();
void cyoaShutdown();
