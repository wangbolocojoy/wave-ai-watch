#pragma once

#include <cstdint>

class Display;

class AudioCodec {
public:
    int output_volume() const { return output_volume_; }
    void SetOutputVolume(int volume) { output_volume_ = volume; }
    int output_sample_rate() const { return 24000; }

private:
    int output_volume_ = 55;
};

class Backlight {
public:
    void SetBrightness(uint8_t brightness, bool permanent = false) {
        (void)permanent;
        brightness_ = brightness;
    }
    uint8_t brightness() const { return brightness_; }
    void RestoreBrightness() { brightness_ = 72; }

private:
    uint8_t brightness_ = 72;
};

class Board {
public:
    static Board& GetInstance();

    void SetDisplay(Display* display) { display_ = display; }
    void SetAudioCodec(AudioCodec* codec) { codec_ = codec; }
    void SetBacklight(Backlight* backlight) { backlight_ = backlight; }
    void SetBattery(int level, bool charging) {
        battery_level_ = level;
        charging_ = charging;
    }

    Display* GetDisplay() { return display_; }
    AudioCodec* GetAudioCodec() { return codec_; }
    Backlight* GetBacklight() { return backlight_; }
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
        level = battery_level_;
        charging = charging_;
        discharging = !charging_;
        return true;
    }

private:
    Display* display_ = nullptr;
    AudioCodec* codec_ = nullptr;
    Backlight* backlight_ = nullptr;
    int battery_level_ = 87;
    bool charging_ = false;
};
