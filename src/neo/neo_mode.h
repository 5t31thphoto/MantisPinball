// neo_mode.h - Near-Earth Object (asteroid) close-approach dashboard.
//
// Fetches today's near-Earth asteroids from NASA NeoWs (DEMO_KEY, no signup)
// and shows the closest approach: name, miss distance in lunar distances and
// km, size, relative speed, and whether NASA flags it potentially hazardous.
// Single HTTPS GET on a refresh, parsed without a JSON library to save RAM.
#pragma once

void neoEnter();
void neoTick();
void neoDraw();
void neoShutdown();
