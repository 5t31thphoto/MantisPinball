// content_format.h - On-flash content pack format for offline reader modes.
//
// Every offline "rabbit-hole-style" mode (Tech Ninja, Scout Book, Survival,
// First Aid, Recipes, Bushcraft, Yoyo, Paper Airplanes, CYOA story, etc.)
// reads a content pack from the LittleFS "content" partition instead of
// compiling the text into the firmware binary. This keeps the binary small
// and lets the installer choose which packs to flash.
//
// ---- Pack file layout (one .pak file per mode, plain UTF-8 text) ----------
//
// A pack is a flat text file with entries separated by a form-feed-like
// delimiter. Each entry has a header line then a body. Entries can optionally
// declare branch choices (for the choose-your-own-adventure story) and a
// step list (for paper airplanes / how-to sequences).
//
//   @PACK <pack title>
//   @ENTRY <id>|<title>
//   <body text, may span many lines>
//   @CHOICE <button:L|R>|<label>|<goto-id>      (optional, for branching)
//   @STEP <step text>                            (optional, for sequences)
//   @END
//
//   ...more @ENTRY blocks...
//
// The first @ENTRY is the index/landing entry. Index entries typically use
// @CHOICE lines to link to topic entries. Reader navigation:
//   - Topic/list modes: index lists entries; R selects, L goes back.
//   - Sequence modes (airplanes): R = next @STEP, L = back to index.
//   - Branching (CYOA): each page shows body + 2 @CHOICE buttons (L / R).
//
// Bodies are word-wrapped by the reader at runtime; keep raw text unwrapped.
#pragma once
#include <stdint.h>

// Maximum sizes (tuned for no-PSRAM: these live in static buffers)
static constexpr int CR_MAX_ENTRIES   = 200;   // entries indexed per pack
static constexpr int CR_MAX_BODY      = 4096;  // bytes of body for one entry
static constexpr int CR_MAX_CHOICES   = 4;     // branch choices per entry
static constexpr int CR_ID_LEN        = 24;
static constexpr int CR_TITLE_LEN     = 48;

enum ContentKind {
    CK_TOPICS,    // index -> entries, L back / R select (Tech Ninja, Scout...)
    CK_SEQUENCE,  // index -> pick -> step-by-step (paper airplanes, how-to)
    CK_BRANCH,    // branching pages with L/R choices (CYOA story)
};
