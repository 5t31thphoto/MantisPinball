// pet_mode.h - The mantis pet: care, mini-games, and ESP-NOW multiplayer.
//
// Tamagotchi controls: LEFT scrolls the action ring, RIGHT confirms. Actions:
//   Feed | Clean | Play | Train | Battle | Trade | Stats
// Train  = single-player practice battle that grants pet XP.
// Battle = Pokemon-style turn battle (Attack/Guard/Special); vs CPU or, when a
//          peer is linked over ESP-NOW, vs another device's mantis.
// Trade  = swap pet consumable items with a linked device over ESP-NOW.
#pragma once

void petEnter();
void petTick();
void petDraw();
void petShutdown();
