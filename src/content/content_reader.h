// content_reader.h - Generic offline article/rabbit-hole reader engine.
//
// One engine drives every offline content mode. A mode is just:
//   1. a .pak file on LittleFS (the content)
//   2. a ContentMode descriptor (pack path, title, ContentKind, theme accent)
//
// The host (main.cpp) calls:
//   contentEnter(&MODE);   on entering the mode
//   contentTick();         every frame (handles buttons/scroll)
//   contentDraw();         every frame (renders to the shared fb sprite)
//   contentShutdown();     on leaving
//
// Buttons (read from main.cpp globals):
//   Topic modes:    R = select highlighted entry / next index page,
//                   L = back to index (or scroll). Long-press handled by host.
//   Sequence modes: R = next step, L = back to index list.
//   Branch modes:   L = take left choice, R = take right choice.
#pragma once
#include "content_format.h"

struct ContentMode {
    const char*  packPath;   // e.g. "/content/techninja.pak"
    const char*  title;      // shown in header
    ContentKind  kind;
    uint16_t     accent;     // theme color for the header band
};

void contentEnter(const ContentMode* mode);
void contentTick();
void contentDraw();
void contentShutdown();

// True once the pack is loaded and at least one entry exists.
bool contentReady();
