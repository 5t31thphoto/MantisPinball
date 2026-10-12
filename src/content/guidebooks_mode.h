// guidebooks_mode.h - GuideBooks: one menu listing every offline guide.
//
// Replaces the individual Scout/Survival/First Aid menu entries with a single
// "GUIDEBOOKS" app that shows a selectable list of guides (like the CYOA
// story-select menu). Picking a guide loads its .pak into the shared content
// reader. Which guides appear is controlled by GUIDE_MASK (set by the
// installer, same mechanism as the rabbit-hole topic mask), so guides can be
// included/excluded without spending menu slots.
#pragma once

void guidebooksEnter();
void guidebooksTick();
void guidebooksDraw();
void guidebooksShutdown();
