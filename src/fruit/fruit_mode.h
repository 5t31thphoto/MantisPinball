// fruit_mode.h - FRUIT FRENZY: gesture-sliced fruit game (PAJ7620U2).
//
// Fruit-ninja style: fruit falls down the screen and you slash it along the
// dashed reticle lines with hand swipes over the gesture sensor plugged into
// the bottom JST.
//
//   LEFT / RIGHT swipe   slash along the horizontal reticle axis
//   UP / DOWN swipe      slash along the vertical reticle axis
//   CW / CCW circle      snap the reticle +/-15 degrees (aim); circle back
//                        to return to center, keep going to tilt the other way
//   PUSH (forward)       confirm in menus; in play, shove a hazard away for
//                        points and an unbroken combo (letting it fall is the
//                        safe play but resets the combo)
//
// Arcade mode: 3 lives, hazards and frenzy powerups. Zen mode: fruit only,
// no lives, chase the high score. Buttons: L/R rotate the reticle as a
// fallback; the OS-wide double-click-both exits to the menu as usual.
//
// The app only appears in the device menu when the sensor hardware is
// actually detected (see fruitSensorPresent / fruitProbeSensor, called by
// main.cpp at boot and every time the menu opens, so hot-plugging works).
#pragma once

void fruitEnter();
void fruitTick();
void fruitDraw();
void fruitShutdown();

// Hardware detection (used by main.cpp to gate the menu entry).
// fruitProbeSensor(): cheap I2C probe (~1 ms); runs the full gesture init the
// first time the sensor appears. Safe to call whenever the menu opens.
void fruitProbeSensor();
// Last probe result: true if a PAJ7620U2 answered on the JST I2C pins.
bool fruitSensorPresent();
