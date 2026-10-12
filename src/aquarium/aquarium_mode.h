// aquarium_mode.h - Living aquarium sim toy with a simple game loop.
//
// A tall water column full of swimming fish that you tend over time. Tap RIGHT
// to drop food: fish eat, gain growth, and you earn coins. Tap LEFT to open the
// shop and spend coins on new fish species and decorations (unlockables). Fish,
// coins, and unlocks persist in NVS, so the tank keeps living between sessions.
// Hold LEFT in the tank to toggle an ambient "screensaver" view (no HUD).
#pragma once

void aquariumEnter();
void aquariumTick();
void aquariumDraw();
void aquariumShutdown();
