#include "watch_fonts.h"

#include <esp_log.h>

#include <memory>

#include "assets.h"
#include "lvgl_font.h"

namespace {
constexpr char kTag[] = "WatchFonts";
WatchFonts fonts;
std::shared_ptr<LvglFont> small_font;
std::shared_ptr<LvglFont> medium_font;
std::shared_ptr<LvglFont> large_font;
bool loaded = false;

std::shared_ptr<LvglFont> LoadFont(const char* name) {
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(name, data, size) || data == nullptr || size == 0) {
        ESP_LOGW(kTag, "Missing font asset %s", name);
        return nullptr;
    }
    auto font = std::make_shared<LvglCBinFont>(data);
    if (font->font() == nullptr) {
        ESP_LOGW(kTag, "Invalid font asset %s", name);
        return nullptr;
    }
    ESP_LOGI(kTag, "Loaded %s", name);
    return font;
}
}  // namespace

void LoadWatchFonts() {
    if (loaded)
        return;
    loaded = true;
    // The old 14px/1bpp bitmap had visibly jagged edges. The 16px/4bpp font
    // uses grayscale coverage and stays sharp at the watch's native pixel grid.
    small_font = LoadFont("font_noto_sans_common_16_4.bin");
    medium_font = LoadFont("font_noto_sans_common_20_4.bin");
    large_font = LoadFont("font_noto_sans_common_30_4.bin");
    fonts.small = small_font ? small_font->font() : nullptr;
    fonts.medium = medium_font ? medium_font->font() : nullptr;
    fonts.large = large_font ? large_font->font() : nullptr;
}

const WatchFonts& GetWatchFonts() {
    return fonts;
}
