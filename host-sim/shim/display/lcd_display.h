#pragma once

#include <lvgl.h>

#include <memory>
#include <mutex>
#include <string>

// The real firmware gets these opaque handles from ESP-IDF. They are only passed
// through on the host; no panel IO is performed here.
typedef void* esp_lcd_panel_io_handle_t;
typedef void* esp_lcd_panel_handle_t;

class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual ~LvglFont() = default;
};

class HostLvglFont : public LvglFont {
public:
    explicit HostLvglFont(const lv_font_t* font) : font_(font) {}
    const lv_font_t* font() const override { return font_; }

private:
    const lv_font_t* font_;
};

class Theme {
public:
    virtual std::shared_ptr<LvglFont> GetTextFont() const = 0;
    virtual ~Theme() = default;
};

class HostTheme : public Theme {
public:
    explicit HostTheme(std::shared_ptr<LvglFont> font) : font_(std::move(font)) {}
    std::shared_ptr<LvglFont> GetTextFont() const override { return font_; }

private:
    std::shared_ptr<LvglFont> font_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual bool Lock(int timeout_ms = 0);
    virtual void Unlock();
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual void SetStatus(const char* status) { (void)status; }
    virtual void SetChatMessage(const char* role, const char* content) {
        (void)role;
        (void)content;
    }
    virtual void OnTextFontChanged() {}
    virtual void SetTheme(Theme* theme) { (void)theme; }
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) {
        (void)notification;
        (void)duration_ms;
    }
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) {
        (void)notification;
        (void)duration_ms;
    }
    virtual void SetPowerSaveMode(bool on) { (void)on; }
    virtual bool SnapshotObjectToJpeg(lv_obj_t* object, std::string& jpeg_data,
                                      int quality = 80) {
        (void)object;
        (void)jpeg_data;
        (void)quality;
        return false;
    }
    virtual Theme* GetTheme() { return theme_.get(); }
    bool setup_ui_called() const { return setup_ui_called_; }

protected:
    bool setup_ui_called_ = false;
    std::shared_ptr<HostTheme> theme_;
    std::recursive_mutex lock_;
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_ != nullptr && display_->Lock(30000)) {}
    ~DisplayLockGuard() {
        if (locked_ && display_ != nullptr) display_->Unlock();
    }
    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;
    explicit operator bool() const { return locked_; }

private:
    Display* display_ = nullptr;
    bool locked_ = false;
};

class LcdDisplay : public Display {
public:
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) {
        (void)text_font;
        return true;
    }
    virtual void SetTheme(Theme* theme) { Display::SetTheme(theme); }
};

// Host stand-in for the real SpiLcdDisplay. It creates the same minimal base
// surface the extracted watch UI expects (status bar + display pointer).
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                  int width, int height, int offset_x, int offset_y, bool mirror_x,
                  bool mirror_y, bool swap_xy);
    virtual ~SpiLcdDisplay() = default;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;

protected:
    lv_display_t* display_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    int width_ = 410;
    int height_ = 502;
};
