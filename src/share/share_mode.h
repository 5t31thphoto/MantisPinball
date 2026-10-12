// share_mode.h - "Pinball Sharing": hosts a playable HTML Pinball Blaster.
//
// Works exactly like the weather/miner setup portals, but instead of a config
// FORM it serves a self-contained HTML5 canvas game that anyone who joins the
// AP can play in their phone browser. The device screen shows join
// instructions (AP name + URL) and the intro-screen link, same as the weather
// setup screen shows its instructions.
//
// AP name: "pinball-sharing". Captive portal redirects all requests to the
// game page at 192.168.4.1.
#pragma once

void shareEnter();
void shareTick();
void shareDraw();
void shareShutdown();
