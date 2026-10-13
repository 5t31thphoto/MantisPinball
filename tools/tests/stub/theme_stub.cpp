// Test builds: the firmware's runtime palette (theme_runtime.cpp keeps it in
// NVS on the board; host tests just need the default colours to exist).
#include "../../../src/theme_colors.h"
MantisPalette g_theme = { THEME_DEFAULTS };
void themeInit() {}
