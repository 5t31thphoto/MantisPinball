// versus_mode.h - Two-player "air hockey pinball" over ESP-NOW.
//
// Two devices link wirelessly. Each player has a ball on their screen and
// defends their bottom drain with the flippers. When a ball flies off the TOP
// of one screen, it crosses to the opponent's screen (entering from their top,
// travelling down). You score a point when the OPPONENT's ball drains on THEIR
// side... no: you score when YOUR ball drains on the opponent's bottom. First
// to 3 points wins. Going to the menu or sleep ends the match.
//
// Link flow:
//   1. Both players enter Versus mode -> device broadcasts HELLO.
//   2. When a peer is heard, a "CONFIRM LINK" prompt shows the peer's id.
//   3. Both press the confirm button -> match starts, each spawns one ball.
#pragma once

void versusEnter();
void versusTick();
void versusDraw();
void versusShutdown();
