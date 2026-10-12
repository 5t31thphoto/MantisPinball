// ham_mode.h - Ham / shortwave radio propagation dashboard.
//
// Fetches real-time solar-terrestrial data from N0NBH's hamqsl.com XML feed
// (keyless) and shows what HF operators want at a glance: which bands are open
// (80-40m, 30-20m, 17-15m, 12-10m for day and night, rated Poor/Fair/Good),
// plus solar flux, sunspots, K/A index, X-ray, aurora, and VHF/E-skip status.
// Page 1 = band-open table; page 2 = solar/space-weather detail. WiFi creds are
// read from the shared setup (same as Weather).
#pragma once

void hamEnter();
void hamTick();
void hamDraw();
void hamShutdown();
