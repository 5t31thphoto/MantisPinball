#pragma once
// ============================================================================
//  Weather Report Mode
//
//  Free APIs, no registration:
//    Geocoding: geocoding.open-meteo.com/v1/search?name=<zip>&count=1
//    Weather:   api.open-meteo.com/v1/forecast?latitude=X&longitude=Y&...
//
//  WiFi credentials are shared with the miner ("btcminer" NVS namespace).
//  Zip code is stored in the "pinball" NVS namespace under key "wx_zip".
//  Never runs at the same time as the miner.
//
//  Call sequence (mirrors miner API):
//    weatherEnter()    — called from menu when STATE_WEATHER selected
//    weatherTick()     — called every loop() frame while in STATE_WEATHER
//    weatherDraw()     — called every loop() frame while in STATE_WEATHER
//    weatherShutdown() — called before leaving STATE_WEATHER for any reason
// ============================================================================

void weatherEnter();
void weatherTick();
void weatherDraw();
void weatherShutdown();
