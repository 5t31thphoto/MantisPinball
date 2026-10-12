// net_yield.h - keep the OS responsive inside blocking network waits.
//
// Any synchronous wait loop (WiFi connect polls, HTTP header/body reads) MUST
// call sysNetYield() each iteration and bail out early if sysNetAborted()
// returns true. The yield samples the buttons and runs the both-button
// double-click detector at full rate, so the "exit to menu" gesture works
// even mid-fetch; the abort flag means a double-click landed and the fetch
// should abandon its work immediately (the main loop will route to the menu
// on the very next frame). Self-throttled - safe to call every iteration of
// even the tightest read loop.
#pragma once
void sysNetYield();
bool sysNetAborted();
