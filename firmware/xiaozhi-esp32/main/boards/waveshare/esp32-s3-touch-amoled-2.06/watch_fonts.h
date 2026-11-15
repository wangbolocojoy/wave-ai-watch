#pragma once

#include <lvgl.h>

struct WatchFonts {
    const lv_font_t* small = nullptr;   // native 14px
    const lv_font_t* medium = nullptr;  // native 20px
    const lv_font_t* large = nullptr;   // native 30px
};

// Loads crisp native CBIN fonts from the assets partition. Safe to call once
// after Assets::Apply(); returns an empty set when the fonts are unavailable.
void LoadWatchFonts();
const WatchFonts& GetWatchFonts();
