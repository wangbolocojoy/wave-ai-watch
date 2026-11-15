#pragma once

#include <lvgl.h>

struct WatchFonts {
    const lv_font_t* small = nullptr;
    const lv_font_t* medium = nullptr;
    const lv_font_t* large = nullptr;
};

inline void LoadWatchFonts() {}
inline const WatchFonts& GetWatchFonts() {
    static WatchFonts fonts;
    return fonts;
}
