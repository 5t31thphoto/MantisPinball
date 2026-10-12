// extras_mode.h - Three live-data modes: NASA APOD, Flight Tracker, Crypto Candles.
//
// All three share the WiFi + HTTPS plumbing pattern used by the other network
// modes. Each has the standard enter/tick/draw/shutdown lifecycle and is driven
// from main.cpp's dispatch.
#pragma once

// NASA Astronomy Picture of the Day
void apodEnter();
void apodTick();
void apodDraw();
void apodShutdown();

// Flight tracker (planes overhead via OpenSky)
void flightEnter();
void flightTick();
void flightDraw();
void flightShutdown();

// Crypto candles (Heikin-Ashi + MACD via CoinGecko OHLC)
void candlesEnter();
void candlesTick();
void candlesDraw();
void candlesShutdown();
