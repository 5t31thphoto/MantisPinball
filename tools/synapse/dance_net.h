// dance_net.h - Mantis Synapse -> MantisNow DANCE sender (see MANTISNOW.md).
// Lets T-Display "Dance Party" mantises dance in time with this one.
//
// OPT-IN: compiled only with -DMANTIS_DANCE_NET (platformio.ini build_flags).
// Without it these calls are empty inlines and the build is exactly the same
// as before - the radio driver is never linked.
#pragma once
#include <stdint.h>
#ifdef MANTIS_DANCE_NET
void danceNetService(bool partyOn);            // call every loop: radio up/down
void danceNetBeat(uint16_t bpm, uint8_t beatInBar);
void danceNetMove(uint8_t moveId, uint8_t intensity);
bool danceNetUp();
#else
static inline void danceNetService(bool) {}
static inline void danceNetBeat(uint16_t, uint8_t) {}
static inline void danceNetMove(uint8_t, uint8_t) {}
static inline bool danceNetUp() { return false; }
#endif
