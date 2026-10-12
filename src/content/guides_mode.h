// guides_mode.h - Offline guide modes (Scout Book, Survival, First Aid).
// These are thin wrappers over the generic content-reader engine; each just
// points the engine at its .pak file on the LittleFS content partition.
#pragma once

void scoutEnter();    void scoutTick();    void scoutDraw();    void scoutShutdown();
void survivalEnter(); void survivalTick(); void survivalDraw(); void survivalShutdown();
void firstaidEnter(); void firstaidTick(); void firstaidDraw(); void firstaidShutdown();
