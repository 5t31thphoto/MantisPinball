// dice_mode.h - 3D wireframe polyhedral dice roller.
//
// Left button cycles through die types (d4, d6, d8, d10, d12, d20). The screen
// shows the die as a rotating 3D line-art polyhedron. Right button rolls: the
// die tumbles in faux-3D (demoscene-style glowing wireframe with depth-shaded
// edges) and settles on a random face, showing the result.
#pragma once

void diceEnter();
void diceTick();
void diceDraw();
void diceShutdown();
