#include "display/lcd_display.h"

#include <cstdio>
#include <cstdlib>

namespace {
#ifndef HOST_TEXT_FONT_PATH
#define HOST_TEXT_FONT_PATH "A:/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
#endif
constexpr int kHostTextFontSize = 30;
}

Display::Display() = default;
Display::~Display() = default;

bool Display::Lock(int timeout_ms) {
    (void)timeout_ms;
    lock_.lock();
    return true;
}

void Display::Unlock() {
    lock_.unlock();
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                             esp_lcd_panel_handle_t panel_handle, int width, int height,
                             int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                             bool swap_xy)
    : width_(width), height_(height) {
    (void)io_handle;
    (void)panel_handle;
    (void)offset_x;
    (void)offset_y;
    (void)mirror_x;
    (void)mirror_y;
    (void)swap_xy;
}

void SpiLcdDisplay::SetupUI() {
    Display::SetupUI();
    display_ = lv_display_get_default();
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    int font_size = kHostTextFontSize;
    if (const char* override_size = std::getenv("WAVE_HOST_FONT_SIZE")) {
        const int parsed = std::atoi(override_size);
        if (parsed >= 10 && parsed <= 48) font_size = parsed;
    }
    const lv_font_t* font = lv_tiny_ttf_create_file(HOST_TEXT_FONT_PATH, font_size);
    if (font == nullptr) {
        font = &lv_font_source_han_sans_sc_16_cjk;
        std::fprintf(stderr, "[host] TinyTTF font unavailable, using built-in 16px CJK font\n");
    } else {
        std::fprintf(stderr, "[host] Loaded %dpx host font: %s\n", font_size, HOST_TEXT_FONT_PATH);
    }
    theme_ = std::make_shared<HostTheme>(std::make_shared<HostLvglFont>(font));

    status_bar_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_bar_);
    lv_obj_set_size(status_bar_, width_, 44);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_hex(0x071019), 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_COVER, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_label_set_text(status_label_, "待机");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xb8c8d1), 0);
    if (font != nullptr) lv_obj_set_style_text_font(status_label_, font, 0);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
}

void SpiLcdDisplay::SetStatus(const char* status) {
    if (status_label_ != nullptr && status != nullptr) {
        lv_label_set_text(status_label_, status);
    }
}

void SpiLcdDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;
    (void)content;
}
