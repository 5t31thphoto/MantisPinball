// theme_runtime.cpp - the device colour scheme, chosen at FLASH time and kept
// by each board across updates.
//
// THEME_BLOB is a tagged block in the firmware image ("MANTIS_THEME_1"). The
// tkinter installer compiles the chosen colours into it; the website's colour
// picker patches it before flashing (website/js/fwpatch.js), exactly like the
// splash text. At boot themeInit() decides which palette to use:
//   * the block is EXPLICIT (someone chose colours for this flash) and differs
//     from what the board saved  -> adopt it and save it
//   * otherwise, the board has saved colours -> keep them (an update that
//     didn't choose colours never resets a kid's colour scheme)
//   * otherwise (first boot) -> use the block's colours and save them
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "theme_colors.h"

struct ThemeBlob {
    char     magic[16];      // "MANTIS_THEME_1"
    uint32_t version;        // 1
    uint32_t explicitChoice; // 1 = colours were chosen for this flash
    uint32_t nonce;          // identifies the colour choice
    uint16_t pal[14];        // TEAL_DEEP ... LIME_PALE (rgb565)
};
static_assert(sizeof(ThemeBlob) == 56, "theme blob layout is shared with fwpatch.js");

static const ThemeBlob THEME_BLOB __attribute__((used, aligned(4))) = {
    "MANTIS_THEME_1", 1, THEME_EXPLICIT, THEME_NONCE, THEME_DEFAULTS
};

MantisPalette g_theme = { THEME_DEFAULTS };   // defaults until themeInit() runs

void themeInit() {
    // read through a volatile pointer: the bytes may have been patched in the
    // image after compilation, so the compiler must not fold them
    const volatile ThemeBlob* b = &THEME_BLOB;
    uint16_t blobPal[14];
    for (int i = 0; i < 14; i++) blobPal[i] = b->pal[i];
    const uint32_t explicitChoice = b->explicitChoice, nonce = b->nonce;

    uint16_t saved[14]; bool haveSaved = false; uint32_t savedNonce = 0;
    Preferences p;
    if (p.begin("theme", true)) {
        haveSaved = p.getBytes("pal", saved, sizeof(saved)) == sizeof(saved);
        savedNonce = p.getUInt("nonce", 0);
        p.end();
    }
    const bool adopt = explicitChoice && nonce != savedNonce;
    if (haveSaved && !adopt) {
        memcpy(g_theme.s, saved, sizeof(saved));
        return;
    }
    memcpy(g_theme.s, blobPal, sizeof(blobPal));
    if (p.begin("theme", false)) {                 // a single write, at boot only
        p.putBytes("pal", blobPal, sizeof(blobPal));
        p.putUInt("nonce", nonce);
        p.end();
    }
}
