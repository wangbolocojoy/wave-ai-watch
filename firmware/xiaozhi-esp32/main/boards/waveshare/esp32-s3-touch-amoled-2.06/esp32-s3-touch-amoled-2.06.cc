#include "wifi_board.h"
#include <ssid_manager.h>
#include <wifi_manager.h>
#include "display/lcd_display.h"
#include "esp_lcd_sh8601.h"

#include "application.h"
#include "assets/lang_config.h"
#include "axp2101.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/lvgl_display/lvgl_font.h"
#include "i2c_device.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "material_symbols.h"
#include "power_save_timer.h"
#include "watch_activity_service.h"
#include "watch_alarm_service.h"
#include "watch_rtc.h"
#include "watch_fonts.h"
#include "whale_girl_jpeg.h"
#include "music_wave_jpeg.h"
#include "watch_config_service.h"
#include "watch_debug_server.h"
#include "watch_media.h"
#include "app_icons.h"
#include "watch_music_player.h"
#include "watch_system_log.h"
#include "watch_voice_memo.h"
#include "watch_timer_service.h"

extern "C" {
// music wave artwork is embedded as JPEG and decoded into PSRAM at boot.
}

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <utility>
#include <stdlib.h>
#include <string>
#include <vector>
#include <new>
#include "esp_private/esp_gpio_reserve.h"

#define TAG "WaveshareEsp32s3TouchAMOLED2inch06"

LV_FONT_DECLARE(font_material_symbols_30_4);

static void AddLoggedWatchTool(McpServer& server, const std::string& name,
                               const std::string& description, const PropertyList& properties,
                               ToolCallback callback) {
    server.AddTool(name, description, properties,
                   [name, callback = std::move(callback)](const PropertyList& arguments) {
                       WatchSystemLog::Event("mcp", "tool_called", name.c_str());
                       auto result = callback(arguments);
                       if (!result)
                           WatchSystemLog::Event("mcp", "tool_failed", name.c_str());
                       return result;
                   });
}

class Pmic : public Axp2101 {
public:
    void EnablePowerKeyShortPressIrq() {
        WriteReg(0x41, static_cast<uint8_t>(ReadReg(0x41) | 0x08));
    }

    bool ConsumePowerKeyShortPress() {
        // Polled every 200 ms from the esp_timer task: tolerate a transient I2C
        // failure instead of aborting the firmware, and retry on the next tick.
        uint8_t status = 0;
        if (TryReadReg(0x49, &status) != ESP_OK)
            return false;
        if ((status & 0x08) == 0)
            return false;
        TryWriteReg(0x49, 0x08);  // W1C
        return true;
    }

    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);   // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x03);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

// 在waveshare_amoled_2_06类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
private:
    // The 410x502 glass rounds all four corners; keep tappable content inside this inset.
    // Physical 2.06" glass has larger corners than the old 44px page mask.
    // Keep all chrome inside a conservative safe area so the case never cuts it.
    // The physical glass rounds more aggressively than the bitmap corners.
    // 56px side inset keeps labels and dock cards out of the four glass corners.
    static constexpr int kScreenCornerRadius = 84;
    static constexpr int kScreenSafeInsetX = 64;
    static constexpr int kScreenSafeInsetTop = 40;
    static constexpr int kScreenSafeInsetBottom = 40;
    enum WatchAction : std::intptr_t {
        kNextFace = 1,
        kStartAssistant,
        kOpenMusic,
        kOpenApps,
        kOpenSettings,
        kOpenActivity,
        kBackHome,
        kMusicPlay,
        kMusicPause,
        kMusicPlayPause,
        kMusicNext,
        kMusicPrevious,
        kMusicStop,
        kBrightnessChanged,
        kVolumeChanged,
        kDndChanged,
        kOpenTimer,
        kOpenAlarm,
        kTimerStartMinute,
        kTimerStartFiveMinutes,
        kTimerStartTenMinutes,
        kTimerCancel,
        kAlarmSetSeven,
        kAlarmSetSevenThirty,
        kAlarmSetEight,
        kAlarmToggle,
        kBeginConfigPairing,
        kFaceLuxe,
        kFaceSport,
        kFaceClassic,
        kFaceCute,
        kRescanMusic,
        kMusicShuffle,
        kOpenMusicList,
        kBackMusicPlayer,
        kOpenNetwork,
        kOpenWifiScan,
        kEnterWifiConfig,
        kOpenVoiceMemo,
        kToggleVoiceMemo,
        kOpenPhotos,
        kOpenVideos,
    };
    static constexpr int kSsidActionBase = 1000;
    static constexpr int kWifiApBase = 2000;
    enum class WatchAppIcon : uint8_t { Assistant, Music, Activity, Alarm, Timer, Settings };
    lv_obj_t* watch_layer_ = nullptr;
    lv_obj_t* watch_page_ = nullptr;
    lv_obj_t* watch_gesture_layer_ = nullptr;
    lv_obj_t* wifi_status_bar_ = nullptr;
    lv_obj_t* wifi_status_icon_ = nullptr;
    lv_obj_t* wifi_status_label_ = nullptr;
    lv_timer_t* wifi_status_timer_ = nullptr;
    lv_point_t watch_swipe_start_ = {};
    bool watch_swipe_tracking_ = false;
    WatchAction page_back_action_ = kBackHome;
    lv_point_t page_back_start_ = {};
    bool page_back_tracking_ = false;
    lv_obj_t* watch_clock_ = nullptr;
    lv_obj_t* watch_date_ = nullptr;
    lv_obj_t* watch_battery_ = nullptr;
    lv_obj_t* watch_face_name_ = nullptr;
    lv_obj_t* watch_face_card_ = nullptr;
    lv_obj_t* watch_face_hint_ = nullptr;
    lv_obj_t* classic_time_ = nullptr;
    lv_obj_t* classic_date_ = nullptr;
    lv_obj_t* classic_meta_ = nullptr;
    lv_obj_t* cute_steps_ = nullptr;
    lv_obj_t* cute_battery_ = nullptr;
    lv_obj_t* watch_face_steps_ = nullptr;
    lv_obj_t* watch_face_goal_ = nullptr;
    lv_obj_t* watch_face_progress_bar_ = nullptr;
    lv_obj_t* watch_face_progress_arc_ = nullptr;
    lv_obj_t* watch_analog_dial_ = nullptr;
    lv_obj_t* watch_analog_hour_hand_ = nullptr;
    lv_obj_t* watch_analog_minute_hand_ = nullptr;
    lv_point_precise_t watch_analog_hour_points_[2] = {};
    lv_point_precise_t watch_analog_minute_points_[2] = {};
    lv_obj_t* watch_brand_ = nullptr;
    lv_obj_t* watch_face_day_ = nullptr;
    lv_obj_t* watch_face_pivot_ = nullptr;
    lv_obj_t* watch_second_hand_ = nullptr;
    lv_obj_t* watch_minute_tip_ = nullptr;
    lv_obj_t* watch_hour_tip_ = nullptr;
    lv_obj_t* watch_flux_arc2_ = nullptr;
    lv_obj_t* watch_flux_arc3_ = nullptr;
    lv_obj_t* watch_subdial_ = nullptr;
    lv_obj_t* watch_subdial_hand_ = nullptr;
    lv_obj_t* watch_tick_lines_[60] = {};
    lv_point_precise_t watch_tick_points_[60][2] = {};
    lv_point_precise_t watch_second_points_[2] = {};
    lv_point_precise_t watch_subdial_points_[2] = {};
    lv_obj_t* watch_music_status_ = nullptr;
    lv_obj_t* watch_music_progress_ = nullptr;
    lv_obj_t* watch_music_duration_ = nullptr;
    lv_obj_t* watch_music_progress_bar_ = nullptr;
    lv_obj_t* watch_music_shuffle_button_ = nullptr;
    lv_obj_t* watch_music_play_button_ = nullptr;
    lv_obj_t* voice_memo_status_ = nullptr;
    lv_obj_t* voice_memo_elapsed_ = nullptr;
    lv_obj_t* voice_memo_file_ = nullptr;
    lv_obj_t* voice_memo_toggle_ = nullptr;
    struct WifiApInfo {
        std::string ssid;
        int rssi = 0;
        bool secured = true;
    };
    lv_obj_t* wifi_scan_list_ = nullptr;
    lv_obj_t* wifi_scan_status_ = nullptr;
    std::vector<WifiApInfo> wifi_scan_results_;
    lv_obj_t* wifi_pwd_textarea_ = nullptr;
    std::string wifi_pending_ssid_;
    lv_obj_t* wifi_connect_status_ = nullptr;
    lv_obj_t* wifi_connect_hint_ = nullptr;
    lv_timer_t* wifi_connect_timer_ = nullptr;
    int wifi_connect_ticks_ = 0;
    std::string wifi_connect_ssid_;
    std::vector<MediaFile> media_entries_;
    PsramBytes photo_bytes_;
    size_t photo_index_ = 0;
    bool media_video_mode_ = false;
    // Flash artwork is stored as JPEG (about 1/5 of raw RGB565) and decoded
    // into PSRAM once, so the app partition keeps headroom.
    lv_image_dsc_t whale_girl_dsc_ = {};
    lv_image_dsc_t music_wave_dsc_ = {};
    bool embedded_images_decoded_ = false;
    lv_image_dsc_t photo_dsc_ = {};
    lv_obj_t* photo_image_ = nullptr;
    lv_obj_t* video_image_ = nullptr;
    lv_image_dsc_t video_dsc_ = {};
    WatchMedia::VideoPlayer video_player_;
    lv_obj_t* voice_memo_list_ = nullptr;
    lv_timer_t* voice_memo_timer_ = nullptr;
    static constexpr int kVoiceMemoRowHeight = 46;
    std::vector<lv_obj_t*> voice_memo_rows_;
    std::vector<std::string> voice_memo_paths_;
    bool voice_memo_was_recording_ = false;
    bool music_list_open_ = false;
    bool music_page_scanning_ = false;
    // The music list is one long scroll area with a sliding window of row
    // widgets: only ~72 rows exist in LVGL at any time, so an 900+ track
    // library never allocates 900+ objects.
    static constexpr int kMusicRowHeight = 42;
    static constexpr int kMusicRowGap = 4;
    static constexpr int kMusicRowStride = kMusicRowHeight + kMusicRowGap;
    static constexpr int kMusicWindowRows = 32;
    static constexpr int kMusicPreloadRows = 8;
    lv_obj_t* watch_music_list_ = nullptr;
    lv_timer_t* music_list_timer_ = nullptr;
    int music_list_first_ = 0;
    int music_list_rows_ = 0;
    bool music_list_refilling_ = false;
    lv_obj_t* activity_steps_label_ = nullptr;
    lv_obj_t* activity_goal_label_ = nullptr;
    lv_obj_t* activity_progress_arc_ = nullptr;
    lv_obj_t* activity_status_label_ = nullptr;
    lv_obj_t* timer_countdown_label_ = nullptr;
    lv_obj_t* alarm_time_label_ = nullptr;
    lv_obj_t* alarm_status_label_ = nullptr;
    lv_obj_t* alarm_toggle_label_ = nullptr;
    lv_obj_t* config_pairing_label_ = nullptr;
    lv_obj_t* settings_face_buttons_[4] = {};
    lv_obj_t* settings_face_current_ = nullptr;
    lv_obj_t* watch_toast_ = nullptr;
    lv_obj_t* watch_toast_label_ = nullptr;
    lv_timer_t* watch_toast_timer_ = nullptr;
    lv_obj_t* aod_layer_ = nullptr;
    lv_obj_t* aod_clock_ = nullptr;
    lv_obj_t* aod_date_ = nullptr;
    lv_obj_t* aod_battery_ = nullptr;
    bool aod_mode_ = false;
    bool voice_overlay_allowed_ = false;
    bool music_wake_hold_ = false;
    std::function<void()> aod_wake_handler_;
    static constexpr int kWatchFaceCount = 4;
    struct WatchNotification {
        std::string text;
        std::time_t time;
    };
    std::vector<WatchNotification> watch_notifications_;
    bool notification_page_open_ = false;
    lv_obj_t* watch_face_dots_[kWatchFaceCount] = {};
    lv_obj_t* settings_brightness_value_ = nullptr;
    lv_obj_t* settings_volume_value_ = nullptr;
    std::function<std::string()> config_pairing_handler_;
    WatchActivityService* activity_service_ = nullptr;
    WatchTimerService* timer_service_ = nullptr;
    WatchAlarmService* alarm_service_ = nullptr;
    lv_timer_t* watch_clock_timer_ = nullptr;
    uint8_t watch_face_ = 0;
    uint8_t watch_clock_ticks_ = 0;
    bool rebinding_text_font_ = false;

    lv_obj_t* assistant_layer_ = nullptr;
    lv_obj_t* assistant_pupils_[2] = {};
    lv_obj_t* assistant_status_ = nullptr;
    lv_obj_t* assistant_caption_ = nullptr;
    lv_obj_t* assistant_caption_viewport_ = nullptr;
    lv_point_t assistant_swipe_start_ = {};
    bool assistant_swipe_tracking_ = false;
    lv_timer_t* assistant_idle_timer_ = nullptr;
    lv_timer_t* assistant_eye_timer_ = nullptr;
    uint8_t assistant_gaze_frame_ = 0;
    bool assistant_session_active_ = false;

    static void HandleWatchEvent(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (self == nullptr)
            return;
        const auto action = static_cast<WatchAction>(
            reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
        ESP_LOGI(TAG, "watch_action_clicked=%s", WatchActionName(action));
        self->HandleWatchAction(action, event);
    }

    static void HandleAppOrbTouch(lv_event_t* event) {
        const auto code = lv_event_get_code(event);
        const auto* title = static_cast<const char*>(lv_event_get_user_data(event));
        lv_indev_t* indev = lv_indev_active();
        lv_point_t point = {};
        if (indev != nullptr)
            lv_indev_get_point(indev, &point);
        ESP_LOGI(TAG, "app_icon_touch=%s code=%d xy=%d,%d",
                 title == nullptr ? "?" : title, static_cast<int>(code),
                 static_cast<int>(point.x), static_cast<int>(point.y));
    }

    static const char* WatchActionName(WatchAction action) {
        switch (action) {
            case kNextFace: return "next_face";
            case kStartAssistant: return "open_assistant";
            case kOpenMusic: return "open_music";
            case kOpenApps: return "open_apps";
            case kOpenSettings: return "open_settings";
            case kOpenActivity: return "open_activity";
            case kBackHome: return "back_home";
            case kMusicPlay: return "music_play";
            case kMusicPause: return "music_pause";
            case kMusicPlayPause: return "music_toggle";
            case kMusicNext: return "music_next";
            case kMusicPrevious: return "music_previous";
            case kMusicStop: return "music_stop";
            case kBrightnessChanged: return "brightness_changed";
            case kVolumeChanged: return "volume_changed";
            case kDndChanged: return "dnd_changed";
            case kOpenTimer: return "open_timer";
            case kOpenAlarm: return "open_alarm";
            case kTimerStartMinute: return "timer_1m";
            case kTimerStartFiveMinutes: return "timer_5m";
            case kTimerStartTenMinutes: return "timer_10m";
            case kTimerCancel: return "timer_cancel";
            case kAlarmSetSeven: return "alarm_0700";
            case kAlarmSetSevenThirty: return "alarm_0730";
            case kAlarmSetEight: return "alarm_0800";
            case kAlarmToggle: return "alarm_toggle";
            case kBeginConfigPairing: return "config_pairing";
            case kFaceLuxe: return "face_luxe";
            case kFaceSport: return "face_sport";
            case kFaceClassic: return "face_classic";
            case kFaceCute: return "face_cute";
            case kRescanMusic: return "music_rescan";
            case kMusicShuffle: return "music_shuffle";
            case kOpenMusicList: return "music_list";
            case kBackMusicPlayer: return "music_player";
            case kOpenNetwork: return "open_network";
            case kOpenWifiScan: return "wifi_scan";
            case kEnterWifiConfig: return "wifi_config";
            case kOpenVoiceMemo: return "open_voice_memo";
            case kToggleVoiceMemo: return "voice_memo_toggle";
            case kOpenPhotos: return "open_photos";
            case kOpenVideos: return "open_videos";
        }
        return "unknown_action";
    }

    static void HandleMusicTrackEvent(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (self == nullptr)
            return;
        const auto encoded_index = reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event));
        if (encoded_index <= 0)
            return;
        // Rows carry their absolute catalog index; the track id is resolved on
        // demand so only the visible window of rows needs to exist.
        const auto track_id =
            WatchMusicPlayer::GetInstance().TrackIdAt(static_cast<size_t>(encoded_index - 1));
        if (track_id.empty())
            return;
        Application::GetInstance().Schedule([self, track_id] {
            WatchMusicPlayer::GetInstance().PlayTrack(track_id);
            DisplayLockGuard lock(self);
            if (lock)
                self->OpenMusicPage();
        });
    }

    void UpdateWifiStatusBar() {
        if (wifi_status_icon_ == nullptr || wifi_status_label_ == nullptr)
            return;
        auto& wifi = WifiManager::GetInstance();
        const bool connected = wifi.IsConnected();
        const int rssi = connected ? wifi.GetRssi() : 0;
        const char* icon = MATERIAL_SYMBOLS_WIFI_OFF;
        const char* text = "离线";
        uint32_t color = 0x6f8794;
        if (connected) {
            text = "在线";
            color = 0x65e6d6;
            if (rssi > -67)
                icon = MATERIAL_SYMBOLS_WIFI;
            else if (rssi > -80)
                icon = MATERIAL_SYMBOLS_WIFI_2_BAR;
            else
                icon = MATERIAL_SYMBOLS_WIFI_1_BAR;
        }
        lv_label_set_text(wifi_status_icon_, icon);
        lv_label_set_text(wifi_status_label_, text);
        lv_obj_set_style_text_color(wifi_status_icon_, lv_color_hex(color), 0);
        lv_obj_set_style_text_color(wifi_status_label_, lv_color_hex(color), 0);
        if (wifi_status_bar_ != nullptr) {
            lv_obj_set_style_border_color(wifi_status_bar_,
                                          lv_color_hex(connected ? 0x2c5d5d : 0x263842), 0);
        }
    }

    static void UpdateWifiStatusTimer(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self != nullptr)
            self->UpdateWifiStatusBar();
    }

    void CreateWifiStatusBar() {
        if (wifi_status_bar_ != nullptr)
            lv_obj_delete(wifi_status_bar_);
        auto* top = lv_layer_top();
        wifi_status_bar_ = lv_obj_create(top);
        lv_obj_remove_style_all(wifi_status_bar_);
        lv_obj_set_size(wifi_status_bar_, 96, 22);
        lv_obj_align(wifi_status_bar_, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(wifi_status_bar_, 11, 0);
        lv_obj_set_style_bg_color(wifi_status_bar_, lv_color_hex(0x08161e), 0);
        lv_obj_set_style_bg_opa(wifi_status_bar_, LV_OPA_80, 0);
        lv_obj_set_style_border_width(wifi_status_bar_, 1, 0);
        lv_obj_set_style_border_color(wifi_status_bar_, lv_color_hex(0x263842), 0);
        lv_obj_clear_flag(wifi_status_bar_, LV_OBJ_FLAG_CLICKABLE);
        wifi_status_icon_ = WatchIcon(wifi_status_bar_, MATERIAL_SYMBOLS_WIFI, 13,
                                      lv_color_hex(0x65e6d6));
        lv_obj_align(wifi_status_icon_, LV_ALIGN_LEFT_MID, 11, 0);
        wifi_status_label_ = WatchLabel(wifi_status_bar_, "在线", 9, lv_color_hex(0x65e6d6));
        lv_obj_set_width(wifi_status_label_, 40);
        lv_obj_set_style_text_align(wifi_status_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(wifi_status_label_, LV_ALIGN_LEFT_MID, 32, 0);
        if (wifi_status_timer_ == nullptr) {
            wifi_status_timer_ = lv_timer_create(UpdateWifiStatusTimer, 2000, this);
        }
        UpdateWifiStatusBar();
    }

    static void UpdateWatchClock(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr)
            return;
        if (++self->watch_clock_ticks_ >= 60) {
            self->watch_clock_ticks_ = 0;
            self->UpdateWatchBattery();
        }
        if (self->watch_clock_ticks_ % 10 == 0)
            self->UpdateActivityLabels();
        if (self->timer_countdown_label_ != nullptr)
            self->UpdateTimerLabel();
        if (self->watch_music_progress_ != nullptr || self->music_page_scanning_)
            self->UpdateMusicProgress();
        if (self->watch_clock_ == nullptr || self->watch_date_ == nullptr)
            return;
        const auto now = std::time(nullptr);
        if (now < 1704067200) {
            lv_label_set_text(self->watch_clock_, "--:--");
            lv_label_set_text(self->watch_date_, "等待网络时间同步");
            if (self->watch_music_status_ != nullptr) {
                std::string status = WatchMusicPlayer::GetInstance().Status();
                if (status.rfind("当前没有", 0) == 0)
                    status = "未播放";
                lv_label_set_text(self->watch_music_status_, status.c_str());
            }
            std::tm preview = {};
            preview.tm_hour = 9;
            preview.tm_min = 41;
            if (self->watch_clock_ticks_ == 1)
                self->UpdateAnalogHands(preview);
            return;
        }
        std::tm local = {};
        if (localtime_r(&now, &local) == nullptr)
            return;
        char time_text[8];
        char date_text[32];
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
        static constexpr const char* weekdays[] = {"周日", "周一", "周二", "周三",
                                                   "周四", "周五", "周六"};
        std::snprintf(date_text, sizeof(date_text), "%d月%d日  %s", local.tm_mon + 1, local.tm_mday,
                      weekdays[local.tm_wday]);
        if (std::strcmp(lv_label_get_text(self->watch_clock_), time_text) != 0)
            lv_label_set_text(self->watch_clock_, time_text);
        if (std::strcmp(lv_label_get_text(self->watch_date_), date_text) != 0)
            lv_label_set_text(self->watch_date_, date_text);
        if (self->watch_face_day_ != nullptr) {
            char day_text[4];
            std::snprintf(day_text, sizeof(day_text), "%02d", local.tm_mday);
            if (std::strcmp(lv_label_get_text(self->watch_face_day_), day_text) != 0)
                lv_label_set_text(self->watch_face_day_, day_text);
        }
        if (self->classic_time_ != nullptr) {
            char classic_time[8];
            std::strftime(classic_time, sizeof(classic_time), "%H:%M", &local);
            lv_label_set_text(self->classic_time_, classic_time);
        }
        if (self->classic_date_ != nullptr) {
            char classic_date[24];
            std::snprintf(classic_date, sizeof(classic_date), "%d月%d日  %s", local.tm_mon + 1,
                          local.tm_mday, weekdays[local.tm_wday]);
            lv_label_set_text(self->classic_date_, classic_date);
        }
        if (self->classic_meta_ != nullptr && self->activity_service_ != nullptr) {
            int level = 0;
            bool charging = false;
            bool discharging = false;
            char meta[32];
            if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                std::snprintf(meta, sizeof(meta), "%d%%  ·  %lu 步", level,
                              static_cast<unsigned long>(self->activity_service_->TodaySteps()));
            } else {
                std::snprintf(meta, sizeof(meta), "%lu 步",
                              static_cast<unsigned long>(self->activity_service_->TodaySteps()));
            }
            lv_label_set_text(self->classic_meta_, meta);
        }
        if (self->cute_steps_ != nullptr && self->activity_service_ != nullptr) {
            char steps[20];
            std::snprintf(steps, sizeof(steps), "%lu 步",
                          static_cast<unsigned long>(self->activity_service_->TodaySteps()));
            lv_label_set_text(self->cute_steps_, steps);
        }
        if (self->cute_battery_ != nullptr) {
            int level = 0;
            bool charging = false;
            bool discharging = false;
            if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                char battery[8];
                std::snprintf(battery, sizeof(battery), "%d%%", level);
                lv_label_set_text(self->cute_battery_, battery);
            }
        }
        if (self->aod_mode_)
            self->UpdateAodLabels(local);
        // Always update the analog hands from the current local time. Using the
        // 9:41 preview as a placeholder is fine, but it must disappear on the
        // first tick; waiting for tm_sec == 0 left the watch showing 9:41.
        self->UpdateAnalogHands(local);
        if (self->watch_music_status_ != nullptr) {
            std::string status = WatchMusicPlayer::GetInstance().Status();
            if (status.rfind("当前没有", 0) == 0)
                status = "未播放";
            if (std::strcmp(lv_label_get_text(self->watch_music_status_), status.c_str()) != 0)
                lv_label_set_text(self->watch_music_status_, status.c_str());
        }
    }

    void UpdateActivityLabels() {
        if (activity_service_ != nullptr && activity_service_->Available()) {
            const auto steps = activity_service_->TodaySteps();
            const auto goal = activity_service_->Goal();
            const auto progress = goal == 0 ? 0 : steps * 100 / goal;
            const auto percentage = static_cast<int>(progress > 100 ? 100 : progress);
            if (watch_face_steps_ != nullptr) {
                char face_steps[20];
                if (watch_face_ == 1) {
                    std::snprintf(face_steps, sizeof(face_steps), "%lu",
                                  static_cast<unsigned long>(steps));
                } else {
                    std::snprintf(face_steps, sizeof(face_steps), "%lu 步",
                                  static_cast<unsigned long>(steps));
                }
                lv_label_set_text(watch_face_steps_, face_steps);
            }
            if (watch_face_goal_ != nullptr) {
                char face_goal[32];
                std::snprintf(face_goal, sizeof(face_goal), "目标 %lu 步",
                              static_cast<unsigned long>(goal));
                lv_label_set_text(watch_face_goal_, face_goal);
            }
            if (watch_face_progress_arc_ != nullptr)
                lv_arc_set_value(watch_face_progress_arc_, percentage);
            if (activity_progress_arc_ != nullptr)
                lv_arc_set_value(activity_progress_arc_, percentage);
            if (watch_flux_arc2_ != nullptr) {
                int level = 0;
                bool charging = false;
                bool discharging = false;
                if (Board::GetInstance().GetBatteryLevel(level, charging, discharging))
                    lv_arc_set_value(watch_flux_arc2_, level);
            }
            if (watch_flux_arc3_ != nullptr)
                lv_arc_set_value(watch_flux_arc3_, percentage);
        }
        if (activity_steps_label_ == nullptr || activity_service_ == nullptr)
            return;
        if (!activity_service_->Available()) {
            lv_label_set_text(activity_steps_label_, "--");
            if (activity_status_label_ != nullptr)
                lv_label_set_text(activity_status_label_, "计步传感器暂不可用");
            return;
        }
        char steps[20];
        char goal[32];
        std::snprintf(steps, sizeof(steps), "%lu",
                      static_cast<unsigned long>(activity_service_->TodaySteps()));
        std::snprintf(goal, sizeof(goal), "每日目标  %lu 步",
                      static_cast<unsigned long>(activity_service_->Goal()));
        lv_label_set_text(activity_steps_label_, steps);
        if (activity_goal_label_ != nullptr)
            lv_label_set_text(activity_goal_label_, goal);
        if (activity_status_label_ != nullptr) {
            const auto progress =
                activity_service_->Goal() == 0
                    ? 0
                    : activity_service_->TodaySteps() * 100 / activity_service_->Goal();
            char status[32];
            std::snprintf(status, sizeof(status), "今日目标完成 %lu%%",
                          static_cast<unsigned long>(progress > 100 ? 100 : progress));
            lv_label_set_text(activity_status_label_, status);
        }
    }

    void UpdateAnalogHands(const std::tm& local) {
        if (watch_analog_hour_hand_ == nullptr || watch_analog_minute_hand_ == nullptr)
            return;
        constexpr double pi = 3.14159265358979323846;
        const auto minute_angle =
            ((static_cast<double>(local.tm_min) + local.tm_sec / 60.0) * (pi / 30.0)) - pi / 2.0;
        const auto hour_angle =
            ((static_cast<double>(local.tm_hour % 12) + local.tm_min / 60.0) * (pi / 6.0)) - pi / 2.0;
        const auto second_angle = (static_cast<double>(local.tm_sec) * (pi / 30.0)) - pi / 2.0;
        constexpr int center = 153;
        watch_analog_hour_points_[0] = {center, center};
        watch_analog_hour_points_[1] = {
            center + static_cast<lv_value_precise_t>(std::cos(hour_angle) * 82),
            center + static_cast<lv_value_precise_t>(std::sin(hour_angle) * 82)};
        watch_analog_minute_points_[0] = {center, center};
        watch_analog_minute_points_[1] = {
            center + static_cast<lv_value_precise_t>(std::cos(minute_angle) * 128),
            center + static_cast<lv_value_precise_t>(std::sin(minute_angle) * 128)};
        lv_line_set_points(watch_analog_hour_hand_, watch_analog_hour_points_, 2);
        lv_line_set_points(watch_analog_minute_hand_, watch_analog_minute_points_, 2);
        if (watch_minute_tip_ != nullptr) {
            lv_obj_set_pos(watch_minute_tip_,
                           static_cast<int32_t>(watch_analog_minute_points_[1].x) - 3,
                           static_cast<int32_t>(watch_analog_minute_points_[1].y) - 3);
        }
        if (watch_hour_tip_ != nullptr) {
            lv_obj_set_pos(watch_hour_tip_,
                           static_cast<int32_t>(watch_analog_hour_points_[1].x) - 4,
                           static_cast<int32_t>(watch_analog_hour_points_[1].y) - 4);
        }
        if (watch_second_hand_ != nullptr) {
            watch_second_points_[0] = {center, center};
            watch_second_points_[1] = {
                center + static_cast<lv_value_precise_t>(std::cos(second_angle) * 136),
                center + static_cast<lv_value_precise_t>(std::sin(second_angle) * 136)};
            lv_line_set_points(watch_second_hand_, watch_second_points_, 2);
        }
        if (watch_subdial_hand_ != nullptr) {
            watch_subdial_points_[0] = {21, 21};
            watch_subdial_points_[1] = {
                21 + static_cast<lv_value_precise_t>(std::sin(second_angle) * 15),
                21 - static_cast<lv_value_precise_t>(std::cos(second_angle) * 15)};
            lv_line_set_points(watch_subdial_hand_, watch_subdial_points_, 2);
        }
    }

    void UpdateTimerLabel() {
        if (timer_countdown_label_ == nullptr || timer_service_ == nullptr)
            return;
        if (timer_service_->Active()) {
            const uint32_t seconds = timer_service_->RemainingSeconds();
            char text[16];
            std::snprintf(text, sizeof(text), "%02lu:%02lu:%02lu",
                          static_cast<unsigned long>(seconds / 3600),
                          static_cast<unsigned long>((seconds / 60) % 60),
                          static_cast<unsigned long>(seconds % 60));
            lv_label_set_text(timer_countdown_label_, text);
        } else {
            lv_label_set_text(timer_countdown_label_,
                              timer_service_->Expired() ? "计时结束" : "未启动");
        }
    }

    void UpdateMusicProgress() {
        auto& player = WatchMusicPlayer::GetInstance();
        if (watch_music_play_button_ != nullptr) {
            auto* icon = lv_obj_get_child(watch_music_play_button_, 0);
            if (icon != nullptr) {
                lv_label_set_text(icon, player.IsPlaying() ? MATERIAL_SYMBOLS_PAUSE
                                                           : MATERIAL_SYMBOLS_PLAY_ARROW);
            }
        }
        if (watch_music_status_ != nullptr) {
            std::string status = player.Status();
            if (status.rfind("正在播放：", 0) == 0)
                status = status.substr(15);
            else if (status.rfind("已暂停：", 0) == 0)
                status = status.substr(12);
            else if (status.rfind("当前没有", 0) == 0)
                status = "未播放";
            if (lv_label_get_text(watch_music_status_) != status)
                lv_label_set_text(watch_music_status_, status.c_str());
        }
        if (music_page_scanning_ && !player.IsScanning()) {
            music_page_scanning_ = false;
            OpenMusicPage();
            return;
        }
        if (watch_music_progress_ == nullptr || watch_music_duration_ == nullptr ||
            watch_music_progress_bar_ == nullptr)
            return;
        const auto [position_ms, duration_ms] = player.Progress();
        const auto position_seconds = position_ms / 1000;
        const auto duration_seconds = duration_ms / 1000;
        char elapsed[12];
        char duration[12];
        std::snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu",
                      static_cast<unsigned long>(position_seconds / 60),
                      static_cast<unsigned long>(position_seconds % 60));
        if (duration_seconds > 0) {
            std::snprintf(duration, sizeof(duration), "%02lu:%02lu",
                          static_cast<unsigned long>(duration_seconds / 60),
                          static_cast<unsigned long>(duration_seconds % 60));
            const auto percentage = static_cast<int>(std::min<uint64_t>(
                static_cast<uint64_t>(position_ms) * 100 / duration_ms, 100));
            lv_bar_set_value(watch_music_progress_bar_, percentage, LV_ANIM_OFF);
        } else {
            std::snprintf(duration, sizeof(duration), "--:--");
            lv_bar_set_value(watch_music_progress_bar_, 0, LV_ANIM_OFF);
        }
        lv_label_set_text(watch_music_progress_, elapsed);
        lv_label_set_text(watch_music_duration_, duration);
    }

    void UpdateWatchBattery() {
        if (watch_battery_ == nullptr)
            return;
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            lv_label_set_text(watch_battery_, "电量 --");
            return;
        }
        char text[20];
        const char* prefix = charging ? "充电" : discharging ? "使用" : "电量";
        std::snprintf(text, sizeof(text), "%s %d%%", prefix, level);
        lv_label_set_text(watch_battery_, text);
    }

    lv_obj_t* WatchLabel(lv_obj_t* parent, const char* text, int size, lv_color_t color) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, color, 0);
        const auto& native = GetWatchFonts();
        const lv_font_t* chosen = nullptr;
        int scale_percent = 100;
        if (size <= 16 && native.small != nullptr) {
            chosen = native.small;
        } else if (size <= 24 && native.medium != nullptr) {
            chosen = native.medium;
        } else if (native.large != nullptr) {
            chosen = native.large;
            if (size > 30)
                scale_percent = std::max(100, std::min(150, size * 100 / 30));
        }
        if (chosen == nullptr) {
            auto theme = GetTheme();
            auto theme_font = theme != nullptr ? theme->GetTextFont() : nullptr;
            chosen = theme_font != nullptr ? theme_font->font() : nullptr;
            if (chosen != nullptr && size > 0) {
                // Host fallback: the desktop font can be continuously scaled.
                scale_percent = std::max(40, std::min(150, size * 100 / 30));
            }
        }
        if (chosen != nullptr)
            lv_obj_set_style_text_font(label, chosen, 0);
        if (size > 0) {
            lv_obj_set_style_text_letter_space(label, size > 28 ? 1 : 0, 0);
            if (scale_percent != 100) {
                lv_obj_set_style_transform_scale(label, scale_percent * 256 / 100, 0);
                lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
                lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
            }
        }
        return label;
    }

    lv_obj_t* WatchButton(lv_obj_t* parent, const char* title, int x, int y, int width, int height,
                          WatchAction action) {
        auto* button = lv_button_create(parent);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, width, height);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x172633), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x294252), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
        lv_obj_set_style_radius(button, 18, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));
        auto* label = WatchLabel(button, title, 16, lv_color_hex(0xeaf5f7));
        lv_obj_center(label);
        return button;
    }

    lv_obj_t* WatchIcon(lv_obj_t* parent, const char* glyph, int size, lv_color_t color) {
        auto* icon = lv_label_create(parent);
        lv_label_set_text(icon, glyph);
        lv_obj_set_style_text_font(icon, &font_material_symbols_30_4, 0);
        lv_obj_set_style_text_color(icon, color, 0);
        const int scale = std::max(35, std::min(150, size * 100 / 30));
        lv_obj_set_style_transform_scale(icon, scale * 256 / 100, 0);
        lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
        return icon;
    }

    lv_obj_t* WatchDockButton(lv_obj_t* parent, const char* glyph, const char* title, int x,
                              int width, WatchAction action, lv_color_t accent) {
        auto* button = lv_button_create(parent);
        lv_obj_set_pos(button, x, LV_VER_RES - 112);
        lv_obj_set_size(button, width, 56);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x172633), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x294252), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
        lv_obj_set_style_radius(button, 18, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));
        auto* icon = WatchIcon(button, glyph, 16, accent);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);
        auto* label = WatchLabel(button, title, 11, lv_color_hex(0xeaf5f7));
        lv_obj_set_width(label, width - 6);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -3);
        return button;
    }

    lv_obj_t* WatchDockTile(lv_obj_t* parent, const char* glyph, const char* title, int x,
                              int y, int width, int height, WatchAction action,
                              lv_color_t accent) {
        auto* button = lv_button_create(parent);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, width, height);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x101f2a), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x294252), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
        lv_obj_set_style_radius(button, 18, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(button, 1, 0);
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));
        auto* icon = WatchIcon(button, glyph, 18, accent);
        lv_obj_set_size(icon, width, 24);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        auto* label = WatchLabel(button, title, 12, lv_color_hex(0xeaf5f7));
        lv_obj_set_size(label, width - 6, 20);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        return button;
    }

    lv_obj_t* WatchIconButton(lv_obj_t* parent, const char* glyph, int x, int y, int width,
                              int height, WatchAction action, lv_color_t color,
                              lv_color_t accent) {
        auto* button = lv_button_create(parent);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, width, height);
        const bool primary = std::strcmp(glyph, MATERIAL_SYMBOLS_PLAY_ARROW) == 0;
        lv_obj_set_style_bg_color(button, primary ? lv_color_hex(0x65e6d6)
                                                   : lv_color_hex(0x132633), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, primary ? 0 : 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x2a4a58), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(button, primary ? 18 : 0, 0);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0x65e6d6), 0);
        lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));
        const int icon_size = width >= 70 ? 30 : (width >= 56 ? 26 : 21);
        auto* icon = WatchIcon(button, glyph, icon_size,
                               primary ? lv_color_hex(0x071019) : lv_color_white());
        lv_obj_center(icon);
        (void)accent;
        return button;
    }

    lv_obj_t* CreateIconShape(lv_obj_t* parent, int x, int y, int width, int height,
                              lv_color_t fill, lv_color_t border, int border_width,
                              int radius, bool filled = true) {
        auto* shape = lv_obj_create(parent);
        lv_obj_remove_style_all(shape);
        lv_obj_set_pos(shape, x, y);
        lv_obj_set_size(shape, width, height);
        lv_obj_set_style_radius(shape, radius, 0);
        lv_obj_set_style_bg_color(shape, fill, 0);
        lv_obj_set_style_bg_opa(shape, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(shape, border, 0);
        lv_obj_set_style_border_width(shape, border_width, 0);
        lv_obj_set_style_border_opa(shape, LV_OPA_COVER, 0);
        return shape;
    }

    void CreateIconStroke(lv_obj_t* parent, int x1, int y1, int x2, int y2,
                          lv_color_t color, int width = 3) {
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const auto length = static_cast<int>(std::ceil(std::hypot(dx, dy)));
        auto* stroke = CreateIconShape(parent, (x1 + x2 - length) / 2,
                                       (y1 + y2 - width) / 2, length, width,
                                       color, color, 0, width / 2);
        constexpr double pi = 3.14159265358979323846;
        const int angle = static_cast<int>(std::lround(std::atan2(dy, dx) * 1800.0 / pi));
        lv_obj_set_style_transform_pivot_x(stroke, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(stroke, LV_PCT(50), 0);
        lv_obj_set_style_transform_rotation(stroke, angle, 0);
    }

    void CreateOriginalAppIcon(lv_obj_t* parent, WatchAppIcon kind, lv_color_t accent) {
        auto* badge = lv_obj_create(parent);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 52, 52);
        lv_obj_set_style_radius(badge, 17, 0);
        lv_obj_set_style_bg_color(badge, accent, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
        lv_obj_set_style_border_color(badge, accent, 0);
        lv_obj_set_style_border_opa(badge, LV_OPA_40, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 5);

        const auto clear = lv_color_black();
        const auto ivory = lv_color_hex(0xf3f5ed);
        switch (kind) {
            case WatchAppIcon::Assistant: {
                CreateIconShape(badge, 7, 10, 38, 32, clear, accent, 2, 12, false);
                CreateIconShape(badge, 14, 20, 9, 11, ivory, ivory, 0, 5);
                CreateIconShape(badge, 29, 20, 9, 11, ivory, ivory, 0, 5);
                CreateIconShape(badge, 17, 23, 4, 6, lv_color_hex(0x163240),
                                lv_color_hex(0x163240), 0, 3);
                CreateIconShape(badge, 32, 23, 4, 6, lv_color_hex(0x163240),
                                lv_color_hex(0x163240), 0, 3);
                CreateIconStroke(badge, 19, 36, 33, 36, accent, 2);
                CreateIconStroke(badge, 26, 5, 26, 10, accent, 2);
                break;
            }
            case WatchAppIcon::Music:
                CreateIconShape(badge, 7, 7, 38, 38, clear, accent, 2, 19, false);
                CreateIconShape(badge, 13, 13, 26, 26, clear, accent, 1, 13, false);
                CreateIconShape(badge, 22, 22, 8, 8, accent, accent, 0, 4);
                CreateIconShape(badge, 14, 15, 4, 4, ivory, ivory, 0, 2);
                CreateIconStroke(badge, 34, 13, 38, 9, accent, 2);
                break;
            case WatchAppIcon::Activity:
                CreateIconStroke(badge, 7, 29, 14, 29, accent, 3);
                CreateIconStroke(badge, 14, 29, 19, 18, accent, 3);
                CreateIconStroke(badge, 19, 18, 25, 35, accent, 3);
                CreateIconStroke(badge, 25, 35, 31, 23, accent, 3);
                CreateIconStroke(badge, 31, 23, 37, 23, accent, 3);
                CreateIconShape(badge, 35, 8, 7, 7, ivory, ivory, 0, 4);
                break;
            case WatchAppIcon::Alarm:
                CreateIconShape(badge, 10, 13, 32, 32, clear, accent, 2, 16, false);
                CreateIconStroke(badge, 26, 20, 26, 28, ivory, 2);
                CreateIconStroke(badge, 26, 28, 33, 31, ivory, 2);
                CreateIconShape(badge, 6, 7, 10, 7, accent, accent, 0, 5);
                CreateIconShape(badge, 36, 7, 10, 7, accent, accent, 0, 5);
                CreateIconStroke(badge, 17, 44, 14, 47, accent, 2);
                CreateIconStroke(badge, 35, 44, 38, 47, accent, 2);
                break;
            case WatchAppIcon::Timer:
                CreateIconStroke(badge, 16, 8, 36, 8, accent, 3);
                CreateIconStroke(badge, 16, 44, 36, 44, accent, 3);
                CreateIconStroke(badge, 18, 10, 34, 24, accent, 2);
                CreateIconStroke(badge, 34, 24, 18, 42, accent, 2);
                CreateIconStroke(badge, 18, 24, 34, 24, ivory, 2);
                CreateIconStroke(badge, 21, 39, 31, 39, accent, 3);
                break;
            case WatchAppIcon::Settings:
                CreateIconShape(badge, 16, 16, 20, 20, clear, accent, 2, 10, false);
                CreateIconShape(badge, 22, 22, 8, 8, accent, accent, 0, 4);
                CreateIconShape(badge, 23, 5, 6, 8, accent, accent, 0, 3);
                CreateIconShape(badge, 23, 39, 6, 8, accent, accent, 0, 3);
                CreateIconShape(badge, 5, 23, 8, 6, accent, accent, 0, 3);
                CreateIconShape(badge, 39, 23, 8, 6, accent, accent, 0, 3);
                CreateIconShape(badge, 10, 10, 6, 6, accent, accent, 0, 3);
                CreateIconShape(badge, 36, 36, 6, 6, accent, accent, 0, 3);
                CreateIconShape(badge, 36, 10, 6, 6, accent, accent, 0, 3);
                CreateIconShape(badge, 10, 36, 6, 6, accent, accent, 0, 3);
                break;
        }
    }

    lv_obj_t* WatchAppTile(lv_obj_t* parent, WatchAppIcon icon_kind, const char* title, int x,
                            int y, int width, int height, WatchAction action, lv_color_t accent) {
        auto* button = lv_button_create(parent);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, width, height);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x142534), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x24404f), 0);
        lv_obj_set_style_radius(button, 20, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));

        const char* glyph = MATERIAL_SYMBOLS_SETTINGS;
        switch (icon_kind) {
            case WatchAppIcon::Assistant: glyph = MATERIAL_SYMBOLS_ROBOT_2; break;
            case WatchAppIcon::Music: glyph = MATERIAL_SYMBOLS_MUSIC_NOTE; break;
            case WatchAppIcon::Activity: glyph = MATERIAL_SYMBOLS_PROGRESS_ACTIVITY; break;
            case WatchAppIcon::Alarm: glyph = MATERIAL_SYMBOLS_ALARM; break;
            case WatchAppIcon::Timer: glyph = MATERIAL_SYMBOLS_SCHEDULE; break;
            case WatchAppIcon::Settings: glyph = MATERIAL_SYMBOLS_SETTINGS; break;
        }

        auto* badge = lv_obj_create(button);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 42, 42);
        lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(badge, 13, 0);
        lv_obj_set_style_bg_color(badge, accent, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
        lv_obj_set_style_border_color(badge, accent, 0);
        lv_obj_set_style_border_opa(badge, LV_OPA_40, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        auto* icon = WatchIcon(badge, glyph, 20, accent);
        lv_obj_center(icon);

        auto* label = WatchLabel(button, title, 14, lv_color_hex(0xeaf5f7));
        lv_obj_set_size(label, width - 8, 22);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);
        return button;
    }

    void CreateFaceTicks(lv_obj_t* parent, int center, int outer, int inner,
                         lv_color_t color) {
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < 60; ++i) {
            const double angle = (i * 2.0 * pi) / 60.0;
            const bool major = i % 5 == 0;
            const int tick_inner = major ? inner - 10 : inner + 7;
            watch_tick_points_[i][0] = {
                center + static_cast<lv_value_precise_t>(std::sin(angle) * tick_inner),
                center - static_cast<lv_value_precise_t>(std::cos(angle) * tick_inner)};
            watch_tick_points_[i][1] = {
                center + static_cast<lv_value_precise_t>(std::sin(angle) * outer),
                center - static_cast<lv_value_precise_t>(std::cos(angle) * outer)};
            auto* tick = lv_line_create(parent);
            watch_tick_lines_[i] = tick;
            lv_obj_set_pos(tick, 0, 0);
            lv_obj_set_size(tick, center * 2, center * 2);
            lv_obj_set_style_line_width(tick, major ? 3 : 1, 0);
            lv_obj_set_style_line_color(tick, color, 0);
            lv_obj_set_style_line_rounded(tick, true, 0);
            lv_line_set_points(tick, watch_tick_points_[i], 2);
        }
    }

    void CreateAnalogHands(lv_obj_t* parent, int center, lv_color_t hour_color,
                           lv_color_t minute_color, lv_color_t second_color) {
        watch_analog_hour_hand_ = lv_line_create(parent);
        watch_analog_minute_hand_ = lv_line_create(parent);
        watch_second_hand_ = lv_line_create(parent);
        for (auto* hand : {watch_analog_hour_hand_, watch_analog_minute_hand_}) {
            lv_obj_set_pos(hand, 0, 0);
            lv_obj_set_size(hand, center * 2, center * 2);
            lv_obj_set_style_line_rounded(hand, true, 0);
        }
        lv_obj_set_pos(watch_second_hand_, 0, 0);
        lv_obj_set_size(watch_second_hand_, center * 2, center * 2);
        lv_obj_set_style_line_width(watch_analog_hour_hand_, 7, 0);
        lv_obj_set_style_line_width(watch_analog_minute_hand_, 3, 0);
        lv_obj_set_style_line_width(watch_second_hand_, 1, 0);
        lv_obj_set_style_line_color(watch_analog_hour_hand_, hour_color, 0);
        lv_obj_set_style_line_color(watch_analog_minute_hand_, minute_color, 0);
        lv_obj_set_style_line_color(watch_second_hand_, second_color, 0);

        watch_minute_tip_ = lv_obj_create(parent);
        lv_obj_remove_style_all(watch_minute_tip_);
        lv_obj_set_size(watch_minute_tip_, 7, 7);
        lv_obj_set_style_radius(watch_minute_tip_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(watch_minute_tip_, minute_color, 0);
        lv_obj_set_style_bg_opa(watch_minute_tip_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(watch_minute_tip_, LV_OBJ_FLAG_CLICKABLE);

        watch_hour_tip_ = lv_obj_create(parent);
        lv_obj_remove_style_all(watch_hour_tip_);
        lv_obj_set_size(watch_hour_tip_, 8, 8);
        lv_obj_set_style_radius(watch_hour_tip_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(watch_hour_tip_, hour_color, 0);
        lv_obj_set_style_bg_opa(watch_hour_tip_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(watch_hour_tip_, LV_OBJ_FLAG_CLICKABLE);

        watch_face_pivot_ = lv_obj_create(parent);
        lv_obj_remove_style_all(watch_face_pivot_);
        lv_obj_set_size(watch_face_pivot_, 10, 10);
        lv_obj_set_pos(watch_face_pivot_, center - 5, center - 5);
        lv_obj_set_style_radius(watch_face_pivot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(watch_face_pivot_, hour_color, 0);
        lv_obj_set_style_bg_opa(watch_face_pivot_, LV_OPA_COVER, 0);
    }

    lv_obj_t* WatchDayWindow(lv_obj_t* parent, lv_color_t border, lv_color_t text_color) {
        auto* window = lv_obj_create(parent);
        lv_obj_remove_style_all(window);
        lv_obj_set_size(window, 40, 27);
        lv_obj_align(window, LV_ALIGN_RIGHT_MID, -31, 0);
        lv_obj_set_style_radius(window, 4, 0);
        lv_obj_set_style_bg_opa(window, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(window, 1, 0);
        lv_obj_set_style_border_color(window, border, 0);
        auto* label = WatchLabel(window, "13", 11, text_color);
        lv_obj_set_width(label, 38);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        return label;
    }

    void DecodeEmbeddedImage(const uint8_t* jpeg, size_t jpeg_size, lv_image_dsc_t& descriptor) {
        uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        descriptor = {};
        if (!WatchMedia::DecodeJpegMemory(jpeg, jpeg_size, &pixels, width, height) ||
            pixels == nullptr) {
            static const uint8_t kPlaceholder[2] = {0, 0};
            descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
            descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
            descriptor.header.w = 1;
            descriptor.header.h = 1;
            descriptor.header.stride = 2;
            descriptor.data = kPlaceholder;
            descriptor.data_size = sizeof(kPlaceholder);
            return;
        }
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = static_cast<uint32_t>(width);
        descriptor.header.h = static_cast<uint32_t>(height);
        descriptor.header.stride = static_cast<uint32_t>(width) * 2;
        descriptor.data = pixels;
        descriptor.data_size = static_cast<size_t>(width) * height * 2;
    }

    void EnsureEmbeddedImagesDecoded() {
        if (embedded_images_decoded_)
            return;
        embedded_images_decoded_ = true;
        DecodeEmbeddedImage(whale_girl_jpeg, whale_girl_jpeg_len, whale_girl_dsc_);
        DecodeEmbeddedImage(music_wave_jpeg, music_wave_jpeg_len, music_wave_dsc_);
        ESP_LOGI(TAG, "Artwork decoded: whale %ux%u, music %ux%u",
                 static_cast<unsigned>(whale_girl_dsc_.header.w),
                 static_cast<unsigned>(whale_girl_dsc_.header.h),
                 static_cast<unsigned>(music_wave_dsc_.header.w),
                 static_cast<unsigned>(music_wave_dsc_.header.h));
    }

    void CreateWatchShell() {
        EnsureEmbeddedImagesDecoded();
        watch_layer_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(watch_layer_);
        lv_obj_set_size(watch_layer_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(watch_layer_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(watch_layer_, true, 0);
        lv_obj_set_style_bg_color(watch_layer_, lv_color_hex(0x071019), 0);
        lv_obj_set_style_bg_opa(watch_layer_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(watch_layer_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(watch_layer_, true, 0);

        watch_brand_ = WatchLabel(watch_layer_, "WAVE  ·  手表", 14, lv_color_hex(0x8da3af));
        lv_obj_align(watch_brand_, LV_ALIGN_TOP_LEFT, kScreenSafeInsetX, 22);
        watch_battery_ = WatchLabel(watch_layer_, "电量 --", 14, lv_color_hex(0x8da3af));
        lv_obj_set_width(watch_battery_, 140);
        lv_obj_set_style_text_align(watch_battery_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(watch_battery_, LV_ALIGN_TOP_RIGHT, -kScreenSafeInsetX, 22);
        watch_clock_ = WatchLabel(watch_layer_, "09:41", 54, lv_color_hex(0xf2f7fb));
        lv_obj_align(watch_clock_, LV_ALIGN_TOP_MID, 0, 52);
        watch_date_ = WatchLabel(watch_layer_, "11月12日  周四", 14, lv_color_hex(0xa9bac4));
        lv_obj_align(watch_date_, LV_ALIGN_TOP_MID, 0, 110);

        watch_face_card_ = lv_obj_create(watch_layer_);
        lv_obj_remove_style_all(watch_face_card_);
        lv_obj_clear_flag(watch_face_card_, LV_OBJ_FLAG_CLICKABLE);

        // A transparent full-screen touch surface above the face catches swipes
        // even when the touch lands on a label/image child of the face card.
        watch_gesture_layer_ = lv_obj_create(watch_layer_);
        lv_obj_remove_style_all(watch_gesture_layer_);
        lv_obj_set_pos(watch_gesture_layer_, 0, 0);
        lv_obj_set_size(watch_gesture_layer_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_opa(watch_gesture_layer_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(watch_gesture_layer_, 0, 0);
        lv_obj_set_style_radius(watch_gesture_layer_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(watch_gesture_layer_, true, 0);
        lv_obj_add_flag(watch_gesture_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(watch_gesture_layer_, HandleFaceTouch, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(watch_gesture_layer_, HandleFaceTouch, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(watch_gesture_layer_, HandleFaceTouch, LV_EVENT_PRESS_LOST, this);

        CreateWifiStatusBar();
        watch_clock_timer_ = lv_timer_create(UpdateWatchClock, 1000, this);
        if (watch_clock_timer_ != nullptr)
            UpdateWatchClock(watch_clock_timer_);
        UpdateWatchBattery();
        UpdateWatchFace();
    }

    void UpdateWatchFace() {
        static constexpr const char* names[] = {"金色雅韵", "活力运动", "经典指针", "软糖"};
        static constexpr uint32_t layer_colors[] = {0x0e0b06, 0x040b10, 0xf3ead6, 0xe8edff};
        static constexpr uint32_t accent_colors[] = {0xe9c987, 0x65e6d6, 0x293f35, 0xf3a6b8};
        if (watch_face_card_ == nullptr)
            return;
        const auto face = watch_face_ % kWatchFaceCount;
        const auto accent = lv_color_hex(accent_colors[face]);
        lv_obj_set_style_bg_color(watch_layer_, lv_color_hex(layer_colors[face]), 0);

        lv_obj_clean(watch_face_card_);
        watch_face_name_ = nullptr;
        watch_face_hint_ = nullptr;
        classic_time_ = nullptr;
        classic_date_ = nullptr;
        classic_meta_ = nullptr;
        cute_steps_ = nullptr;
        cute_battery_ = nullptr;
        watch_face_steps_ = nullptr;
        watch_face_goal_ = nullptr;
        watch_face_progress_bar_ = nullptr;
        watch_face_progress_arc_ = nullptr;
        watch_face_day_ = nullptr;
        watch_face_pivot_ = nullptr;
        watch_analog_dial_ = nullptr;
        watch_analog_hour_hand_ = nullptr;
        watch_analog_minute_hand_ = nullptr;
        watch_second_hand_ = nullptr;
        watch_minute_tip_ = nullptr;
        watch_hour_tip_ = nullptr;
        watch_flux_arc2_ = nullptr;
        watch_flux_arc3_ = nullptr;
        watch_subdial_ = nullptr;
        watch_subdial_hand_ = nullptr;
        for (auto& line : watch_tick_lines_) line = nullptr;

        if (watch_brand_ != nullptr) {
            char brand[48];
            std::snprintf(brand, sizeof(brand), "WAVE  ·  %s", names[face]);
            lv_label_set_text(watch_brand_, brand);
            lv_obj_set_style_text_color(watch_brand_,
                                        face == 2 ? lv_color_hex(0x8da3af) : accent, 0);
        }
        if (watch_brand_ != nullptr) {
            if (face >= 2)
                lv_obj_add_flag(watch_brand_, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(watch_brand_, LV_OBJ_FLAG_HIDDEN);
        }
        if (watch_battery_ != nullptr) {
            lv_obj_set_style_text_color(watch_battery_, lv_color_hex(0x8da3af), 0);
            if (face >= 2)
                lv_obj_add_flag(watch_battery_, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(watch_battery_, LV_OBJ_FLAG_HIDDEN);
        }
        if (watch_date_ != nullptr) {
            if (face >= 2) {
                lv_obj_add_flag(watch_date_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(watch_date_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(watch_date_, LV_ALIGN_TOP_MID, 0, face == 1 ? 110 : 44);
                lv_obj_set_style_text_color(watch_date_, accent, 0);
            }
        }
        if (watch_clock_ != nullptr) {
            if (face == 1) {
                lv_obj_clear_flag(watch_clock_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(watch_clock_, LV_ALIGN_TOP_MID, 0, 52);
                lv_obj_set_style_text_color(watch_clock_, lv_color_hex(0xf2f7fb), 0);
            } else {
                lv_obj_add_flag(watch_clock_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        lv_obj_set_style_bg_opa(watch_face_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(watch_face_card_, 0, 0);
        lv_obj_set_style_border_opa(watch_face_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(watch_face_card_, 0, 0);

        if (face == 0) {
            lv_obj_set_pos(watch_face_card_, 52, 72);
            lv_obj_set_size(watch_face_card_, 306, 306);
            lv_obj_set_style_radius(watch_face_card_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(watch_face_card_, lv_color_hex(0x1b150c), 0);
            lv_obj_set_style_border_width(watch_face_card_, 1, 0);
            lv_obj_set_style_border_color(watch_face_card_, accent, 0);
            CreateFaceTicks(watch_face_card_, 153, 145, 130, lv_color_hex(0x8f7644));
            auto* sign = WatchLabel(watch_face_card_, "WAVE", 10, accent);
            lv_obj_align(sign, LV_ALIGN_TOP_MID, 0, 34);
            watch_face_day_ = WatchDayWindow(watch_face_card_, accent, lv_color_hex(0xf0e3c9));
            watch_face_steps_ = WatchLabel(watch_face_card_, "-- 步", 9, lv_color_hex(0xb49a67));
            lv_obj_align(watch_face_steps_, LV_ALIGN_BOTTOM_MID, 0, -43);
            CreateAnalogHands(watch_face_card_, 153, lv_color_hex(0xe9c987),
                              lv_color_hex(0xd8bd87), lv_color_hex(0xb76845));
        } else if (face == 1) {
            lv_obj_set_pos(watch_face_card_, 85, 134);
            lv_obj_set_size(watch_face_card_, 240, 240);
            lv_obj_set_style_radius(watch_face_card_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(watch_face_card_, LV_OPA_TRANSP, 0);
            auto make_arc = [this](int size, lv_color_t color, lv_color_t track) {
                auto* arc = lv_arc_create(watch_face_card_);
                lv_obj_set_size(arc, size, size);
                lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
                lv_arc_set_rotation(arc, 135);
                lv_arc_set_bg_angles(arc, 0, 270);
                lv_arc_set_range(arc, 0, 100);
                lv_arc_set_value(arc, 0);
                lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
                lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
                lv_obj_set_style_arc_color(arc, track, LV_PART_MAIN);
                lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
                lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
                lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
                return arc;
            };
            watch_face_progress_arc_ = make_arc(232, lv_color_hex(0x65e6d6), lv_color_hex(0x16303a));
            watch_flux_arc2_ = make_arc(208, lv_color_hex(0xe9c987), lv_color_hex(0x2b2a20));
            watch_flux_arc3_ = make_arc(184, lv_color_hex(0xb8e994), lv_color_hex(0x1c2b22));
            watch_face_steps_ = WatchLabel(watch_face_card_, "--", 30, lv_color_hex(0xf2f7fb));
            lv_obj_align(watch_face_steps_, LV_ALIGN_CENTER, 0, -6);
            watch_face_hint_ = WatchLabel(watch_face_card_, "今日步数", 11,
                                          lv_color_hex(0xa9bac4));
            lv_obj_align(watch_face_hint_, LV_ALIGN_CENTER, 0, 22);
            watch_face_goal_ = WatchLabel(watch_face_card_, "目标 8000 步", 10,
                                          lv_color_hex(0xb8e994));
            lv_obj_align(watch_face_goal_, LV_ALIGN_CENTER, 0, 47);
        } else if (face == 2) {
            // Reference-inspired classic face: large serif-like numerals,
            // a central digital time/date, a small battery/step readout and
            // two deep-green hands with an orange center cap.
            lv_obj_set_pos(watch_face_card_, 52, 72);
            lv_obj_set_size(watch_face_card_, 306, 306);
            lv_obj_set_style_radius(watch_face_card_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(watch_face_card_, lv_color_hex(0xf3ead6), 0);
            lv_obj_set_style_border_width(watch_face_card_, 0, 0);
            CreateFaceTicks(watch_face_card_, 153, 145, 132, lv_color_hex(0x6c7b6a));

            constexpr double pi = 3.14159265358979323846;
            for (int i = 1; i <= 12; ++i) {
                const double angle = (i * pi) / 6.0;
                const int x = 153 + static_cast<int>(std::lround(std::sin(angle) * 112.0));
                const int y = 153 - static_cast<int>(std::lround(std::cos(angle) * 112.0));
                char numeral[3];
                std::snprintf(numeral, sizeof(numeral), "%d", i);
                auto* label = WatchLabel(watch_face_card_, numeral, 20,
                                         lv_color_hex(0x293f35));
                lv_obj_set_size(label, 40, 34);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_pos(label, x - 20, y - 17);
            }

            classic_time_ = WatchLabel(watch_face_card_, "09:41", 30,
                                       lv_color_hex(0x293f35));
            lv_obj_align(classic_time_, LV_ALIGN_CENTER, 0, -54);
            classic_date_ = WatchLabel(watch_face_card_, "11月14日 周六", 14,
                                       lv_color_hex(0xb76845));
            lv_obj_align(classic_date_, LV_ALIGN_CENTER, 0, -12);
            classic_meta_ = WatchLabel(watch_face_card_, "95%  ·  0 步", 11,
                                       lv_color_hex(0xb76845));
            lv_obj_align(classic_meta_, LV_ALIGN_CENTER, 0, 44);

            CreateAnalogHands(watch_face_card_, 153, lv_color_hex(0x293f35),
                              lv_color_hex(0x293f35), lv_color_hex(0xb76845));
            if (watch_second_hand_ != nullptr)
                lv_obj_add_flag(watch_second_hand_, LV_OBJ_FLAG_HIDDEN);
            if (watch_face_pivot_ != nullptr) {
                lv_obj_set_style_bg_color(watch_face_pivot_, lv_color_hex(0xe2552f), 0);
                lv_obj_set_size(watch_face_pivot_, 14, 14);
                lv_obj_set_pos(watch_face_pivot_, 146, 146);
            }
            // Hybrid layout: keep the digital time/date/meta legible when a
            // hand crosses the center, like the reference face.
            if (classic_time_ != nullptr)
                lv_obj_move_foreground(classic_time_);
            if (classic_date_ != nullptr)
                lv_obj_move_foreground(classic_date_);
            if (classic_meta_ != nullptr)
                lv_obj_move_foreground(classic_meta_);
        } else {
            // Mochi: a soft pastel Apple-Watch-like digital face with a cute
            // mascot, live time/date, and two real health/battery pills.
            lv_obj_set_pos(watch_face_card_, 0, 0);
            lv_obj_set_size(watch_face_card_, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_style_radius(watch_face_card_, 0, 0);
            lv_obj_set_style_bg_opa(watch_face_card_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(watch_face_card_, 0, 0);

            auto bubble = [&](int x, int y, int size, uint32_t color, lv_opa_t opacity) {
                auto* object = lv_obj_create(watch_face_card_);
                lv_obj_remove_style_all(object);
                lv_obj_set_pos(object, x, y);
                lv_obj_set_size(object, size, size);
                lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
                lv_obj_set_style_bg_opa(object, opacity, 0);
                lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
            };
            bubble(28, 62, 84, 0xd4deff, LV_OPA_70);
            bubble(322, 46, 54, 0xd4deff, LV_OPA_70);
            bubble(12, 360, 66, 0xc9d6ff, LV_OPA_60);
            bubble(344, 330, 70, 0xc9d6ff, LV_OPA_60);

            classic_time_ = WatchLabel(watch_face_card_, "09:41", 58, lv_color_hex(0x1b2350));
            lv_obj_align(classic_time_, LV_ALIGN_TOP_MID, 0, 56);
            classic_date_ = WatchLabel(watch_face_card_, "11月14日 周六", 14,
                                       lv_color_hex(0x4d6bfe));
            lv_obj_align(classic_date_, LV_ALIGN_TOP_MID, 0, 128);

            // The illustrated whale-girl PNG is compiled in here. The 1x1
            // placeholder falls back to the wordmark until a real asset is
            // generated with scripts/png_to_whale_image.py.
            auto* mark = WatchLabel(watch_face_card_, "DEEPSEEK", 12, lv_color_hex(0x4d6bfe));
            lv_obj_set_style_text_letter_space(mark, 4, 0);
            lv_obj_align(mark, LV_ALIGN_CENTER, 0, 64);
            auto* whale = lv_image_create(watch_face_card_);
            lv_image_set_src(whale, &whale_girl_dsc_);
            if (whale_girl_dsc_.header.w > 1 && whale_girl_dsc_.header.h > 1) {
                const int visual = 240;
                lv_obj_set_size(whale, visual, visual);
                lv_image_set_scale(whale, static_cast<uint32_t>(visual * 256 / whale_girl_dsc_.header.w));
                lv_obj_align(whale, LV_ALIGN_CENTER, 0, 20);
                lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(whale, LV_OBJ_FLAG_HIDDEN);
            }

            auto* steps_pill = lv_obj_create(watch_face_card_);
            lv_obj_remove_style_all(steps_pill);
            lv_obj_set_pos(steps_pill, 64, 400);
            lv_obj_set_size(steps_pill, 136, 48);
            lv_obj_set_style_radius(steps_pill, 18, 0);
            lv_obj_set_style_bg_color(steps_pill, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_opa(steps_pill, LV_OPA_60, 0);
            lv_obj_set_style_border_width(steps_pill, 1, 0);
            lv_obj_set_style_border_color(steps_pill, lv_color_hex(0x4d6bfe), 0);
            auto* steps_dot = lv_obj_create(steps_pill);
            lv_obj_remove_style_all(steps_dot);
            lv_obj_set_pos(steps_dot, 14, 19);
            lv_obj_set_size(steps_dot, 10, 10);
            lv_obj_set_style_radius(steps_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(steps_dot, lv_color_hex(0x7ecfa8), 0);
            lv_obj_set_style_bg_opa(steps_dot, LV_OPA_COVER, 0);
            cute_steps_ = WatchLabel(steps_pill, "0 步", 12, lv_color_hex(0x4a2b3a));
            lv_obj_align(cute_steps_, LV_ALIGN_LEFT_MID, 32, 0);

            auto* battery_pill = lv_obj_create(watch_face_card_);
            lv_obj_remove_style_all(battery_pill);
            lv_obj_set_pos(battery_pill, 208, 400);
            lv_obj_set_size(battery_pill, 138, 48);
            lv_obj_set_style_radius(battery_pill, 18, 0);
            lv_obj_set_style_bg_color(battery_pill, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_opa(battery_pill, LV_OPA_60, 0);
            lv_obj_set_style_border_width(battery_pill, 1, 0);
            lv_obj_set_style_border_color(battery_pill, lv_color_hex(0x4d6bfe), 0);
            auto* battery_dot = lv_obj_create(battery_pill);
            lv_obj_remove_style_all(battery_dot);
            lv_obj_set_pos(battery_dot, 14, 19);
            lv_obj_set_size(battery_dot, 10, 10);
            lv_obj_set_style_radius(battery_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(battery_dot, lv_color_hex(0xf3c66b), 0);
            lv_obj_set_style_bg_opa(battery_dot, LV_OPA_COVER, 0);
            cute_battery_ = WatchLabel(battery_pill, "95%", 12, lv_color_hex(0x4a2b3a));
            lv_obj_align(cute_battery_, LV_ALIGN_LEFT_MID, 32, 0);
        }
        UpdateActivityLabels();
        std::tm preview = {};
        preview.tm_hour = 9;
        preview.tm_min = 41;
        UpdateAnalogHands(preview);
    }

    static void HandleFaceTouch(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr)
            return;
        lv_point_t point = {};
        lv_indev_get_point(indev, &point);
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
            self->watch_swipe_start_ = point;
            self->watch_swipe_tracking_ = true;
        } else if (code == LV_EVENT_PRESS_LOST) {
            self->watch_swipe_tracking_ = false;
        } else if (code == LV_EVENT_RELEASED && self->watch_swipe_tracking_) {
            self->watch_swipe_tracking_ = false;
            const int dx = point.x - self->watch_swipe_start_.x;
            const int dy = point.y - self->watch_swipe_start_.y;
            if (abs(dy) >= 36 && abs(dy) > abs(dx)) {
                if (dy > 0)
                    self->OpenAppsPage();
                else
                    self->OpenNotificationsPage();
            }
        }
    }

    static void HandleWatchGesture(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        const auto dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) {
            self->OpenAppsPage();
        } else if (dir == LV_DIR_BOTTOM) {
            self->OpenNotificationsPage();
        }
    }

    void GoBackFromPage() {
        const auto action = page_back_action_;
        page_back_action_ = kBackHome;
        HandleWatchAction(action, nullptr);
    }

    static void HandlePageBackTouch(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr)
            return;
        lv_point_t point = {};
        lv_indev_get_point(indev, &point);
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
            self->page_back_start_ = point;
            self->page_back_tracking_ = true;
        } else if (code == LV_EVENT_PRESS_LOST) {
            self->page_back_tracking_ = false;
        } else if (code == LV_EVENT_RELEASED && self->page_back_tracking_) {
            self->page_back_tracking_ = false;
            const int dx = point.x - self->page_back_start_.x;
            const int dy = point.y - self->page_back_start_.y;
            if (dy <= -28 && abs(dy) > abs(dx))
                self->GoBackFromPage();
        }
    }

    static void HandlePageGesture(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
            if (self->music_list_open_)
                self->OpenMusicPage();
            else
                self->OpenWatchApp("表盘");
        }
    }

    // Every pointer/timer below belongs to the page that is about to be
    // deleted. Both OpenPage() and the back-to-watch-face path must go through
    // here, otherwise a page timer outlives its widgets and fires on freed
    // LVGL objects (that was the cause of the random panics after leaving the
    // voice memo page).
    void ReleaseWatchPageHandles() {
        if (voice_memo_timer_ != nullptr) {
            lv_timer_delete(voice_memo_timer_);
            voice_memo_timer_ = nullptr;
        }
        voice_memo_status_ = nullptr;
        voice_memo_elapsed_ = nullptr;
        voice_memo_file_ = nullptr;
        voice_memo_toggle_ = nullptr;
        voice_memo_list_ = nullptr;
        voice_memo_rows_.clear();
        voice_memo_paths_.clear();
        voice_memo_was_recording_ = false;
        video_player_.Stop();
        video_image_ = nullptr;
        photo_image_ = nullptr;
        // clear()+shrink_to_fit() is not guaranteed to hand the PSRAM block back;
        // swapping in an empty vector releases it deterministically.
        PsramBytes().swap(photo_bytes_);
        if (wifi_connect_timer_ != nullptr) {
            lv_timer_delete(wifi_connect_timer_);
            wifi_connect_timer_ = nullptr;
        }
        wifi_scan_list_ = nullptr;
        wifi_scan_status_ = nullptr;
        wifi_pwd_textarea_ = nullptr;
        wifi_connect_status_ = nullptr;
        wifi_connect_hint_ = nullptr;
        if (assistant_idle_timer_ != nullptr) {
            lv_timer_delete(assistant_idle_timer_);
            assistant_idle_timer_ = nullptr;
        }
        if (assistant_layer_ != nullptr) {
            lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(assistant_layer_, LV_OBJ_FLAG_CLICKABLE);
        }
        assistant_session_active_ = false;
        voice_overlay_allowed_ = false;
        notification_page_open_ = false;
        for (auto*& button : settings_face_buttons_)
            button = nullptr;
        settings_face_current_ = nullptr;
        if (watch_toast_ != nullptr)
            lv_obj_add_flag(watch_toast_, LV_OBJ_FLAG_HIDDEN);
        music_page_scanning_ = false;
        watch_music_status_ = nullptr;
        watch_music_progress_ = nullptr;
        watch_music_duration_ = nullptr;
        watch_music_progress_bar_ = nullptr;
        watch_music_shuffle_button_ = nullptr;
        watch_music_play_button_ = nullptr;
        ResetMusicList();
        music_list_open_ = false;
        activity_steps_label_ = nullptr;
        activity_goal_label_ = nullptr;
        activity_progress_arc_ = nullptr;
        activity_status_label_ = nullptr;
        timer_countdown_label_ = nullptr;
        alarm_time_label_ = nullptr;
        alarm_status_label_ = nullptr;
        alarm_toggle_label_ = nullptr;
        config_pairing_label_ = nullptr;
        settings_brightness_value_ = nullptr;
        settings_volume_value_ = nullptr;
    }

    void OpenPage(const char* title, WatchAction back_action = kBackHome,
                  bool show_header = true) {
        if (watch_layer_ == nullptr)
            return;
        ESP_LOGI(TAG, "watch_page_open=%s", title);
        ReleaseWatchPageHandles();
        if (watch_page_ != nullptr)
            lv_obj_delete(watch_page_);
        watch_page_ = nullptr;
        page_back_action_ = back_action;
        watch_page_ = lv_obj_create(watch_layer_);
        lv_obj_remove_style_all(watch_page_);
        lv_obj_set_size(watch_page_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(watch_page_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(watch_page_, true, 0);
        lv_obj_set_style_bg_color(watch_page_, lv_color_hex(0x091827), 0);
        lv_obj_set_style_bg_opa(watch_page_, LV_OPA_COVER, 0);
        activity_steps_label_ = nullptr;
        activity_goal_label_ = nullptr;
        activity_progress_arc_ = nullptr;
        activity_status_label_ = nullptr;
        timer_countdown_label_ = nullptr;
        alarm_time_label_ = nullptr;
        alarm_status_label_ = nullptr;
        alarm_toggle_label_ = nullptr;
        config_pairing_label_ = nullptr;
        if (show_header) {
            auto* heading = WatchLabel(watch_page_, title, 20, lv_color_hex(0xf2f7fb));
            lv_obj_set_width(heading, LV_HOR_RES - 100);
            lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 33);
        }
        auto* back_zone = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(back_zone);
        lv_obj_set_pos(back_zone, 0, LV_VER_RES - 36);
        lv_obj_set_size(back_zone, LV_HOR_RES, 36);
        lv_obj_set_style_bg_opa(back_zone, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(back_zone, 0, 0);
        lv_obj_add_flag(back_zone, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back_zone, HandlePageBackTouch, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(back_zone, HandlePageBackTouch, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(back_zone, HandlePageBackTouch, LV_EVENT_PRESS_LOST, this);
        lv_obj_move_foreground(watch_page_);
    }

    void OpenNotificationsPage() {
        OpenPage("通知");
        notification_page_open_ = true;
        if (watch_notifications_.empty()) {
            auto* icon = WatchIcon(watch_page_, MATERIAL_SYMBOLS_NOTIFICATIONS, 34,
                                   lv_color_hex(0x65e6d6));
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -42);
            auto* empty = WatchLabel(watch_page_, "暂无通知", 20, lv_color_hex(0xf2f7fb));
            lv_obj_align(empty, LV_ALIGN_CENTER, 0, 18);
            auto* hint = WatchLabel(watch_page_, "系统提醒会出现在这里", 12,
                                    lv_color_hex(0x8296a3));
            lv_obj_align(hint, LV_ALIGN_CENTER, 0, 54);
            return;
        }
        auto* list = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, 48, 88);
        lv_obj_set_size(list, LV_HOR_RES - 96, 380);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 8, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        for (auto it = watch_notifications_.rbegin(); it != watch_notifications_.rend(); ++it) {
            auto* card = lv_obj_create(list);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, LV_HOR_RES - 96, 72);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x101a20), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x263941), 0);
            lv_obj_set_style_radius(card, 18, 0);
            lv_obj_set_style_shadow_width(card, 0, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
            auto* icon = WatchIcon(card, MATERIAL_SYMBOLS_NOTIFICATIONS, 16,
                                   lv_color_hex(0x65e6d6));
            lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 14, 12);
            auto* text = WatchLabel(card, it->text.c_str(), 14, lv_color_hex(0xeaf5f7));
            lv_obj_set_pos(text, 46, 8);
            lv_obj_set_size(text, LV_HOR_RES - 152, 46);
            lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
            char when[8];
            std::tm local = {};
            localtime_r(&it->time, &local);
            std::strftime(when, sizeof(when), "%H:%M", &local);
            auto* time_label = WatchLabel(card, when, 10, lv_color_hex(0x71828c));
            lv_obj_align(time_label, LV_ALIGN_BOTTOM_RIGHT, -14, -7);
        }
    }

    void UpdateSettingsFaceSelection() {
        static const char* face_short_names[] = {"金色", "运动", "经典", "鲸鱼娘"};
        if (settings_face_current_ != nullptr) {
            char current[24];
            std::snprintf(current, sizeof(current), "当前 %s",
                          face_short_names[watch_face_ < 4 ? watch_face_ : 3]);
            lv_label_set_text(settings_face_current_, current);
        }
        for (int i = 0; i < 4; ++i) {
            auto* button = settings_face_buttons_[i];
            if (button == nullptr)
                continue;
            const bool selected = i == watch_face_;
            lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
            lv_obj_set_style_border_color(button, lv_color_hex(selected ? 0x65e6d6 : 0x294252), 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(selected ? 0x65e6d6 : 0x0c1b24), 0);
            auto* label = lv_obj_get_child(button, 0);
            if (label != nullptr)
                lv_obj_set_style_text_color(
                    label, lv_color_hex(selected ? 0x071019 : 0xeaf5f7), 0);
        }
    }

    void SelectWatchFace(int face) {
        if (face < 0 || face >= kWatchFaceCount)
            return;
        watch_face_ = face;
        UpdateWatchFace();
        {
            Settings preferences("watchui", true);
            preferences.SetInt("face", face);
        }
        UpdateSettingsFaceSelection();
    }

    // Entering config mode through WifiBoard::EnterWifiConfigMode() is refused
    // once the device already sits in the wifi_configuring state (which is the
    // normal state when the watch boots without saved credentials), so bring
    // the provisioning AP back directly.
    void EnsureWifiConfigMode() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateWifiConfiguring)
            app.SetDeviceState(kDeviceStateWifiConfiguring);
        WifiManager::GetInstance().StartConfigAp();
    }

    void OpenNetworkPage() {
        OpenPage("网络", kOpenSettings);
        auto* wifi_card = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(wifi_card);
        lv_obj_set_pos(wifi_card, 48, 88);
        lv_obj_set_size(wifi_card, LV_HOR_RES - 96, 112);
        lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x101a20), 0);
        lv_obj_set_style_bg_opa(wifi_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(wifi_card, 1, 0);
        lv_obj_set_style_border_color(wifi_card, lv_color_hex(0x263941), 0);
        lv_obj_set_style_radius(wifi_card, 22, 0);
        lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_CLICKABLE);

        auto& wifi = WifiManager::GetInstance();
        const auto current_ssid = wifi.GetSsid();
        const auto ip = wifi.GetIpAddress();
        const int rssi = wifi.GetRssi();
        auto* status = WatchLabel(wifi_card,
                                  current_ssid.empty() ? "未连接 Wi-Fi" : current_ssid.c_str(),
                                  17, lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(status, 16, 10);
        char info[64];
        if (current_ssid.empty())
            std::snprintf(info, sizeof(info), "可添加手机热点 2.4GHz");
        else
            std::snprintf(info, sizeof(info), "%s · %d dBm", ip.c_str(), rssi);
        auto* info_label = WatchLabel(wifi_card, info, 11, lv_color_hex(0x8296a3));
        lv_obj_set_pos(info_label, 16, 40);

        auto* saved_heading = WatchLabel(watch_page_, "已保存网络", 12, lv_color_hex(0x6f8794));
        lv_obj_set_pos(saved_heading, 48, 216);
        auto* list = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, 48, 238);
        lv_obj_set_size(list, LV_HOR_RES - 96, 136);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 5, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

        auto& ssid_manager = SsidManager::GetInstance();
        const auto& saved = ssid_manager.GetSsidList();
        if (saved.empty()) {
            auto* empty = WatchLabel(list, "还没有保存的 Wi-Fi\n点下方按钮用手机添加", 12,
                                     lv_color_hex(0x8296a3));
            lv_obj_set_width(empty, LV_HOR_RES - 2 * kScreenSafeInsetX - 12);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_center(empty);
        } else {
            for (size_t i = 0; i < saved.size() && i < 6; ++i) {
                const auto& item = saved[i];
                auto* row = lv_button_create(list);
                lv_obj_set_width(row, LV_HOR_RES - 2 * kScreenSafeInsetX);
                lv_obj_set_height(row, 40);
                lv_obj_set_style_bg_color(row, item.ssid == current_ssid
                                                   ? lv_color_hex(0x173f42)
                                                   : lv_color_hex(0x0d1a24),
                                          0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_radius(row, 10, 0);
                lv_obj_set_style_shadow_width(row, 0, 0);
                lv_obj_add_event_cb(
                    row, HandleWatchEvent, LV_EVENT_CLICKED,
                    reinterpret_cast<void*>(static_cast<std::intptr_t>(kSsidActionBase + i)));
                auto* icon = WatchIcon(row, MATERIAL_SYMBOLS_WIFI, 14,
                                       item.ssid == current_ssid
                                           ? lv_color_hex(0x65e6d6)
                                           : lv_color_hex(0x6f8794));
                lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
                auto* name = WatchLabel(row, item.ssid.c_str(), 12,
                                        item.ssid == current_ssid
                                            ? lv_color_hex(0x65e6d6)
                                            : lv_color_hex(0xeaf5f7));
                lv_obj_set_size(name, LV_HOR_RES - 2 * kScreenSafeInsetX - 52, 22);
                lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, 34, 0);
            }
        }

        auto* scan_button = lv_button_create(watch_page_);
        lv_obj_set_pos(scan_button, 48, 384);
        lv_obj_set_size(scan_button, LV_HOR_RES - 96, 46);
        lv_obj_set_style_radius(scan_button, 20, 0);
        lv_obj_set_style_bg_color(scan_button, lv_color_hex(0x12313a), 0);
        lv_obj_set_style_bg_opa(scan_button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(scan_button, 1, 0);
        lv_obj_set_style_border_color(scan_button, lv_color_hex(0x65e6d6), 0);
        lv_obj_add_event_cb(scan_button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kOpenWifiScan)));
        auto* scan_label = WatchLabel(scan_button, "扫描附近 Wi-Fi 并连接", 13,
                                      lv_color_hex(0x65e6d6));
        lv_obj_center(scan_label);

        auto* phone_button = lv_button_create(watch_page_);
        lv_obj_set_pos(phone_button, 48, 438);
        lv_obj_set_size(phone_button, LV_HOR_RES - 96, 32);
        lv_obj_set_style_radius(phone_button, 16, 0);
        lv_obj_set_style_bg_color(phone_button, lv_color_hex(0x0d1a24), 0);
        lv_obj_set_style_bg_opa(phone_button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(phone_button, 0, 0);
        lv_obj_add_event_cb(phone_button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kEnterWifiConfig)));
        auto* phone_label = WatchLabel(phone_button, "或用手机配网（2.4GHz 热点）", 11,
                                       lv_color_hex(0x8296a3));
        lv_obj_center(phone_label);

        // Leaving the scan page must not strand the device without a network.
        auto* wifi_board = static_cast<WifiBoard*>(&Board::GetInstance());
        if (wifi_board != nullptr)
            wifi_board->SetAutoConfigModeSuppressed(false);
        auto& manager = WifiManager::GetInstance();
        if (!manager.IsConnected() && !manager.IsConfigMode()) {
            if (SsidManager::GetInstance().GetSsidList().empty())
                EnsureWifiConfigMode();
            else
                manager.StartStation();
        }
    }

    static std::vector<WifiApInfo> ScanWifiNetworks() {
        std::vector<WifiApInfo> results;
        esp_wifi_set_mode(WIFI_MODE_STA);
        const esp_err_t start_error = esp_wifi_start();
        const bool started_here = start_error == ESP_OK;
        if (start_error != ESP_OK && start_error != ESP_ERR_WIFI_STATE)
            return results;
        wifi_scan_config_t config = {};
        config.show_hidden = false;
        if (esp_wifi_scan_start(&config, true) == ESP_OK) {
            uint16_t count = 0;
            if (esp_wifi_scan_get_ap_num(&count) == ESP_OK && count > 0) {
                std::vector<wifi_ap_record_t> records(count);
                uint16_t got = count;
                if (esp_wifi_scan_get_ap_records(&got, records.data()) == ESP_OK) {
                    records.resize(got);
                    std::sort(records.begin(), records.end(),
                              [](const wifi_ap_record_t& left, const wifi_ap_record_t& right) {
                                  return left.rssi > right.rssi;
                              });
                    for (const auto& record : records) {
                        if (record.ssid[0] == '\0')
                            continue;
                        WifiApInfo info;
                        info.ssid.assign(reinterpret_cast<const char*>(record.ssid));
                        info.rssi = record.rssi;
                        info.secured = record.authmode != WIFI_AUTH_OPEN;
                        results.push_back(std::move(info));
                    }
                }
            }
        }
        if (started_here)
            esp_wifi_stop();
        return results;
    }

    static void WifiScanTask(void* arg) {
        auto* self = static_cast<CustomLcdDisplay*>(arg);
        auto results = ScanWifiNetworks();
        Application::GetInstance().Schedule([self, results = std::move(results)]() mutable {
            DisplayLockGuard lock(self);
            if (!lock)
                return;
            self->ApplyWifiScanResults(std::move(results));
        });
        vTaskDelete(nullptr);
    }

    void ApplyWifiScanResults(std::vector<WifiApInfo> results) {
        if (wifi_scan_list_ == nullptr)
            return;
        char detail[16];
        std::snprintf(detail, sizeof(detail), "%u found",
                      static_cast<unsigned>(results.size()));
        WatchSystemLog::Event("wifi", "scan_finished", detail);
        wifi_scan_results_ = std::move(results);
        lv_obj_clean(wifi_scan_list_);
        if (wifi_scan_results_.empty()) {
            auto* empty = WatchLabel(wifi_scan_list_, "没有扫描到 Wi-Fi\n点下方重新扫描", 12,
                                     lv_color_hex(0x8296a3));
            lv_obj_set_width(empty, LV_HOR_RES - 2 * kScreenSafeInsetX - 12);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(empty);
            return;
        }
        const auto current = WifiManager::GetInstance().GetSsid();
        for (size_t i = 0; i < wifi_scan_results_.size() && i < 12; ++i) {
            const auto& ap = wifi_scan_results_[i];
            auto* row = lv_button_create(wifi_scan_list_);
            lv_obj_set_width(row, LV_HOR_RES - 2 * kScreenSafeInsetX);
            lv_obj_set_height(row, 44);
            lv_obj_set_style_bg_color(row, ap.ssid == current ? lv_color_hex(0x173f42)
                                                             : lv_color_hex(0x0d1a24),
                                      0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 11, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_add_event_cb(
                row, HandleWatchEvent, LV_EVENT_CLICKED,
                reinterpret_cast<void*>(static_cast<std::intptr_t>(kWifiApBase + static_cast<int>(i))));
            auto* icon = WatchIcon(row, MATERIAL_SYMBOLS_WIFI, 15, lv_color_hex(0x65e6d6));
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
            auto* name = WatchLabel(row, ap.ssid.c_str(), 13,
                                    ap.ssid == current ? lv_color_hex(0x65e6d6)
                                                       : lv_color_hex(0xeaf5f7));
            lv_obj_set_size(name, LV_HOR_RES - 2 * kScreenSafeInsetX - 78, 24);
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 38, 0);
            if (ap.secured) {
                auto* lock = WatchIcon(row, MATERIAL_SYMBOLS_LOCK, 12,
                                       lv_color_hex(0x8296a3));
                lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -14, 0);
            }
        }
        lv_obj_update_layout(wifi_scan_list_);
    }

    void OpenWifiScanPage(bool rescan) {
        if (auto* wifi_board = static_cast<WifiBoard*>(&Board::GetInstance()))
            wifi_board->SetAutoConfigModeSuppressed(true);
        OpenPage("选择 Wi-Fi", kOpenNetwork);
        auto& manager = WifiManager::GetInstance();
        if (manager.IsConnected())
            manager.StopStation();
        if (manager.IsConfigMode())
            manager.StopConfigAp();

        wifi_scan_list_ = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(wifi_scan_list_);
        lv_obj_set_pos(wifi_scan_list_, kScreenSafeInsetX, 74);
        lv_obj_set_size(wifi_scan_list_, LV_HOR_RES - 2 * kScreenSafeInsetX, 336);
        lv_obj_set_layout(wifi_scan_list_, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(wifi_scan_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wifi_scan_list_, 5, 0);
        lv_obj_set_style_pad_all(wifi_scan_list_, 0, 0);
        lv_obj_set_scroll_dir(wifi_scan_list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(wifi_scan_list_, LV_SCROLLBAR_MODE_AUTO);

        auto* rescan_button = lv_button_create(watch_page_);
        lv_obj_set_pos(rescan_button, 48, 424);
        lv_obj_set_size(rescan_button, LV_HOR_RES - 96, 42);
        lv_obj_set_style_radius(rescan_button, 18, 0);
        lv_obj_set_style_bg_color(rescan_button, lv_color_hex(0x12313a), 0);
        lv_obj_set_style_bg_opa(rescan_button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rescan_button, 1, 0);
        lv_obj_set_style_border_color(rescan_button, lv_color_hex(0x65e6d6), 0);
        lv_obj_add_event_cb(rescan_button, [](lv_event_t* event) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
            if (self != nullptr)
                self->OpenWifiScanPage(true);
        }, LV_EVENT_CLICKED, this);
        auto* rescan_label = WatchLabel(rescan_button, "重新扫描", 12, lv_color_hex(0x65e6d6));
        lv_obj_center(rescan_label);

        if (rescan) {
            wifi_scan_results_.clear();
            auto* hint = WatchLabel(wifi_scan_list_, "正在扫描附近的 Wi-Fi …", 12,
                                    lv_color_hex(0x8296a3));
            lv_obj_set_width(hint, LV_HOR_RES - 2 * kScreenSafeInsetX - 12);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(hint);
            xTaskCreate(WifiScanTask, "wifi_scan", 5120, this, 2, nullptr);
        } else {
            ApplyWifiScanResults(wifi_scan_results_);
        }
    }

    void OpenWifiPasswordPage(const std::string& ssid) {
        wifi_pending_ssid_ = ssid;
        OpenPage("输入 Wi-Fi 密码", kOpenNetwork);
        auto* ssid_label = WatchLabel(watch_page_, ssid.c_str(), 15, lv_color_hex(0x65e6d6));
        lv_obj_set_width(ssid_label, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_style_text_align(ssid_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(ssid_label, LV_ALIGN_TOP_MID, 0, 68);

        wifi_pwd_textarea_ = lv_textarea_create(watch_page_);
        lv_obj_set_pos(wifi_pwd_textarea_, kScreenSafeInsetX, 94);
        lv_obj_set_size(wifi_pwd_textarea_, LV_HOR_RES - 2 * kScreenSafeInsetX, 52);
        lv_textarea_set_one_line(wifi_pwd_textarea_, true);
        lv_textarea_set_password_mode(wifi_pwd_textarea_, true);
        lv_textarea_set_placeholder_text(wifi_pwd_textarea_, "输入密码");
        lv_obj_set_style_bg_color(wifi_pwd_textarea_, lv_color_hex(0x101a20), 0);
        lv_obj_set_style_bg_opa(wifi_pwd_textarea_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(wifi_pwd_textarea_, lv_color_hex(0xeaf5f7), 0);
        lv_obj_set_style_border_width(wifi_pwd_textarea_, 1, 0);
        lv_obj_set_style_border_color(wifi_pwd_textarea_, lv_color_hex(0x263941), 0);
        lv_obj_set_style_radius(wifi_pwd_textarea_, 14, 0);

        auto* hint = WatchLabel(watch_page_, "输完点键盘右下角 ✓ 连接", 11,
                                lv_color_hex(0x8296a3));
        lv_obj_set_width(hint, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 152);

        auto* keyboard = lv_keyboard_create(watch_page_);
        lv_obj_set_size(keyboard, LV_HOR_RES - 12, 300);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x0d1a24), 0);
        lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(keyboard, 0, 0);
        lv_obj_set_style_radius(keyboard, 16, 0);
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(keyboard, wifi_pwd_textarea_);
        lv_obj_add_event_cb(keyboard, HandleWifiKeyboardEvent, LV_EVENT_READY, this);
        lv_obj_add_event_cb(keyboard, HandleWifiKeyboardEvent, LV_EVENT_CANCEL, this);
    }

    static void HandleWifiKeyboardEvent(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
            Application::GetInstance().Schedule([self] {
                DisplayLockGuard lock(self);
                if (lock)
                    self->OpenWifiScanPage(false);
            });
            return;
        }
        std::string password;
        if (self->wifi_pwd_textarea_ != nullptr) {
            const char* text = lv_textarea_get_text(self->wifi_pwd_textarea_);
            if (text != nullptr)
                password = text;
        }
        const auto ssid = self->wifi_pending_ssid_;
        Application::GetInstance().Schedule([self, ssid, password]() mutable {
            DisplayLockGuard lock(self);
            if (lock)
                self->ConnectWifi(ssid, password);
        });
    }

    void ConnectWifi(const std::string& ssid, const std::string& password) {
        if (ssid.empty())
            return;
        wifi_connect_ssid_ = ssid;
        wifi_connect_ticks_ = 0;
        SsidManager::GetInstance().AddSsid(ssid, password);
        WatchSystemLog::Event("wifi", "on_device_connect", ssid.c_str());
        OpenPage("连接 Wi-Fi", kOpenNetwork);
        wifi_connect_status_ = WatchLabel(watch_page_, ("正在连接 " + ssid).c_str(), 18,
                                          lv_color_hex(0xeaf5f7));
        lv_obj_set_width(wifi_connect_status_, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_label_set_long_mode(wifi_connect_status_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(wifi_connect_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(wifi_connect_status_, LV_ALIGN_TOP_MID, 0, 200);
        wifi_connect_hint_ = WatchLabel(watch_page_, "最多等待 20 秒", 12,
                                        lv_color_hex(0x8296a3));
        lv_obj_set_width(wifi_connect_hint_, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_style_text_align(wifi_connect_hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(wifi_connect_hint_, LV_ALIGN_TOP_MID, 0, 240);

        auto& manager = WifiManager::GetInstance();
        manager.StopStation();
        manager.StartStation();
        if (wifi_connect_timer_ == nullptr) {
            wifi_connect_timer_ = lv_timer_create(OnWifiConnectTimer, 1000, this);
        } else {
            lv_timer_reset(wifi_connect_timer_);
            lv_timer_resume(wifi_connect_timer_);
        }
    }

    static void OnWifiConnectTimer(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self != nullptr)
            self->UpdateWifiConnectState();
    }

    void UpdateWifiConnectState() {
        auto& manager = WifiManager::GetInstance();
        ++wifi_connect_ticks_;
        if (manager.IsConnected()) {
            if (auto* wifi_board = static_cast<WifiBoard*>(&Board::GetInstance()))
                wifi_board->SetAutoConfigModeSuppressed(false);
            if (wifi_connect_timer_ != nullptr)
                lv_timer_pause(wifi_connect_timer_);
            if (wifi_connect_status_ != nullptr)
                lv_label_set_text_fmt(wifi_connect_status_, "已连接 %s",
                                      manager.GetSsid().c_str());
            if (wifi_connect_hint_ != nullptr)
                lv_label_set_text(wifi_connect_hint_, "点左上角返回");
            return;
        }
        if (wifi_connect_ticks_ > 20) {
            if (wifi_connect_timer_ != nullptr)
                lv_timer_pause(wifi_connect_timer_);
            if (wifi_connect_status_ != nullptr) {
                lv_obj_set_style_text_color(wifi_connect_status_, lv_color_hex(0xff8090), 0);
                lv_label_set_text(wifi_connect_status_, "连接失败");
            }
            if (wifi_connect_hint_ != nullptr)
                lv_label_set_text_fmt(wifi_connect_hint_, "请检查 %s 的密码后重试",
                                      wifi_connect_ssid_.c_str());
        }
    }

    void OpenSettingsPage() {
        OpenPage("设置", kOpenApps);
        // The whole settings panel fits on one screen, so do not scroll a
        // full-screen page (that path is expensive on the QSPI AMOLED).
        lv_obj_clear_flag(watch_page_, LV_OBJ_FLAG_SCROLLABLE);

        auto card = [this](int y, int height) {
            auto* box = lv_obj_create(watch_page_);
            lv_obj_remove_style_all(box);
            lv_obj_set_pos(box, 48, y);
            lv_obj_set_size(box, LV_HOR_RES - 96, height);
            lv_obj_set_style_bg_color(box, lv_color_hex(0x101a20), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x263941), 0);
            lv_obj_set_style_radius(box, 20, 0);
            lv_obj_set_style_shadow_width(box, 0, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
            return box;
        };
        auto style_slider = [](lv_obj_t* slider, lv_color_t accent) {
            lv_obj_set_style_bg_color(slider, lv_color_hex(0x1b2932), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(slider, accent, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, lv_color_hex(0xf4f7fc), LV_PART_KNOB);
            lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_pad_all(slider, 0, 0);
        };

        auto* face_card = card(80, 88);
        auto* face_icon = WatchIcon(face_card, MATERIAL_SYMBOLS_WATCH, 20,
                                    lv_color_hex(0x8fc6ff));
        lv_obj_align(face_icon, LV_ALIGN_TOP_LEFT, 16, 8);
        auto* face_title = WatchLabel(face_card, "表盘", 16, lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(face_title, 50, 4);
        settings_face_current_ = WatchLabel(face_card, "", 10, lv_color_hex(0x65e6d6));
        lv_obj_align(settings_face_current_, LV_ALIGN_TOP_RIGHT, -14, 12);
        static const char* face_short_names[] = {"金色", "运动", "经典", "鲸鱼娘"};
        const WatchAction face_actions[] = {kFaceLuxe, kFaceSport, kFaceClassic, kFaceCute};
        for (int i = 0; i < 4; ++i) {
            auto* button = lv_obj_create(face_card);
            lv_obj_remove_style_all(button);
            lv_obj_set_pos(button, 12 + i * 70, 42);
            lv_obj_set_size(button, 64, 38);
            lv_obj_set_style_radius(button, 14, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x0c1b24), 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(button, 1, 0);
            lv_obj_set_style_border_color(button, lv_color_hex(0x294252), 0);
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<std::intptr_t>(face_actions[i])));
            auto* label = WatchLabel(button, face_short_names[i], 11, lv_color_hex(0xeaf5f7));
            lv_obj_center(label);
            settings_face_buttons_[i] = button;
        }

        auto* brightness_card = card(176, 74);
        auto* brightness_icon = WatchIcon(brightness_card, MATERIAL_SYMBOLS_BRIGHTNESS_6, 20,
                                         lv_color_hex(0xe9c987));
        lv_obj_align(brightness_icon, LV_ALIGN_TOP_LEFT, 16, 8);
        auto* brightness_title = WatchLabel(brightness_card, "屏幕亮度", 16,
                                            lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(brightness_title, 50, 4);
        auto* backlight = Board::GetInstance().GetBacklight();
        const int brightness_value = backlight ? backlight->brightness() : 70;
        char brightness_text[8];
        std::snprintf(brightness_text, sizeof(brightness_text), "%d%%", brightness_value);
        settings_brightness_value_ = WatchLabel(brightness_card, brightness_text, 14,
                                                lv_color_hex(0xe9c987));
        lv_obj_align(settings_brightness_value_, LV_ALIGN_TOP_RIGHT, -16, 4);
        auto* brightness = lv_slider_create(brightness_card);
        lv_obj_set_pos(brightness, 16, 42);
        lv_obj_set_size(brightness, LV_HOR_RES - 128, 12);
        lv_slider_set_range(brightness, 5, 100);
        lv_slider_set_value(brightness, brightness_value, LV_ANIM_OFF);
        style_slider(brightness, lv_color_hex(0xe9c987));
        lv_obj_add_event_cb(
            brightness, HandleWatchEvent, LV_EVENT_VALUE_CHANGED,
            reinterpret_cast<void*>(static_cast<std::intptr_t>(kBrightnessChanged)));
        lv_obj_add_event_cb(
            brightness, HandleWatchEvent, LV_EVENT_RELEASED,
            reinterpret_cast<void*>(static_cast<std::intptr_t>(kBrightnessChanged)));

        auto* volume_card = card(258, 74);
        auto* volume_icon = WatchIcon(volume_card, MATERIAL_SYMBOLS_VOLUME_UP, 20,
                                      lv_color_hex(0x65e6d6));
        lv_obj_align(volume_icon, LV_ALIGN_TOP_LEFT, 16, 8);
        auto* volume_title = WatchLabel(volume_card, "语音音量", 16, lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(volume_title, 50, 4);
        auto* codec = Board::GetInstance().GetAudioCodec();
        const int volume_value = codec ? codec->output_volume() : 55;
        char volume_text[8];
        std::snprintf(volume_text, sizeof(volume_text), "%d%%", volume_value);
        settings_volume_value_ = WatchLabel(volume_card, volume_text, 14,
                                            lv_color_hex(0x65e6d6));
        lv_obj_align(settings_volume_value_, LV_ALIGN_TOP_RIGHT, -16, 4);
        auto* volume = lv_slider_create(volume_card);
        lv_obj_set_pos(volume, 16, 42);
        lv_obj_set_size(volume, LV_HOR_RES - 128, 12);
        lv_slider_set_range(volume, 0, 100);
        lv_slider_set_value(volume, volume_value, LV_ANIM_OFF);
        style_slider(volume, lv_color_hex(0x65e6d6));
        lv_obj_add_event_cb(volume, HandleWatchEvent, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kVolumeChanged)));
        lv_obj_add_event_cb(volume, HandleWatchEvent, LV_EVENT_RELEASED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kVolumeChanged)));

        auto* dnd_card = card(340, 58);
        auto* dnd_icon = WatchIcon(dnd_card, MATERIAL_SYMBOLS_NOTIFICATIONS, 18,
                                   lv_color_hex(0xb4c4ce));
        lv_obj_align(dnd_icon, LV_ALIGN_TOP_LEFT, 16, 12);
        auto* dnd_title = WatchLabel(dnd_card, "勿扰模式", 16, lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(dnd_title, 50, 4);
        auto* dnd_note = WatchLabel(dnd_card, "闹钟与倒计时只显示提醒", 11,
                                    lv_color_hex(0x8296a3));
        lv_obj_set_pos(dnd_note, 50, 30);
        auto* dnd = lv_switch_create(dnd_card);
        lv_obj_align(dnd, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_bg_color(dnd, lv_color_hex(0x1b2932), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dnd, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dnd, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dnd, lv_color_hex(0x65e6d6), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(dnd, lv_color_hex(0xf4f7fc), LV_PART_KNOB);
        Settings preferences("watchui", true);
        if (preferences.GetBool("dnd", false))
            lv_obj_add_state(dnd, LV_STATE_CHECKED);
        lv_obj_add_event_cb(dnd, HandleWatchEvent, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kDndChanged)));

        auto* pairing_card = card(406, 58);
        lv_obj_add_flag(pairing_card, LV_OBJ_FLAG_CLICKABLE);
        auto* pairing_icon = WatchIcon(pairing_card, MATERIAL_SYMBOLS_LINK, 18,
                                       lv_color_hex(0x8fc6ff));
        lv_obj_align(pairing_icon, LV_ALIGN_TOP_LEFT, 16, 12);
        auto* pairing_title = WatchLabel(pairing_card, "网络与配对", 16,
                                         lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(pairing_title, 50, 4);
        auto* pairing_note = WatchLabel(pairing_card, "Wi-Fi · 手机热点 · 配网", 11,
                                        lv_color_hex(0x8296a3));
        lv_obj_set_pos(pairing_note, 50, 30);
        auto* chevron = WatchLabel(pairing_card, "›", 22, lv_color_hex(0x8296a3));
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_add_event_cb(
            pairing_card, HandleWatchEvent, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<std::intptr_t>(kOpenNetwork)));

        UpdateSettingsFaceSelection();
    }

    // Shows which folder was indexed and how many tracks it holds, so the
    // music page never silently mixes in other folders.
    std::string LibrarySummary() {
        auto& player = WatchMusicPlayer::GetInstance();
        if (player.IsScanning())
            return "正在索引 SD 卡曲库";
        if (!player.IsMounted())
            return "未检测到 SD 卡";
        const auto count = player.TrackCount();
        const auto root = player.LibraryRoot();
        if (count == 0)
            return root.empty() ? "未找到 Music 文件夹" : "Music 文件夹为空";
        std::string summary = "曲库 " + std::to_string(count) + " 首 · ";
        if (root.empty())
            summary += "卡根目录";
        else
            summary += root;
        return summary;
    }

    void OpenMusicPage() {
        OpenPage("音乐", kOpenApps);
        auto& player = WatchMusicPlayer::GetInstance();
        player.Initialize();
        music_page_scanning_ = player.IsScanning();
        auto* list_button = WatchButton(watch_page_, "列表", 292, 28, 54, 42, kOpenMusicList);
        (void)list_button;

        std::string initial_status =
            music_page_scanning_ ? "正在扫描 SD 卡..." : player.Status();
        if (initial_status.rfind("当前没有", 0) == 0)
            initial_status = "未播放";
        if (initial_status.rfind("正在播放：", 0) == 0)
            initial_status = initial_status.substr(15);
        else if (initial_status.rfind("已暂停：", 0) == 0)
            initial_status = initial_status.substr(12);

        EnsureEmbeddedImagesDecoded();
        auto* art = lv_image_create(watch_page_);
        lv_obj_remove_style_all(art);
        lv_obj_set_pos(art, 95, 75);
        lv_obj_set_size(art, 220, 205);
        lv_obj_set_style_radius(art, 30, 0);
        lv_obj_set_style_clip_corner(art, true, 0);
        lv_obj_set_style_border_width(art, 1, 0);
        lv_obj_set_style_border_color(art, lv_color_hex(0x2b6577), 0);
        lv_obj_set_style_shadow_width(art, 14, 0);
        lv_obj_set_style_shadow_color(art, lv_color_hex(0x17617a), 0);
        lv_obj_set_style_shadow_opa(art, LV_OPA_20, 0);
        lv_image_set_src(art, &music_wave_dsc_);

        watch_music_status_ = WatchLabel(watch_page_, initial_status.c_str(), 30,
                                         lv_color_hex(0xf2f7fb));
        lv_obj_set_width(watch_music_status_, LV_HOR_RES - 72);
        lv_obj_set_height(watch_music_status_, 34);
        lv_obj_align(watch_music_status_, LV_ALIGN_TOP_MID, 0, 281);
        lv_obj_set_style_text_align(watch_music_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(watch_music_status_, LV_LABEL_LONG_MODE_DOTS);

        auto* library_label = WatchLabel(watch_page_, LibrarySummary().c_str(), 17,
                                         lv_color_hex(0xa9bac4));
        lv_obj_set_width(library_label, LV_HOR_RES - 100);
        lv_label_set_long_mode(library_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(library_label, LV_ALIGN_TOP_MID, 0, 315);
        lv_obj_set_style_text_align(library_label, LV_TEXT_ALIGN_CENTER, 0);

        watch_music_progress_bar_ = lv_bar_create(watch_page_);
        lv_obj_set_pos(watch_music_progress_bar_, 26, 351);
        lv_obj_set_size(watch_music_progress_bar_, LV_HOR_RES - 52, 6);
        lv_bar_set_range(watch_music_progress_bar_, 0, 100);
        lv_bar_set_value(watch_music_progress_bar_, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_opa(watch_music_progress_bar_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(watch_music_progress_bar_, LV_OPA_COVER,
                                LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(watch_music_progress_bar_, lv_color_hex(0x45677a), LV_PART_MAIN);
        lv_obj_set_style_bg_color(watch_music_progress_bar_, lv_color_hex(0x65e6d6),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_radius(watch_music_progress_bar_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(watch_music_progress_bar_, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        watch_music_progress_ = WatchLabel(watch_page_, "00:00", 11,
                                           lv_color_hex(0xa9bac4));
        lv_obj_set_width(watch_music_progress_, 70);
        lv_obj_align(watch_music_progress_, LV_ALIGN_TOP_LEFT, 26, 360);
        watch_music_duration_ = WatchLabel(watch_page_, "--:--", 11,
                                           lv_color_hex(0xa9bac4));
        lv_obj_set_width(watch_music_duration_, 70);
        lv_obj_set_style_text_align(watch_music_duration_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(watch_music_duration_, LV_ALIGN_TOP_RIGHT, -26, 360);

        const int row_y = 400;
        auto* shuffle = lv_button_create(watch_page_);
        lv_obj_set_pos(shuffle, 31, row_y);
        lv_obj_set_size(shuffle, 60, 60);
        lv_obj_set_style_radius(shuffle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(shuffle, lv_color_hex(0x142a3a), 0);
        lv_obj_set_style_bg_opa(shuffle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(shuffle, 1, 0);
        lv_obj_set_style_border_color(shuffle, lv_color_hex(0x2e5064), 0);
        lv_obj_set_style_border_opa(shuffle, LV_OPA_60, 0);
        lv_obj_set_style_shadow_width(shuffle, 0, 0);
        lv_obj_set_style_shadow_opa(shuffle, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(shuffle, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kMusicShuffle)));
        auto* shuffle_label = WatchLabel(
            shuffle, "随机", 11,
            lv_color_hex(player.ShuffleEnabled() ? 0x65e6d6 : 0xa9bac4));
        lv_obj_set_width(shuffle_label, 60);
        lv_obj_set_style_text_align(shuffle_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(shuffle_label);
        watch_music_shuffle_button_ = shuffle;

        WatchIconButton(watch_page_, MATERIAL_SYMBOLS_SKIP_PREVIOUS, 115, row_y,
                        60, 60, kMusicPrevious, lv_color_hex(0xc6d5df),
                        lv_color_hex(0x65e6d6));
        watch_music_play_button_ = WatchIconButton(
            watch_page_, MATERIAL_SYMBOLS_PLAY_ARROW, 198, row_y - 4, 74, 74,
            kMusicPlayPause, lv_color_hex(0x65e6d6), lv_color_hex(0x65e6d6));
        WatchIconButton(watch_page_, MATERIAL_SYMBOLS_SKIP_NEXT, 304, row_y,
                        60, 60, kMusicNext, lv_color_hex(0xc6d5df),
                        lv_color_hex(0x65e6d6));
        UpdateMusicProgress();
    }

    // Drops the list widgets and its polling timer. Safe to call repeatedly;
    // OpenPage() uses it whenever the watch leaves the music list.
    void ResetMusicList() {
        if (music_list_timer_ != nullptr) {
            lv_timer_delete(music_list_timer_);
            music_list_timer_ = nullptr;
        }
        watch_music_list_ = nullptr;
        music_list_first_ = 0;
        music_list_rows_ = 0;
        music_list_refilling_ = false;
    }

    void AddMusicListRow(int catalog_index, const std::string& track_id,
                         const std::string& title, const std::string& current_track) {
        const bool current = !track_id.empty() && track_id == current_track;
        auto* row = lv_button_create(watch_music_list_);
        lv_obj_set_width(row, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_height(row, kMusicRowHeight);
        lv_obj_set_style_bg_color(row, current ? lv_color_hex(0x173f42)
                                               : lv_color_hex(0x0d1a24),
                                  0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 11, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, HandleMusicTrackEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(catalog_index + 1)));
        char number[8];
        std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(catalog_index + 1));
        auto* index_label = WatchLabel(row, number, 9, lv_color_hex(0x6f8794));
        lv_obj_align(index_label, LV_ALIGN_LEFT_MID, 10, 0);
        auto* title_label = WatchLabel(row, title.c_str(), 14,
                                       current ? lv_color_hex(0x65e6d6)
                                               : lv_color_hex(0xeaf5f7));
        lv_obj_set_size(title_label, LV_HOR_RES - 2 * kScreenSafeInsetX - 72, 32);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 44, 0);
        auto* note = WatchIcon(row, MATERIAL_SYMBOLS_PLAY_ARROW, 12,
                               current ? lv_color_hex(0x65e6d6) : lv_color_hex(0x3f5560));
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    // Materializes the rows [first, first + kMusicWindowRows) and keeps
    // `keep_visible` on the same spot of the screen, so scrolling stays
    // continuous while the window slides through the catalog.
    void FillMusicWindow(int first, int keep_visible) {
        if (watch_music_list_ == nullptr)
            return;
        music_list_refilling_ = true;
        lv_obj_clean(watch_music_list_);
        music_list_first_ = 0;
        music_list_rows_ = 0;
        auto& player = WatchMusicPlayer::GetInstance();
        const int total = static_cast<int>(player.TrackCount());
        if (total <= 0) {
            auto* empty = WatchLabel(watch_music_list_, "没有歌曲\n在卡根目录建 Music 文件夹", 12,
                                     lv_color_hex(0xa9bac4));
            lv_obj_set_width(empty, LV_HOR_RES - 2 * kScreenSafeInsetX - 12);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(empty);
            music_list_refilling_ = false;
            return;
        }
        const int max_first = std::max(0, total - kMusicWindowRows);
        first = std::clamp(first, 0, max_first);
        const int count = std::min(kMusicWindowRows, total - first);
        const auto library =
            player.Library(static_cast<size_t>(first), static_cast<size_t>(count));
        const auto current_track = player.CurrentTrackId();
        for (size_t i = 0; i < library.size(); ++i) {
            AddMusicListRow(first + static_cast<int>(i), library[i].first, library[i].second,
                            current_track);
        }
        music_list_first_ = first;
        music_list_rows_ = static_cast<int>(library.size());
        lv_obj_update_layout(watch_music_list_);
        const int offset_rows =
            std::clamp(keep_visible - first, 0, std::max(0, music_list_rows_ - 1));
        lv_obj_scroll_to_y(watch_music_list_, offset_rows * kMusicRowStride, LV_ANIM_OFF);
        music_list_refilling_ = false;
    }

    // Preloads before the user reaches the edge of the window. The two
    // thresholds are separated so a refill never immediately triggers the
    // opposite refill.
    void UpdateMusicListWindow() {
        if (watch_music_list_ == nullptr || music_list_refilling_)
            return;
        auto& player = WatchMusicPlayer::GetInstance();
        const int total = static_cast<int>(player.TrackCount());
        if (total <= 0)
            return;
        if (music_list_rows_ <= 0) {
            FillMusicWindow(0, 0);
            return;
        }
        const int viewport_rows =
            (lv_obj_get_height(watch_music_list_) + kMusicRowStride - 1) / kMusicRowStride;
        const int scroll_y = lv_obj_get_scroll_y(watch_music_list_);
        const int visible_first = music_list_first_ + scroll_y / kMusicRowStride;
        const int window_last = music_list_first_ + music_list_rows_ - 1;
        const int window_rows = std::max(1, music_list_rows_);
        const int down_trigger = music_list_first_ + std::max(0, window_rows - viewport_rows -
                                                                   kMusicPreloadRows);
        if (visible_first >= down_trigger && window_last + 1 < total) {
            const int max_first = std::max(0, total - kMusicWindowRows);
            const int next_first =
                std::min(max_first, std::max(0, visible_first - kMusicPreloadRows));
            if (next_first > music_list_first_)
                FillMusicWindow(next_first, visible_first);
            return;
        }
        if (music_list_first_ > 0 && visible_first - 3 < music_list_first_) {
            const int next_first = std::max(
                0, visible_first - (kMusicWindowRows - viewport_rows - 2 * kMusicPreloadRows));
            if (next_first < music_list_first_)
                FillMusicWindow(next_first, visible_first);
        }
    }

    static void OnMusicListTimer(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr)
            return;
        self->UpdateMusicListWindow();
    }

    void OpenMusicListPage() {
        auto& player = WatchMusicPlayer::GetInstance();
        player.Initialize();
        const int total = static_cast<int>(player.TrackCount());
        std::string title = total > 0 ? "歌曲 · " + std::to_string(total) + " 首" : "歌曲";
        OpenPage(title.c_str(), kBackMusicPlayer);
        music_list_open_ = true;

        watch_music_list_ = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(watch_music_list_);
        lv_obj_set_pos(watch_music_list_, kScreenSafeInsetX, 78);
        lv_obj_set_size(watch_music_list_, LV_HOR_RES - 2 * kScreenSafeInsetX, 384);
        lv_obj_set_layout(watch_music_list_, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(watch_music_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(watch_music_list_, kMusicRowGap, 0);
        lv_obj_set_style_pad_all(watch_music_list_, 0, 0);
        lv_obj_set_scroll_dir(watch_music_list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(watch_music_list_, LV_SCROLLBAR_MODE_AUTO);

        FillMusicWindow(0, 0);
        if (music_list_timer_ == nullptr)
            music_list_timer_ = lv_timer_create(OnMusicListTimer, 100, this);
    }


    lv_obj_t* WatchAppOrb(lv_obj_t* parent, WatchAppIcon icon_kind, const char* title, int x,
                          int y, WatchAction action, lv_color_t accent) {
        auto* button = lv_button_create(parent);
        lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, 100, 132);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(button, HandleAppOrbTouch, LV_EVENT_PRESSED,
                            const_cast<char*>(title));
        lv_obj_add_event_cb(button, HandleAppOrbTouch, LV_EVENT_RELEASED,
                            const_cast<char*>(title));
        lv_obj_add_event_cb(button, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(action)));

        const char* glyph = MATERIAL_SYMBOLS_SETTINGS;
        switch (icon_kind) {
            case WatchAppIcon::Assistant: glyph = MATERIAL_SYMBOLS_ROBOT_2; break;
            case WatchAppIcon::Music: glyph = MATERIAL_SYMBOLS_MUSIC_NOTE; break;
            case WatchAppIcon::Activity: glyph = MATERIAL_SYMBOLS_PROGRESS_ACTIVITY; break;
            case WatchAppIcon::Alarm: glyph = MATERIAL_SYMBOLS_ALARM; break;
            case WatchAppIcon::Timer: glyph = MATERIAL_SYMBOLS_SCHEDULE; break;
            case WatchAppIcon::Settings: glyph = MATERIAL_SYMBOLS_SETTINGS; break;
        }
        const char* note = "快速打开";
        switch (icon_kind) {
            case WatchAppIcon::Assistant: note = "语音助手"; break;
            case WatchAppIcon::Music: note = "本地音乐"; break;
            case WatchAppIcon::Activity: note = "今日步数"; break;
            case WatchAppIcon::Alarm: note = "每日闹钟"; break;
            case WatchAppIcon::Timer: note = "快捷计时"; break;
            case WatchAppIcon::Settings: note = "设备设置"; break;
        }
        lv_color_t gradient = lv_color_hex(0x6d7cf5);
        switch (icon_kind) {
            case WatchAppIcon::Assistant: gradient = lv_color_hex(0x55b8ff); break;
            case WatchAppIcon::Music: gradient = lv_color_hex(0xff875d); break;
            case WatchAppIcon::Activity: gradient = lv_color_hex(0x45d2a0); break;
            case WatchAppIcon::Alarm: gradient = lv_color_hex(0xffc34d); break;
            case WatchAppIcon::Timer: gradient = lv_color_hex(0x73b7ff); break;
            case WatchAppIcon::Settings: gradient = lv_color_hex(0x9aa9bb); break;
        }
        auto* badge = lv_obj_create(button);
        lv_obj_remove_style_all(badge);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(badge, 74, 74);
        lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(badge, 23, 0);
        lv_obj_set_style_bg_color(badge, accent, 0);
        lv_obj_set_style_bg_grad_color(badge, gradient, 0);
        lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(0xdceaff), 0);
        lv_obj_set_style_border_opa(badge, LV_OPA_30, 0);
        lv_obj_set_style_shadow_width(badge, 15, 0);
        lv_obj_set_style_shadow_color(badge, accent, 0);
        lv_obj_set_style_shadow_opa(badge, LV_OPA_30, 0);
        auto* icon = WatchIcon(badge, glyph, 30, lv_color_hex(0xffffff));
        lv_obj_center(icon);
        auto* label = WatchLabel(button, title, 14, lv_color_hex(0xf2f7fb));
        lv_obj_set_width(label, 100);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 82);
        auto* detail = WatchLabel(button, note, 10, lv_color_hex(0xa9bac4));
        lv_obj_set_width(detail, 100);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 108);
        return button;
    }

    void OpenAppsPage() {
        OpenPage("应用", kBackHome, false);
        lv_obj_set_style_bg_color(watch_page_, lv_color_hex(0x0a1a2b), 0);
        struct AppTile {
            const lv_image_dsc_t* icon;
            const char* label;
            WatchAction action;
        };
        static const AppTile kTiles[9] = {
            {&app_icon_assistant, "大肥鱼", kStartAssistant},
            {&app_icon_music, "音乐", kOpenMusic},
            {&app_icon_activity, "活动", kOpenActivity},
            {&app_icon_alarm, "闹钟", kOpenAlarm},
            {&app_icon_timer, "倒计时", kOpenTimer},
            {&app_icon_settings, "设置", kOpenSettings},
            {&app_icon_voice_memo, "录音", kOpenVoiceMemo},
            {&app_icon_photos, "图片", kOpenPhotos},
            {&app_icon_videos, "视频", kOpenVideos},
        };
        // Keep the grid clear of the panel's 84 px rounded corners: the outer
        // cells are inset and the first row starts below the corner arc.
        constexpr int kCellWidth = 126;
        constexpr int kCellHeight = 144;
        const int origin_x = (LV_HOR_RES - 3 * kCellWidth) / 2;
        const int origin_y = 44;
        for (int index = 0; index < 9; ++index) {
            const int column = index % 3;
            const int row = index / 3;
            auto* button = lv_button_create(watch_page_);
            lv_obj_remove_style_all(button);
            lv_obj_set_pos(button, origin_x + column * kCellWidth, origin_y + row * kCellHeight);
            lv_obj_set_size(button, kCellWidth, kCellHeight);
            lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
            lv_obj_set_style_shadow_width(button, 0, 0);
            lv_obj_add_event_cb(
                button, HandleWatchEvent, LV_EVENT_CLICKED,
                reinterpret_cast<void*>(static_cast<std::intptr_t>(kTiles[index].action)));
            auto* icon = lv_image_create(button);
            lv_image_set_src(icon, kTiles[index].icon);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
            auto* label = WatchLabel(button, kTiles[index].label, 13, lv_color_hex(0xeaf5f7));
            lv_obj_set_width(label, kCellWidth - 6);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }

    void ShowMediaEmptyState(const char* title, const char* hint) {
        auto* icon = WatchIcon(watch_page_, MATERIAL_SYMBOLS_IMAGE, 34, lv_color_hex(0x4f6470));
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -52);
        auto* heading = WatchLabel(watch_page_, title, 16, lv_color_hex(0xeaf5f7));
        lv_obj_set_width(heading, LV_HOR_RES - 60);
        lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(heading, LV_ALIGN_CENTER, 0, -2);
        auto* detail = WatchLabel(watch_page_, hint, 11, lv_color_hex(0x8296a3));
        lv_obj_set_width(detail, LV_HOR_RES - 72);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(detail, LV_ALIGN_CENTER, 0, 36);
    }

    static void HandleMediaRowEvent(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (self == nullptr)
            return;
        const auto encoded = reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event));
        if (encoded < 0)
            return;
        const auto index = static_cast<size_t>(encoded);
        const bool video = self->media_video_mode_;
        Application::GetInstance().Schedule([self, index, video] {
            DisplayLockGuard lock(self);
            if (!lock)
                return;
            if (video)
                self->OpenVideoPlayer(index);
            else
                self->OpenPhotoViewer(index);
        });
    }

    static void HandlePhotoNavigate(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (self == nullptr || self->media_entries_.empty())
            return;
        const auto delta = reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event));
        if (delta == 0)
            return;
        const auto count = self->media_entries_.size();
        const auto next = (self->photo_index_ + count + (delta > 0 ? 1 : count - 1)) % count;
        Application::GetInstance().Schedule([self, next] {
            DisplayLockGuard lock(self);
            if (lock)
                self->OpenPhotoViewer(next);
        });
    }

    void OpenPhotosPage() {
        OpenPage("图片", kOpenApps);
        media_video_mode_ = false;
        if (!WatchMedia::SdCardReady()) {
            ShowMediaEmptyState("未检测到 TF 卡", "插入 TF 卡后重新进入");
            return;
        }
        media_entries_ = WatchMedia::ListPhotos();
        if (media_entries_.empty()) {
            ShowMediaEmptyState("没有照片", "把 JPG 图片放到 /sdcard/Photos");
            return;
        }
        auto* list = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, kScreenSafeInsetX, 72);
        lv_obj_set_size(list, LV_HOR_RES - 2 * kScreenSafeInsetX, 396);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 5, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        for (size_t index = 0; index < media_entries_.size(); ++index) {
            AddMediaRow(list, media_entries_[index], index);
        }
    }

    void OpenVideosPage() {
        OpenPage("视频", kOpenApps);
        media_video_mode_ = true;
        if (!WatchMedia::SdCardReady()) {
            ShowMediaEmptyState("未检测到 TF 卡", "插入 TF 卡后重新进入");
            return;
        }
        media_entries_ = WatchMedia::ListVideos();
        if (media_entries_.empty()) {
            ShowMediaEmptyState("没有视频", "把 MJPEG 编码的 .avi 放到 /sdcard/Video");
            return;
        }
        auto* list = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, kScreenSafeInsetX, 72);
        lv_obj_set_size(list, LV_HOR_RES - 2 * kScreenSafeInsetX, 396);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 5, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        for (size_t index = 0; index < media_entries_.size(); ++index) {
            AddMediaRow(list, media_entries_[index], index);
        }
    }

    void AddMediaRow(lv_obj_t* list, const MediaFile& entry, size_t index) {
        auto* row = lv_button_create(list);
        lv_obj_set_width(row, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_height(row, 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0d1a24), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 11, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, HandleMediaRowEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(index)));
        auto* name = WatchLabel(row, entry.name.c_str(), 13, lv_color_hex(0xeaf5f7));
        lv_obj_set_width(name, LV_HOR_RES - 2 * kScreenSafeInsetX - 96);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        char size_text[16];
        std::snprintf(size_text, sizeof(size_text), "%ld KB", entry.size / 1024);
        auto* size = WatchLabel(row, size_text, 11, lv_color_hex(0x8296a3));
        lv_obj_align(size, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    void OpenPhotoViewer(size_t index) {
        if (index >= media_entries_.size())
            return;
        OpenPage(media_entries_[index].name.c_str(), kOpenPhotos);
        media_video_mode_ = false;
        photo_index_ = index;
        int width = 0;
        int height = 0;
        // 480 KiB keeps a full-screen photo plus the MultiNet heap well inside
        // the 8 MiB PSRAM budget; bigger images fall back to 1/2 or 1/4 decode.
        if (!WatchMedia::DecodeJpegFile(media_entries_[index].path, 480 * 1024, photo_bytes_, width,
                                        height)) {
            ShowMediaEmptyState("无法显示这张图片", "仅支持 JPG；过大的图片会自动缩小");
            return;
        }
        photo_dsc_ = {};
        photo_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        photo_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        photo_dsc_.header.w = static_cast<uint32_t>(width);
        photo_dsc_.header.h = static_cast<uint32_t>(height);
        photo_dsc_.header.stride = static_cast<uint32_t>(width) * 2;
        photo_dsc_.data = photo_bytes_.data();
        photo_dsc_.data_size = photo_bytes_.size();
        photo_image_ = lv_image_create(watch_page_);
        lv_image_set_src(photo_image_, &photo_dsc_);
        lv_obj_clear_flag(photo_image_, LV_OBJ_FLAG_CLICKABLE);
        const uint32_t scale_x = static_cast<uint32_t>(388 * 256) / width;
        const uint32_t scale_y = static_cast<uint32_t>(424 * 256) / height;
        lv_image_set_scale(photo_image_, std::min(scale_x, scale_y));
        lv_obj_align(photo_image_, LV_ALIGN_CENTER, 0, 2);

        const int zone_width = LV_HOR_RES / 3;
        for (int direction = -1; direction <= 1; direction += 2) {
            auto* zone = lv_obj_create(watch_page_);
            lv_obj_remove_style_all(zone);
            lv_obj_set_pos(zone, direction < 0 ? 0 : LV_HOR_RES - zone_width, 66);
            lv_obj_set_size(zone, zone_width, LV_VER_RES - 108);
            lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(
                zone, HandlePhotoNavigate, LV_EVENT_CLICKED,
                reinterpret_cast<void*>(static_cast<std::intptr_t>(direction)));
        }
        auto* hint = WatchLabel(watch_page_, "轻点左右两侧切换", 10, lv_color_hex(0x6f8794));
        lv_obj_set_width(hint, LV_HOR_RES - 40);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
    }

    void OpenVideoPlayer(size_t index) {
        if (index >= media_entries_.size())
            return;
        // AVI audio and the music player share one PCM queue; stop music first.
        WatchMusicPlayer::GetInstance().Stop();
        OpenPage(media_entries_[index].name.c_str(), kOpenVideos);
        media_video_mode_ = true;
        video_dsc_ = {};
        video_image_ = lv_image_create(watch_page_);
        lv_obj_align(video_image_, LV_ALIGN_CENTER, 0, 2);
        lv_obj_clear_flag(video_image_, LV_OBJ_FLAG_CLICKABLE);
        auto* hint = WatchLabel(watch_page_, "MJPEG · 点返回退出", 10, lv_color_hex(0x6f8794));
        lv_obj_set_width(hint, LV_HOR_RES - 40);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
        if (!video_player_.Play(media_entries_[index].path,
                                [this](const uint8_t* rgb565, int frame_width, int frame_height) {
                                    OnVideoFrame(rgb565, frame_width, frame_height);
                                })) {
            ShowMediaEmptyState("无法播放", "需要 MJPEG（MJPG）编码的 AVI 文件");
        }
    }

    void OnVideoFrame(const uint8_t* rgb565, int width, int height) {
        if (rgb565 == nullptr || width <= 0 || height <= 0)
            return;
        DisplayLockGuard lock(this);
        if (!lock || video_image_ == nullptr)
            return;
        video_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        video_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        video_dsc_.header.w = static_cast<uint32_t>(width);
        video_dsc_.header.h = static_cast<uint32_t>(height);
        video_dsc_.header.stride = static_cast<uint32_t>(width) * 2;
        video_dsc_.data = rgb565;
        video_dsc_.data_size = static_cast<size_t>(width) * height * 2;
        lv_image_set_src(video_image_, &video_dsc_);
        lv_obj_set_size(video_image_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        const uint32_t scale_x = static_cast<uint32_t>(388 * 256) / width;
        const uint32_t scale_y = static_cast<uint32_t>(424 * 256) / height;
        lv_image_set_scale(video_image_, std::min(scale_x, scale_y));
        lv_obj_align(video_image_, LV_ALIGN_CENTER, 0, 2);
    }

    static std::string MemoTimeText(const VoiceMemoEntry& entry) {
        // memo_YYYYMMDD_HHMMSS.wav -> "MM-DD HH:MM"
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (std::sscanf(entry.name.c_str(), "memo_%4d%2d%2d_%2d%2d%2d", &year, &month, &day,
                        &hour, &minute, &second) == 6) {
            char text[24];
            std::snprintf(text, sizeof(text), "%02d-%02d %02d:%02d", month, day, hour, minute);
            return text;
        }
        const auto dot = entry.name.find_last_of('.');
        return dot == std::string::npos ? entry.name : entry.name.substr(0, dot);
    }

    static std::string MemoDurationText(uint32_t duration_ms) {
        const uint32_t seconds = duration_ms / 1000;
        char text[16];
        std::snprintf(text, sizeof(text), "%02u:%02u", static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60));
        return text;
    }

    static void HandleVoiceMemoListEvent(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (self == nullptr)
            return;
        const auto encoded = reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event));
        if (encoded <= 0 || static_cast<size_t>(encoded - 1) >= self->voice_memo_paths_.size())
            return;
        const auto path = self->voice_memo_paths_[static_cast<size_t>(encoded - 1)];
        const bool same = WatchMusicPlayer::GetInstance().PlayingPath() == path;
        Application::GetInstance().Schedule([self, path, same] {
            if (same)
                WatchMusicPlayer::GetInstance().Stop();
            else
                WatchMusicPlayer::GetInstance().PlayFilePath(path);
            DisplayLockGuard lock(self);
            if (lock)
                self->RefreshVoiceMemoPage();
        });
    }

    void RebuildVoiceMemoList() {
        voice_memo_rows_.clear();
        voice_memo_paths_.clear();
        if (voice_memo_list_ == nullptr)
            return;
        lv_obj_clean(voice_memo_list_);
        auto entries = WatchVoiceMemoRecorder::ListRecordings();
        if (entries.empty()) {
            auto* empty = WatchLabel(voice_memo_list_, "还没有录音\n点上面的麦克风开始", 12,
                                     lv_color_hex(0x8296a3));
            lv_obj_set_width(empty, LV_HOR_RES - 2 * kScreenSafeInsetX - 12);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(empty);
            return;
        }
        const auto playing = WatchMusicPlayer::GetInstance().PlayingPath();
        voice_memo_rows_.reserve(entries.size());
        voice_memo_paths_.reserve(entries.size());
        for (const auto& entry : entries) {
            const bool current = !playing.empty() && playing == entry.path;
            auto* row = lv_button_create(voice_memo_list_);
            lv_obj_set_width(row, LV_HOR_RES - 2 * kScreenSafeInsetX);
            lv_obj_set_height(row, kVoiceMemoRowHeight);
            lv_obj_set_style_bg_color(
                row, current ? lv_color_hex(0x173f42) : lv_color_hex(0x0d1a24), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 11, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_add_event_cb(
                row, HandleVoiceMemoListEvent, LV_EVENT_CLICKED,
                reinterpret_cast<void*>(static_cast<std::intptr_t>(voice_memo_paths_.size() + 1)));
            auto* name = WatchLabel(row, MemoTimeText(entry).c_str(), 13,
                                    current ? lv_color_hex(0x65e6d6) : lv_color_hex(0xeaf5f7));
            lv_obj_set_width(name, LV_HOR_RES - 2 * kScreenSafeInsetX - 96);
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
            auto* duration = WatchLabel(row, MemoDurationText(entry.duration_ms).c_str(), 11,
                                        lv_color_hex(0x8296a3));
            lv_obj_align(duration, LV_ALIGN_RIGHT_MID, -12, 0);
            voice_memo_rows_.push_back(row);
            voice_memo_paths_.push_back(entry.path);
        }
        lv_obj_update_layout(voice_memo_list_);
    }

    void RefreshVoiceMemoListHighlight() {
        const auto playing = WatchMusicPlayer::GetInstance().PlayingPath();
        const size_t count = std::min(voice_memo_rows_.size(), voice_memo_paths_.size());
        for (size_t i = 0; i < count; ++i) {
            const bool current = !playing.empty() && playing == voice_memo_paths_[i];
            lv_obj_set_style_bg_color(
                voice_memo_rows_[i], current ? lv_color_hex(0x173f42) : lv_color_hex(0x0d1a24),
                0);
        }
    }

    void RefreshVoiceMemoPage() {
        if (voice_memo_status_ == nullptr || voice_memo_elapsed_ == nullptr ||
            voice_memo_toggle_ == nullptr)
            return;
        auto& recorder = WatchVoiceMemoRecorder::GetInstance();
        const bool recording = recorder.IsRecording();
        if (voice_memo_was_recording_ && !recording)
            RebuildVoiceMemoList();
        voice_memo_was_recording_ = recording;
        lv_label_set_text(voice_memo_status_,
                          recording ? "正在录音 · 再点一次结束" : "点麦克风开始录音");
        lv_obj_set_style_text_color(voice_memo_status_,
                                    recording ? lv_color_hex(0xff8090)
                                              : lv_color_hex(0xa9bac4),
                                    0);
        const uint32_t seconds = recorder.ElapsedSeconds();
        char elapsed[16];
        std::snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu:%02lu",
                      static_cast<unsigned long>(seconds / 3600),
                      static_cast<unsigned long>((seconds / 60) % 60),
                      static_cast<unsigned long>(seconds % 60));
        lv_label_set_text(voice_memo_elapsed_, elapsed);
        lv_obj_set_style_bg_color(voice_memo_toggle_,
                                  lv_color_hex(recording ? 0xff5e75 : 0x65e6d6), 0);
        lv_obj_t* icon = lv_obj_get_child(voice_memo_toggle_, 0);
        if (icon != nullptr) {
            lv_label_set_text(icon, recording ? MATERIAL_SYMBOLS_STOP : MATERIAL_SYMBOLS_MIC);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x071019), 0);
        }
        RefreshVoiceMemoListHighlight();
    }

    void OpenVoiceMemoPage() {
        OpenPage("语音备忘录", kBackHome);
        voice_memo_toggle_ = lv_button_create(watch_page_);
        lv_obj_set_size(voice_memo_toggle_, 84, 84);
        lv_obj_align(voice_memo_toggle_, LV_ALIGN_TOP_MID, 0, 66);
        lv_obj_set_style_radius(voice_memo_toggle_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(voice_memo_toggle_, lv_color_hex(0x65e6d6), 0);
        lv_obj_set_style_bg_opa(voice_memo_toggle_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(voice_memo_toggle_, 0, 0);
        lv_obj_set_style_shadow_width(voice_memo_toggle_, 0, 0);
        lv_obj_add_event_cb(voice_memo_toggle_, HandleWatchEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(kToggleVoiceMemo)));
        auto* icon = WatchIcon(voice_memo_toggle_, MATERIAL_SYMBOLS_MIC, 34,
                               lv_color_hex(0x071019));
        lv_obj_center(icon);

        voice_memo_elapsed_ = WatchLabel(watch_page_, "00:00:00", 26,
                                         lv_color_hex(0xf2f7fb));
        lv_obj_set_width(voice_memo_elapsed_, LV_HOR_RES - 80);
        lv_obj_align(voice_memo_elapsed_, LV_ALIGN_TOP_MID, 0, 156);
        lv_obj_set_style_text_align(voice_memo_elapsed_, LV_TEXT_ALIGN_CENTER, 0);

        voice_memo_status_ = WatchLabel(watch_page_, "点麦克风开始录音", 12,
                                        lv_color_hex(0xa9bac4));
        lv_obj_set_width(voice_memo_status_, LV_HOR_RES - 80);
        lv_obj_align(voice_memo_status_, LV_ALIGN_TOP_MID, 0, 190);
        lv_obj_set_style_text_align(voice_memo_status_, LV_TEXT_ALIGN_CENTER, 0);

        voice_memo_list_ = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(voice_memo_list_);
        lv_obj_set_pos(voice_memo_list_, kScreenSafeInsetX, 214);
        lv_obj_set_size(voice_memo_list_, LV_HOR_RES - 2 * kScreenSafeInsetX, 254);
        lv_obj_set_layout(voice_memo_list_, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(voice_memo_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(voice_memo_list_, 4, 0);
        lv_obj_set_style_pad_all(voice_memo_list_, 0, 0);
        lv_obj_set_scroll_dir(voice_memo_list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(voice_memo_list_, LV_SCROLLBAR_MODE_AUTO);

        voice_memo_was_recording_ = WatchVoiceMemoRecorder::GetInstance().IsRecording();
        RebuildVoiceMemoList();
        RefreshVoiceMemoPage();
        voice_memo_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
                if (self != nullptr)
                    self->RefreshVoiceMemoPage();
            }, 500, this);
    }

    void OpenActivityPage() {
        OpenPage("今日活动", kOpenApps);
        activity_progress_arc_ = lv_arc_create(watch_page_);
        lv_obj_set_pos(activity_progress_arc_, 79, 102);
        lv_obj_set_size(activity_progress_arc_, 252, 252);
        lv_arc_set_rotation(activity_progress_arc_, 135);
        lv_arc_set_bg_angles(activity_progress_arc_, 0, 270);
        lv_arc_set_range(activity_progress_arc_, 0, 100);
        lv_arc_set_value(activity_progress_arc_, 0);
        lv_obj_remove_style(activity_progress_arc_, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(activity_progress_arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(activity_progress_arc_, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_color(activity_progress_arc_, lv_color_hex(0x16252b),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_width(activity_progress_arc_, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(activity_progress_arc_, lv_color_hex(0x5ee6c7),
                                   LV_PART_INDICATOR);

        activity_steps_label_ = WatchLabel(watch_page_, "--", 52, lv_color_hex(0xf2f7fb));
        lv_obj_align(activity_steps_label_, LV_ALIGN_CENTER, 0, -30);
        auto* unit = WatchLabel(watch_page_, "今日步数", 13, lv_color_hex(0xa9bac4));
        lv_obj_align(unit, LV_ALIGN_CENTER, 0, 12);
        activity_goal_label_ = WatchLabel(watch_page_, "每日目标 8000 步", 12,
                                          lv_color_hex(0xe9c987));
        lv_obj_align(activity_goal_label_, LV_ALIGN_CENTER, 0, 52);
        activity_status_label_ = WatchLabel(watch_page_, "正在读取计步器", 11,
                                            lv_color_hex(0x8296a3));
        lv_obj_align(activity_status_label_, LV_ALIGN_CENTER, 0, 76);
        UpdateActivityLabels();
    }

    void OpenTimerPage() {
        OpenPage("倒计时", kOpenApps);
        auto* ring = lv_arc_create(watch_page_);
        lv_obj_set_pos(ring, 91, 94);
        lv_obj_set_size(ring, 228, 228);
        lv_arc_set_rotation(ring, 135);
        lv_arc_set_bg_angles(ring, 0, 270);
        lv_arc_set_range(ring, 0, 100);
        lv_arc_set_value(ring, 100);
        lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(ring, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring, lv_color_hex(0x18262c), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring, lv_color_hex(0x65e6d6), LV_PART_INDICATOR);

        timer_countdown_label_ = WatchLabel(watch_page_, "未启动", 32, lv_color_hex(0xf2f7fb));
        lv_obj_align(timer_countdown_label_, LV_ALIGN_CENTER, 0, -20);
        auto* note = WatchLabel(watch_page_, "快捷计时", 12,
                                lv_color_hex(0x8296a3));
        lv_obj_align(note, LV_ALIGN_CENTER, 0, 18);

        const int width = (LV_HOR_RES - 2 * kScreenSafeInsetX - 20) / 3;
        WatchButton(watch_page_, "1 分钟", kScreenSafeInsetX, 342, width, 48, kTimerStartMinute);
        WatchButton(watch_page_, "5 分钟", kScreenSafeInsetX + 10 + width, 342, width, 48,
                    kTimerStartFiveMinutes);
        WatchButton(watch_page_, "10 分钟", kScreenSafeInsetX + 20 + 2 * width, 342, width, 48,
                    kTimerStartTenMinutes);
        WatchButton(watch_page_, "取消计时", kScreenSafeInsetX, 402,
                    LV_HOR_RES - 2 * kScreenSafeInsetX, 48, kTimerCancel);
        UpdateTimerLabel();
    }

    void UpdateAlarmLabels() {
        if (alarm_service_ == nullptr)
            return;
        char time_text[6];
        const int hour = std::min<int>(23, alarm_service_->Hour());
        const int minute = std::min<int>(59, alarm_service_->Minute());
        std::snprintf(time_text, sizeof(time_text), "%02d:%02d", hour, minute);
        if (alarm_time_label_ != nullptr)
            lv_label_set_text(alarm_time_label_, time_text);
        if (alarm_status_label_ != nullptr)
            lv_label_set_text(alarm_status_label_,
                              alarm_service_->Enabled() ? "闹钟已开启" : "闹钟已关闭");
        if (alarm_toggle_label_ != nullptr)
            lv_label_set_text(alarm_toggle_label_,
                              alarm_service_->Enabled() ? "关闭闹钟" : "开启闹钟");
    }

    void OpenAlarmPage() {
        OpenPage("每日闹钟", kOpenApps);
        auto* card = lv_obj_create(watch_page_);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, 48, 91);
        lv_obj_set_size(card, LV_HOR_RES - 96, 144);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x101a20), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2b3b42), 0);
        lv_obj_set_style_radius(card, 24, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

        auto* bell = WatchIcon(card, MATERIAL_SYMBOLS_ALARM, 16, lv_color_hex(0xe9c987));
        lv_obj_align(bell, LV_ALIGN_TOP_LEFT, 16, 12);
        auto* title = WatchLabel(card, "每日重复", 11, lv_color_hex(0xa9bac4));
        lv_obj_set_pos(title, 42, 10);
        alarm_status_label_ = WatchLabel(card, "闹钟已关闭", 10, lv_color_hex(0x8296a3));
        lv_obj_align(alarm_status_label_, LV_ALIGN_TOP_RIGHT, -14, 12);
        alarm_time_label_ = WatchLabel(card, "07:00", 48, lv_color_hex(0xf2f7fb));
        lv_obj_set_width(alarm_time_label_, LV_HOR_RES - 112);
        lv_obj_set_style_text_align(alarm_time_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(alarm_time_label_, LV_ALIGN_CENTER, 0, 2);
        auto* repeat = WatchLabel(card, "勿扰模式下静音", 10, lv_color_hex(0x8296a3));
        lv_obj_align(repeat, LV_ALIGN_BOTTOM_MID, 0, -8);
        auto* preset_title = WatchLabel(watch_page_, "快捷时间", 11, lv_color_hex(0x81929c));
        lv_obj_set_pos(preset_title, kScreenSafeInsetX, 250);
        const int width = (LV_HOR_RES - 2 * kScreenSafeInsetX - 20) / 3;
        WatchButton(watch_page_, "07:00", kScreenSafeInsetX, 276, width, 48, kAlarmSetSeven);
        WatchButton(watch_page_, "07:30", kScreenSafeInsetX + 10 + width, 276, width, 48,
                    kAlarmSetSevenThirty);
        WatchButton(watch_page_, "08:00", kScreenSafeInsetX + 20 + 2 * width, 276, width, 48,
                    kAlarmSetEight);
        auto* toggle = WatchButton(watch_page_, "开启闹钟", kScreenSafeInsetX, 340,
                                   LV_HOR_RES - 2 * kScreenSafeInsetX, 50,
                                   kAlarmToggle);
        alarm_toggle_label_ = lv_obj_get_child(toggle, 0);
        auto* hint = WatchLabel(watch_page_, "闹钟响铃时可点击屏幕停止", 10,
                                lv_color_hex(0x8296a3));
        lv_obj_set_pos(hint, kScreenSafeInsetX, 402);
        lv_obj_set_width(hint, LV_HOR_RES - 2 * kScreenSafeInsetX);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        UpdateAlarmLabels();
    }

    void HandleWatchAction(WatchAction action, lv_event_t* event) {
        if (event == nullptr ||
            ((action == kBrightnessChanged || action == kVolumeChanged)
                 ? lv_event_get_code(event) == LV_EVENT_RELEASED
                 : lv_event_get_code(event) == LV_EVENT_CLICKED))
            WatchSystemLog::Event("ui", WatchActionName(action));
        if (static_cast<int>(action) >= kWifiApBase) {
            const int index = static_cast<int>(action) - kWifiApBase;
            if (index >= 0 && index < static_cast<int>(wifi_scan_results_.size())) {
                const auto ap = wifi_scan_results_[static_cast<size_t>(index)];
                WatchSystemLog::Event("wifi", "ap_selected", ap.ssid.c_str());
                if (ap.secured)
                    OpenWifiPasswordPage(ap.ssid);
                else
                    ConnectWifi(ap.ssid, "");
            }
            return;
        }
        if (static_cast<int>(action) >= kSsidActionBase) {
            const int index = static_cast<int>(action) - kSsidActionBase;
            auto& ssid_manager = SsidManager::GetInstance();
            if (index >= 0 && index < static_cast<int>(ssid_manager.GetSsidList().size())) {
                WatchSystemLog::Event("wifi", "saved_network_selected");
                SsidManager::GetInstance().SetDefaultSsid(index);
                WifiManager::GetInstance().StopStation();
                WifiManager::GetInstance().StartStation();
                OpenNetworkPage();
            }
            return;
        }
        switch (action) {
            case kNextFace:
                break;
            case kFaceLuxe:
                SelectWatchFace(0);
                break;
            case kFaceSport:
                SelectWatchFace(1);
                break;
            case kFaceClassic:
                SelectWatchFace(2);
                break;
            case kFaceCute:
                SelectWatchFace(3);
                break;
            case kStartAssistant:
                RequestVoiceAssistant();
                break;
            case kOpenMusic:
                OpenMusicPage();
                break;
            case kOpenVoiceMemo:
                OpenVoiceMemoPage();
                break;
            case kOpenPhotos:
                OpenPhotosPage();
                break;
            case kOpenVideos:
                OpenVideosPage();
                break;
            case kToggleVoiceMemo: {
                auto& app = Application::GetInstance();
                if (WatchVoiceMemoRecorder::GetInstance().IsRecording()) {
                    app.StopVoiceMemo();
                } else {
                    WatchMusicPlayer::GetInstance().Stop();
                    app.StartVoiceMemo();
                }
                break;
            }
            case kOpenApps:
                OpenAppsPage();
                break;
            case kOpenSettings:
                OpenSettingsPage();
                break;
            case kOpenActivity:
                OpenActivityPage();
                break;
            case kOpenTimer:
                OpenTimerPage();
                break;
            case kOpenAlarm:
                OpenAlarmPage();
                break;
            case kAlarmSetSeven:
            case kAlarmSetSevenThirty:
            case kAlarmSetEight:
                if (alarm_service_ != nullptr) {
                    const int minute = action == kAlarmSetSevenThirty ? 30 : 0;
                    const int hour = action == kAlarmSetEight ? 8 : 7;
                    alarm_service_->SetAlarm(hour, minute, true);
                    UpdateAlarmLabels();
                }
                break;
            case kAlarmToggle:
                if (alarm_service_ != nullptr) {
                    alarm_service_->SetEnabled(!alarm_service_->Enabled());
                    UpdateAlarmLabels();
                }
                break;
            case kBeginConfigPairing:
                if (config_pairing_label_ != nullptr) {
                    const auto pairing = config_pairing_handler_ ? config_pairing_handler_()
                                                                 : "配置服务尚未启动";
                    lv_label_set_text(config_pairing_label_, pairing.c_str());
                }
                break;
            case kTimerStartMinute:
            case kTimerStartFiveMinutes:
            case kTimerStartTenMinutes: {
                if (timer_service_ != nullptr) {
                    const uint32_t minutes = action == kTimerStartMinute        ? 1
                                             : action == kTimerStartFiveMinutes ? 5
                                                                                : 10;
                    timer_service_->Start(minutes * 60);
                    UpdateTimerLabel();
                }
                break;
            }
            case kTimerCancel:
                if (timer_service_ != nullptr)
                    timer_service_->Cancel();
                UpdateTimerLabel();
                break;
            case kRescanMusic: {
                auto& player = WatchMusicPlayer::GetInstance();
                player.Rescan();
                music_page_scanning_ = player.IsScanning();
                OpenMusicPage();
                break;
            }
            case kOpenNetwork:
                OpenNetworkPage();
                break;
            case kOpenWifiScan:
                OpenWifiScanPage(true);
                break;
            case kEnterWifiConfig:
                if (auto* wifi = static_cast<WifiBoard*>(&Board::GetInstance())) {
                    wifi->SetAutoConfigModeSuppressed(false);
                    EnsureWifiConfigMode();
                    auto ap = WifiManager::GetInstance().GetApSsid();
                    if (!ap.empty()) {
                        ShowNotification("手机连接 " + ap + "\n浏览器打开 192.168.4.1");
                    }
                }
                break;
            case kBackHome:
                if (watch_page_ != nullptr) {
                    lv_obj_delete(watch_page_);
                    watch_page_ = nullptr;
                }
                watch_music_status_ = nullptr;
                watch_music_progress_ = nullptr;
                watch_music_duration_ = nullptr;
                watch_music_progress_bar_ = nullptr;
                watch_music_shuffle_button_ = nullptr;
                watch_music_play_button_ = nullptr;
                ResetMusicList();
                music_page_scanning_ = false;
                notification_page_open_ = false;
                for (auto*& button : settings_face_buttons_)
                    button = nullptr;
                settings_face_current_ = nullptr;
                break;
            case kOpenMusicList:
                OpenMusicListPage();
                break;
            case kBackMusicPlayer:
                OpenMusicPage();
                break;
            case kMusicShuffle: {
                auto& player = WatchMusicPlayer::GetInstance();
                player.ToggleShuffle();
                if (watch_music_shuffle_button_ != nullptr) {
                    lv_obj_t* icon = lv_obj_get_child(watch_music_shuffle_button_, 0);
                    if (icon != nullptr)
                        lv_obj_set_style_text_color(
                            icon, lv_color_hex(player.ShuffleEnabled() ? 0x65e6d6 : 0x4f6470), 0);
                }
                break;
            }
            case kMusicPlayPause: {
                auto& player = WatchMusicPlayer::GetInstance();
                if (player.IsPlaying())
                    player.Pause();
                else
                    player.PlayOrResume();
                break;
            }
            case kMusicPlay:
                Application::GetInstance().Schedule(
                    [] { WatchMusicPlayer::GetInstance().PlayOrResume(); });
                break;
            case kMusicPause:
                WatchMusicPlayer::GetInstance().Pause();
                break;
            case kMusicNext:
                Application::GetInstance().Schedule([this] {
                    WatchMusicPlayer::GetInstance().Next();
                    DisplayLockGuard lock(this);
                    if (lock)
                        UpdateMusicProgress();
                });
                break;
            case kMusicPrevious:
                Application::GetInstance().Schedule([this] {
                    WatchMusicPlayer::GetInstance().Previous();
                    DisplayLockGuard lock(this);
                    if (lock)
                        UpdateMusicProgress();
                });
                break;
            case kMusicStop:
                WatchMusicPlayer::GetInstance().Stop();
                break;
            case kBrightnessChanged: {
                auto* backlight = Board::GetInstance().GetBacklight();
                auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
                if (settings_brightness_value_ != nullptr) {
                    char value[8];
                    std::snprintf(value, sizeof(value), "%d%%", static_cast<int>(lv_slider_get_value(slider)));
                    lv_label_set_text(settings_brightness_value_, value);
                }
                if (backlight != nullptr) {
                    backlight->SetBrightness(lv_slider_get_value(slider));
                    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
                        const int value = lv_slider_get_value(slider);
                        char detail[12];
                        std::snprintf(detail, sizeof(detail), "%d%%", value);
                        WatchSystemLog::Event("settings", "brightness", detail);
                        Application::GetInstance().Schedule([value] {
                            if (auto* backlight = Board::GetInstance().GetBacklight())
                                backlight->SetBrightness(value, true);
                        });
                    }
                }
                break;
            }
            case kVolumeChanged: {
                if (lv_event_get_code(event) != LV_EVENT_RELEASED)
                    break;
                auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
                const int value = lv_slider_get_value(slider);
                if (settings_volume_value_ != nullptr) {
                    char text[8];
                    std::snprintf(text, sizeof(text), "%d%%", value);
                    lv_label_set_text(settings_volume_value_, text);
                }
                char detail[12];
                std::snprintf(detail, sizeof(detail), "%d%%", value);
                WatchSystemLog::Event("settings", "volume", detail);
                Application::GetInstance().Schedule([value] {
                    if (auto* codec = Board::GetInstance().GetAudioCodec())
                        codec->SetOutputVolume(value);
                });
                break;
            }
            case kDndChanged: {
                const bool enabled =
                    lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target(event)),
                                     LV_STATE_CHECKED);
                WatchSystemLog::Event("settings", "do_not_disturb", enabled ? "on" : "off");
                Settings preferences("watchui", true);
                preferences.SetBool("dnd", enabled);
                if (timer_service_ != nullptr)
                    timer_service_->SetDoNotDisturb(enabled);
                break;
            }
            default:
                break;
        }
    }

    static void AnimateAssistantEyes(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr)
            return;
        // The eyes are invisible most of the time; skip the 8 Hz relayout then.
        if (self->assistant_layer_ == nullptr ||
            lv_obj_has_flag(self->assistant_layer_, LV_OBJ_FLAG_HIDDEN))
            return;
        static const int gaze_x[] = {30, 42, 36, 22, 32, 42, 30, 22};
        static const int gaze_y[] = {25, 30, 38, 30, 23, 34, 38, 28};
        const int frame = self->assistant_gaze_frame_++ % 8;
        for (auto* pupil : self->assistant_pupils_) {
            if (pupil == nullptr)
                continue;
            lv_obj_set_pos(pupil, gaze_x[frame], gaze_y[frame]);
            lv_obj_set_size(pupil, 48, frame == 6 ? 7 : 54);
        }
    }

    static void HideAssistantAfterIdle(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr)
            return;
        self->assistant_idle_timer_ = nullptr;
        if (self->assistant_layer_ == nullptr)
            return;
        DisplayLockGuard lock(self);
        if (!lock)
            return;
        lv_obj_add_flag(self->assistant_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(self->assistant_layer_, LV_OBJ_FLAG_CLICKABLE);
        if (self->watch_layer_ != nullptr)
            lv_obj_clear_flag(self->watch_layer_, LV_OBJ_FLAG_HIDDEN);
        WatchMusicPlayer::GetInstance().SetAssistantSessionActive(false);
        self->assistant_session_active_ = false;
        self->voice_overlay_allowed_ = false;
    }

    static void HandleAssistantSwipe(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self == nullptr)
            return;
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr)
            return;
        lv_point_t point = {};
        lv_indev_get_point(indev, &point);
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
            self->assistant_swipe_start_ = point;
            self->assistant_swipe_tracking_ = true;
        } else if (code == LV_EVENT_PRESS_LOST) {
            self->assistant_swipe_tracking_ = false;
        } else if (code == LV_EVENT_RELEASED && self->assistant_swipe_tracking_) {
            self->assistant_swipe_tracking_ = false;
            const int dx = point.x - self->assistant_swipe_start_.x;
            const int dy = point.y - self->assistant_swipe_start_.y;
            if (dy <= -30 && abs(dy) > abs(dx)) {
                auto& app = Application::GetInstance();
                app.StopListening();
                app.AbortSpeaking(kAbortReasonNone);
                app.SetDeviceState(kDeviceStateIdle);
            }
        }
    }

    static void HandleAssistantExit(lv_event_t* event) {
        (void)event;
        auto& app = Application::GetInstance();
        app.StopListening();
        app.AbortSpeaking(kAbortReasonNone);
        app.SetDeviceState(kDeviceStateIdle);
    }

    void CreateAssistantOverlay() {
        assistant_layer_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(assistant_layer_);
        lv_obj_set_size(assistant_layer_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(assistant_layer_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(assistant_layer_, true, 0);
        lv_obj_clear_flag(assistant_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(assistant_layer_, lv_color_hex(0x071019), 0);
        lv_obj_set_style_bg_opa(assistant_layer_, LV_OPA_COVER, 0);
        lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);

        auto* title = lv_label_create(assistant_layer_);
        lv_label_set_text(title, "大肥鱼语音助手");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_set_style_text_color(title, lv_color_hex(0xf2f7fb), 0);

        lv_obj_t* eye_pair = lv_obj_create(assistant_layer_);
        lv_obj_remove_style_all(eye_pair);
        lv_obj_set_size(eye_pair, 330, 180);
        lv_obj_clear_flag(eye_pair, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(eye_pair, LV_ALIGN_CENTER, 0, -38);
        lv_obj_set_style_bg_color(eye_pair, lv_color_hex(0x102a39), 0);
        lv_obj_set_style_bg_opa(eye_pair, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye_pair, 72, 0);

        for (int i = 0; i < 2; ++i) {
            lv_obj_t* eye = lv_obj_create(eye_pair);
            lv_obj_remove_style_all(eye);
            lv_obj_set_size(eye, 120, 128);
            lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(eye, 18 + i * 174, 14);
            lv_obj_set_style_bg_color(eye, lv_color_hex(0xe8fbff), 0);
            lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);

            assistant_pupils_[i] = lv_obj_create(eye);
            lv_obj_remove_style_all(assistant_pupils_[i]);
            lv_obj_set_size(assistant_pupils_[i], 44, 52);
            lv_obj_clear_flag(assistant_pupils_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(assistant_pupils_[i], 38, 31);
            lv_obj_set_style_bg_color(assistant_pupils_[i], lv_color_hex(0x102c44), 0);
            lv_obj_set_style_bg_opa(assistant_pupils_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(assistant_pupils_[i], LV_RADIUS_CIRCLE, 0);

            lv_obj_t* shine = lv_obj_create(assistant_pupils_[i]);
            lv_obj_remove_style_all(shine);
            lv_obj_set_size(shine, 13, 13);
            lv_obj_clear_flag(shine, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(shine, 8, 8);
            lv_obj_set_style_bg_color(shine, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(shine, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);

            auto* cheek = lv_obj_create(eye_pair);
            lv_obj_remove_style_all(cheek);
            lv_obj_set_size(cheek, 22, 10);
            lv_obj_clear_flag(cheek, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(cheek, i == 0 ? 28 : 280, 148);
            lv_obj_set_style_bg_color(cheek, lv_color_hex(0xff9eb2), 0);
            lv_obj_set_style_bg_opa(cheek, LV_OPA_70, 0);
            lv_obj_set_style_radius(cheek, LV_RADIUS_CIRCLE, 0);
        }

        assistant_status_ = lv_label_create(assistant_layer_);
        lv_label_set_text(assistant_status_, "正在聆听");
        lv_obj_align(assistant_status_, LV_ALIGN_CENTER, 0, 78);
        lv_obj_set_style_text_color(assistant_status_, lv_color_hex(0x65e6d6), 0);

        auto* assistant_swipe_layer = lv_obj_create(assistant_layer_);
        lv_obj_remove_style_all(assistant_swipe_layer);
        lv_obj_set_pos(assistant_swipe_layer, 0, 0);
        lv_obj_set_size(assistant_swipe_layer, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_opa(assistant_swipe_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(assistant_swipe_layer, 0, 0);
        lv_obj_add_flag(assistant_swipe_layer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(assistant_swipe_layer, HandleAssistantSwipe, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(assistant_swipe_layer, HandleAssistantSwipe, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(assistant_swipe_layer, HandleAssistantSwipe, LV_EVENT_PRESS_LOST, this);

        assistant_caption_viewport_ = lv_obj_create(assistant_layer_);
        lv_obj_remove_style_all(assistant_caption_viewport_);
        lv_obj_set_pos(assistant_caption_viewport_, 40, 354);
        lv_obj_set_size(assistant_caption_viewport_, LV_HOR_RES - 80, 92);
        lv_obj_set_scroll_dir(assistant_caption_viewport_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(assistant_caption_viewport_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(assistant_caption_viewport_, LV_OBJ_FLAG_CLICKABLE);
        assistant_caption_ = lv_label_create(assistant_caption_viewport_);
        lv_obj_set_width(assistant_caption_, LV_HOR_RES - 80);
        lv_label_set_long_mode(assistant_caption_, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(assistant_caption_, "你好，我在这里");
        lv_obj_set_pos(assistant_caption_, 0, 0);
        lv_obj_set_style_text_align(assistant_caption_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(assistant_caption_, lv_color_hex(0xc6d5df), 0);

        auto theme = GetTheme();
        auto font = theme != nullptr ? theme->GetTextFont() : nullptr;
        if (font != nullptr && font->font() != nullptr) {
            lv_obj_set_style_text_font(title, font->font(), 0);
            lv_obj_set_style_text_font(assistant_status_, font->font(), 0);
            lv_obj_set_style_text_font(assistant_caption_, font->font(), 0);
        }
        assistant_eye_timer_ = lv_timer_create(AnimateAssistantEyes, 650, this);
    }

public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        if (!lock)
            return;
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        Settings preferences("watchui");
        const int saved_face = preferences.GetInt("face", 3);
        watch_face_ = saved_face >= 0 && saved_face < kWatchFaceCount ? saved_face : 3;
        CreateWatchShell();
        CreateAssistantOverlay();
        // The stock chat UI is fully covered by the watch interface. Skip rendering it on
        // every invalidated area so LVGL spends its time drawing only the visible watch.
        lv_obj_add_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN);
    }

    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) override {
        rebinding_text_font_ = true;
        const bool updated = LcdDisplay::SetTextFont(std::move(text_font));
        rebinding_text_font_ = false;
        return updated;
    }

    virtual void SetTheme(Theme* theme) override {
        if (!rebinding_text_font_ && watch_layer_ != nullptr && theme == GetTheme()) {
            // Asset loading already rebound this same theme with the new font. Avoid a second
            // LVGL style transition over the rebuilt watch tree during boot.
            Display::SetTheme(theme);
            return;
        }
        LcdDisplay::SetTheme(theme);
    }

    // Used by the LAN debug endpoint to verify the media pages without touching
    // the screen. Not exposed to voice/MCP.
    void DebugOpenMedia(bool video, int index) {
        if (video) {
            OpenVideosPage();
            if (index >= 0)
                OpenVideoPlayer(static_cast<size_t>(index));
        } else {
            OpenPhotosPage();
            if (index >= 0)
                OpenPhotoViewer(static_cast<size_t>(index));
        }
    }

    bool SnapshotWatchToJpeg(std::string& jpeg_data, int quality = 80) {
        // The watch interface lives on lv_layer_top(), not lv_screen_active().
        return SnapshotObjectToJpeg(lv_layer_top(), jpeg_data, quality);
    }

    bool SetWatchFace(const std::string& name) {
        uint8_t selected;
        if (name == "鎏金雅韵" || name == "鎏金" || name == "金色雅韵" || name == "金色")
            selected = 0;
        else if (name == "活力运动" || name == "运动")
            selected = 1;
        else if (name == "经典指针" || name == "经典")
            selected = 2;
        else if (name == "软糖" || name == "可爱" || name == "Mochi" || name == "cute")
            selected = 3;
        else
            return false;
        {
            DisplayLockGuard lock(this);
            if (!lock || watch_layer_ == nullptr)
                return false;
            watch_face_ = selected;
            UpdateWatchFace();
        }
        Settings preferences("watchui", true);
        preferences.SetInt("face", selected);
        WatchSystemLog::Event("ui", "face_selected", name.c_str());
        if (settings_face_buttons_[0] != nullptr)
            UpdateSettingsFaceSelection();
        return true;
    }

    bool HasWatchPage() const { return watch_page_ != nullptr; }
    bool IsAodActive() const { return aod_mode_; }
    void AllowVoiceOverlay() { voice_overlay_allowed_ = true; }
    void RequestVoiceAssistant() {
        auto& app = Application::GetInstance();
        voice_overlay_allowed_ = true;
        // Without a network the audio channel cannot open at all; explain it
        // instead of dropping into a silent listening/audio-testing state.
        if (!WifiManager::GetInstance().IsConnected()) {
            ShowAssistantOfflineHint();
            return;
        }
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateConnecting) {
            // The channel can take a few seconds (MQTT + handshake); tell the
            // user instead of swallowing repeated taps silently.
            ShowAssistantStatusHint("正在连接…", lv_color_hex(0x60d6c4), 4000);
            return;
        }
        if (state == kDeviceStateListening)
            return;  // a session is already running
        if (state == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
            return;
        }
        if (state == kDeviceStateIdle) {
            // ToggleChatState() opens the channel with the default listening
            // mode (realtime/auto-stop). StartListening() is the push-to-talk
            // path (manual stop), which never gets an answer until STOP is sent.
            app.ToggleChatState();
            return;
        }
        app.StartListening();
    }

    void ShowAssistantStatusHint(const char* text, lv_color_t color, uint32_t timeout_ms) {
        DisplayLockGuard lock(this);
        if (!lock || assistant_layer_ == nullptr)
            return;
        if (assistant_idle_timer_ != nullptr) {
            lv_timer_delete(assistant_idle_timer_);
            assistant_idle_timer_ = nullptr;
        }
        if (assistant_status_ != nullptr) {
            lv_label_set_text(assistant_status_, text);
            lv_obj_set_style_text_color(assistant_status_, color, 0);
        }
        if (watch_layer_ != nullptr)
            lv_obj_add_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
        assistant_idle_timer_ = lv_timer_create(HideAssistantAfterIdle, timeout_ms, this);
        if (assistant_idle_timer_ != nullptr)
            lv_timer_set_repeat_count(assistant_idle_timer_, 1);
    }

    void ShowAssistantOfflineHint() {
        DisplayLockGuard lock(this);
        if (!lock || assistant_layer_ == nullptr)
            return;
        if (assistant_idle_timer_ != nullptr) {
            lv_timer_delete(assistant_idle_timer_);
            assistant_idle_timer_ = nullptr;
        }
        lv_label_set_text(assistant_status_, "尚未联网");
        lv_obj_set_style_text_color(assistant_status_, lv_color_hex(0xe2a33a), 0);
        if (assistant_caption_ != nullptr) {
            lv_label_set_text(assistant_caption_,
                              "手机连接热点 Xiaozhi-83CD，浏览器打开 http://192.168.4.1 配网");
        }
        if (watch_layer_ != nullptr)
            lv_obj_add_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_CLICKABLE);
        assistant_idle_timer_ = lv_timer_create(HideAssistantAfterIdle, 6000, this);
        if (assistant_idle_timer_ != nullptr)
            lv_timer_set_repeat_count(assistant_idle_timer_, 1);
        WatchSystemLog::Event("voice", "offline_hint");
    }

    bool IsAssistantOverlayVisible() const {
        return assistant_layer_ != nullptr &&
               !lv_obj_has_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    bool OpenWatchApp(const std::string& name) {
        WatchSystemLog::Event("app", "open_requested", name.c_str());
        DisplayLockGuard lock(this);
        if (!lock || watch_layer_ == nullptr)
            return false;
        if (name == "小智" || name == "助手" || name == "大肥鱼" || name == "大肥鱼助手")
            RequestVoiceAssistant();
        else if (name == "语音备忘录" || name == "录音")
            OpenVoiceMemoPage();
        else if (name == "音乐")
            OpenMusicPage();
        else if (name == "歌曲" || name == "列表" || name == "曲库")
            OpenMusicListPage();
        else if (name == "播放音乐") {
            Application::GetInstance().Schedule(
                [] { WatchMusicPlayer::GetInstance().PlayOrResume(); });
        } else if (name == "活动")
            OpenActivityPage();
        else if (name == "计时器" || name == "倒计时")
            OpenTimerPage();
        else if (name == "闹钟" || name == "每日闹钟")
            OpenAlarmPage();
        else if (name == "应用")
            OpenAppsPage();
        else if (name == "设置")
            OpenSettingsPage();
        else if (name == "AOD") {
            Application::GetInstance().Schedule([this] { SetPowerSaveMode(true); });
        } else if (name == "唤醒") {
            Application::GetInstance().Schedule([this] { SetPowerSaveMode(false); });
        } else if (name == "通知") {
            OpenNotificationsPage();
        } else if (name == "测试通知") {
            Application::GetInstance().Schedule(
                [this] { ShowNotification("大肥鱼提醒：该休息一下了", 5000); });
        } else if (name == "表盘" || name == "主页" || name == "首页") {
            // Same teardown as OpenPage(): the page timers must die with the
            // widgets they touch.
            ReleaseWatchPageHandles();
            if (watch_page_ != nullptr) {
                lv_obj_delete(watch_page_);
                watch_page_ = nullptr;
            }
            UpdateWatchBattery();
        } else
            return false;
        return true;
    }

    void SetAodWakeHandler(std::function<void()> handler) {
        aod_wake_handler_ = std::move(handler);
    }

    void SetActivityService(WatchActivityService* service) { activity_service_ = service; }

    void SetTimerService(WatchTimerService* service) { timer_service_ = service; }

    void SetAlarmService(WatchAlarmService* service) { alarm_service_ = service; }

    void SetConfigPairingHandler(std::function<std::string()> handler) {
        config_pairing_handler_ = std::move(handler);
    }

    static void HandleAodWake(lv_event_t* event) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self != nullptr && self->aod_wake_handler_)
            self->aod_wake_handler_();
    }

    void EnsureAodLayer() {
        if (aod_layer_ != nullptr)
            return;
        aod_layer_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(aod_layer_);
        lv_obj_set_size(aod_layer_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(aod_layer_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(aod_layer_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(aod_layer_, kScreenCornerRadius, 0);
        lv_obj_set_style_clip_corner(aod_layer_, true, 0);
        lv_obj_add_flag(aod_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(aod_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(aod_layer_, HandleAodWake, LV_EVENT_CLICKED, this);
        aod_clock_ = WatchLabel(aod_layer_, "12:00", 52, lv_color_hex(0xd8e7ec));
        lv_obj_align(aod_clock_, LV_ALIGN_CENTER, 0, -24);
        aod_date_ = WatchLabel(aod_layer_, "11月14日 周六", 14, lv_color_hex(0x8296a3));
        lv_obj_align(aod_date_, LV_ALIGN_CENTER, 0, 42);
        aod_battery_ = WatchLabel(aod_layer_, "95%", 12, lv_color_hex(0x4f6470));
        lv_obj_align(aod_battery_, LV_ALIGN_CENTER, 0, 76);
    }

    void UpdateAodLabels(const std::tm& local) {
        if (aod_clock_ == nullptr)
            return;
        char time_text[8];
        char date_text[24];
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
        static constexpr const char* weekdays[] = {"周日", "周一", "周二", "周三",
                                                   "周四", "周五", "周六"};
        std::snprintf(date_text, sizeof(date_text), "%d月%d日  %s", local.tm_mon + 1,
                      local.tm_mday, weekdays[local.tm_wday]);
        lv_label_set_text(aod_clock_, time_text);
        lv_label_set_text(aod_date_, date_text);
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            char battery[16];
            std::snprintf(battery, sizeof(battery), "%d%%", level);
            lv_label_set_text(aod_battery_, battery);
        }
        const int shift_x = (local.tm_min % 5) - 2;
        const int shift_y = (local.tm_min % 3) - 1;
        lv_obj_align(aod_clock_, LV_ALIGN_CENTER, shift_x, -24 + shift_y);
    }

    void EnsureWatchToast() {
        if (watch_toast_ != nullptr)
            return;
        watch_toast_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(watch_toast_);
        lv_obj_set_pos(watch_toast_, kScreenSafeInsetX, 82);
        lv_obj_set_size(watch_toast_, LV_HOR_RES - 2 * kScreenSafeInsetX, 62);
        lv_obj_set_style_bg_color(watch_toast_, lv_color_hex(0x18303d), 0);
        lv_obj_set_style_bg_opa(watch_toast_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(watch_toast_, 1, 0);
        lv_obj_set_style_border_color(watch_toast_, lv_color_hex(0x2b5560), 0);
        lv_obj_set_style_radius(watch_toast_, 18, 0);
        lv_obj_set_style_shadow_width(watch_toast_, 0, 0);
        lv_obj_add_flag(watch_toast_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(watch_toast_, LV_OBJ_FLAG_CLICKABLE);
        auto* icon = WatchIcon(watch_toast_, MATERIAL_SYMBOLS_NOTIFICATIONS, 18,
                               lv_color_hex(0x65e6d6));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
        watch_toast_label_ = WatchLabel(watch_toast_, "", 14, lv_color_hex(0xeaf5f7));
        lv_obj_set_pos(watch_toast_label_, 44, 8);
        lv_obj_set_size(watch_toast_label_, LV_HOR_RES - 2 * kScreenSafeInsetX - 56, 46);
        lv_label_set_long_mode(watch_toast_label_, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(watch_toast_label_, LV_TEXT_ALIGN_LEFT, 0);
    }

    static void HideWatchToast(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr)
            return;
        self->watch_toast_timer_ = nullptr;
        if (self->watch_toast_ != nullptr)
            lv_obj_add_flag(self->watch_toast_, LV_OBJ_FLAG_HIDDEN);
    }

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        if (!setup_ui_called_ || notification == nullptr || *notification == '\0') {
            Display::ShowNotification(notification, duration_ms);
            return;
        }
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        WatchNotification item{notification, std::time(nullptr)};
        watch_notifications_.push_back(item);
        if (watch_notifications_.size() > 8)
            watch_notifications_.erase(watch_notifications_.begin());
        if (notification_page_open_) {
            OpenNotificationsPage();
            return;
        }
        EnsureWatchToast();
        if (watch_toast_label_ == nullptr)
            return;
        lv_label_set_text(watch_toast_label_, notification);
        lv_obj_clear_flag(watch_toast_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(watch_toast_);
        if (watch_toast_timer_ != nullptr)
            lv_timer_delete(watch_toast_timer_);
        watch_toast_timer_ = lv_timer_create(HideWatchToast, duration_ms > 0 ? duration_ms : 3000,
                                             this);
        if (watch_toast_timer_ != nullptr)
            lv_timer_set_repeat_count(watch_toast_timer_, 1);
    }

    virtual void ShowNotification(const std::string& notification,
                                  int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }

    virtual void SetPowerSaveMode(bool on) override {
        if (!setup_ui_called_ || watch_layer_ == nullptr)
            return;
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        if (on) {
            EnsureAodLayer();
            if (aod_layer_ == nullptr)
                return;
            aod_mode_ = true;
            std::tm local = {};
            const auto now = std::time(nullptr);
            if (localtime_r(&now, &local) != nullptr)
                UpdateAodLabels(local);
            lv_obj_clear_flag(aod_layer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(aod_layer_);
            lv_obj_add_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
            if (assistant_layer_ != nullptr)
                lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
        } else {
            aod_mode_ = false;
            if (aod_layer_ != nullptr)
                lv_obj_add_flag(aod_layer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    virtual void SetStatus(const char* status) override {
        SpiLcdDisplay::SetStatus(status);
        if (assistant_layer_ == nullptr || status == nullptr)
            return;

        const bool listening = std::strcmp(status, Lang::Strings::LISTENING) == 0;
        const bool speaking = std::strcmp(status, Lang::Strings::SPEAKING) == 0;
        const bool connecting = std::strcmp(status, Lang::Strings::CONNECTING) == 0;
        const bool idle = std::strcmp(status, Lang::Strings::STANDBY) == 0;
        if (!listening && !speaking && !connecting && !idle)
            return;
        if (listening || speaking || connecting)
            WatchSystemLog::Event("voice", listening ? "listening"
                                                  : speaking ? "speaking" : "connecting");

        // A wake word during music can be a false positive from the song or
        // speaker. Pause quietly first; only show/stop for a real answer.
        if ((listening || connecting) && WatchMusicPlayer::GetInstance().IsPlaying() &&
            !voice_overlay_allowed_) {
            WatchMusicPlayer::GetInstance().Pause();
            music_wake_hold_ = true;
            if (assistant_layer_ != nullptr)
                lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
            if (watch_layer_ != nullptr)
                lv_obj_clear_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (speaking && music_wake_hold_) {
            // Keep the song paused instead of stopping it. Most wake words
            // during playback are false positives caused by the music itself;
            // stopping here made the song die after one assistant reply.
            // HandleStateChangedEvent resumes playback when the assistant
            // returns to idle, and an explicit "停止播放" still stops it via MCP.
            music_wake_hold_ = true;
        }
        if (speaking && WatchMusicPlayer::GetInstance().IsPlaying() &&
            !voice_overlay_allowed_) {
            // Only late TTS/answer events are suppressed. A genuine wake word
            // produces listening/connecting and must still open 大肥鱼 while
            // music is playing.
            DisplayLockGuard lock(this);
            if (!lock)
                return;
            if (assistant_layer_ != nullptr)
                lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
            if (watch_layer_ != nullptr)
                lv_obj_clear_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        DisplayLockGuard lock(this);
        if (!lock)
            return;
        if (idle) {
            if (music_wake_hold_) {
                // False wake word: quietly resume the song, no overlay flash.
                music_wake_hold_ = false;
                WatchMusicPlayer::GetInstance().PlayOrResume();
            }
            // VAD frequently emits a short standby between listening and
            // speaking. Do not tear the overlay down on that transient state.
            if (assistant_idle_timer_ == nullptr) {
                assistant_idle_timer_ = lv_timer_create(HideAssistantAfterIdle, 450, this);
                if (assistant_idle_timer_ != nullptr)
                    lv_timer_set_repeat_count(assistant_idle_timer_, 1);
            } else {
                lv_timer_reset(assistant_idle_timer_);
            }
            return;
        }
        if (assistant_idle_timer_ != nullptr) {
            lv_timer_delete(assistant_idle_timer_);
            assistant_idle_timer_ = nullptr;
        }
        if (!assistant_session_active_) {
            assistant_session_active_ = true;
            WatchMusicPlayer::GetInstance().SetAssistantSessionActive(true);
        }
        lv_obj_add_flag(watch_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(assistant_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(assistant_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(assistant_status_, lv_color_hex(0x65e6d6), 0);
        lv_label_set_text(assistant_status_, listening  ? "正在聆听"
                                             : speaking ? "大肥鱼正在回答"
                                                        : "正在连接大肥鱼");
        if (connecting)
            lv_label_set_text(assistant_caption_, "请稍候，我正在连接语音服务");
        else if (listening)
            lv_label_set_text(assistant_caption_, "可以直接说话，也可以随时打断");
        if (assistant_eye_timer_ != nullptr) {
            lv_timer_set_period(assistant_eye_timer_, speaking ? 320 : 650);
        }
    }

    virtual void OnTextFontChanged() override {
        if (!setup_ui_called_ || watch_layer_ == nullptr)
            return;
        LoadWatchFonts();
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        if (watch_clock_timer_ != nullptr) {
            lv_timer_delete(watch_clock_timer_);
            watch_clock_timer_ = nullptr;
        }
        if (watch_toast_timer_ != nullptr) {
            lv_timer_delete(watch_toast_timer_);
            watch_toast_timer_ = nullptr;
        }
        if (watch_toast_ != nullptr) {
            lv_obj_delete(watch_toast_);
            watch_toast_ = nullptr;
        }
        if (aod_layer_ != nullptr) {
            lv_obj_delete(aod_layer_);
            aod_layer_ = nullptr;
        }
        aod_mode_ = false;
        watch_toast_label_ = nullptr;
        aod_clock_ = nullptr;
        aod_date_ = nullptr;
        aod_battery_ = nullptr;
        if (assistant_eye_timer_ != nullptr) {
            lv_timer_delete(assistant_eye_timer_);
            assistant_eye_timer_ = nullptr;
        }
        if (assistant_layer_ != nullptr) {
            lv_obj_delete(assistant_layer_);
            assistant_layer_ = nullptr;
        }
        if (watch_layer_ != nullptr) {
            lv_obj_delete(watch_layer_);
            watch_layer_ = nullptr;
        }
        watch_page_ = nullptr;
        watch_clock_ = nullptr;
        watch_date_ = nullptr;
        watch_battery_ = nullptr;
        watch_brand_ = nullptr;
        watch_face_card_ = nullptr;
        watch_face_name_ = nullptr;
        watch_face_hint_ = nullptr;
        classic_time_ = nullptr;
        classic_date_ = nullptr;
        classic_meta_ = nullptr;
        cute_steps_ = nullptr;
        cute_battery_ = nullptr;
        watch_face_steps_ = nullptr;
        watch_face_goal_ = nullptr;
        watch_face_progress_bar_ = nullptr;
        watch_face_progress_arc_ = nullptr;
        watch_flux_arc2_ = nullptr;
        watch_flux_arc3_ = nullptr;
        watch_face_day_ = nullptr;
        watch_face_pivot_ = nullptr;
        watch_analog_dial_ = nullptr;
        watch_analog_hour_hand_ = nullptr;
        watch_analog_minute_hand_ = nullptr;
        watch_second_hand_ = nullptr;
        watch_subdial_ = nullptr;
        watch_subdial_hand_ = nullptr;
        for (auto& line : watch_tick_lines_) line = nullptr;
        watch_music_status_ = nullptr;
        watch_music_progress_ = nullptr;
        watch_music_duration_ = nullptr;
        watch_music_progress_bar_ = nullptr;
        ResetMusicList();
        activity_steps_label_ = nullptr;
        activity_goal_label_ = nullptr;
        activity_progress_arc_ = nullptr;
        activity_status_label_ = nullptr;
        timer_countdown_label_ = nullptr;
        alarm_time_label_ = nullptr;
        alarm_status_label_ = nullptr;
        alarm_toggle_label_ = nullptr;
        config_pairing_label_ = nullptr;
        settings_brightness_value_ = nullptr;
        settings_volume_value_ = nullptr;
        assistant_pupils_[0] = nullptr;
        assistant_pupils_[1] = nullptr;
        assistant_status_ = nullptr;
        assistant_caption_ = nullptr;
        assistant_session_active_ = false;

        CreateWatchShell();
        CreateAssistantOverlay();
        SetStatus(Lang::Strings::STANDBY);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        SpiLcdDisplay::SetChatMessage(role, content);
        if (assistant_layer_ == nullptr || content == nullptr || role == nullptr ||
            (std::strcmp(role, "assistant") != 0 && std::strcmp(role, "user") != 0))
            return;

        DisplayLockGuard lock(this);
        if (!lock)
            return;
        lv_label_set_text(assistant_caption_, content);
        if (assistant_caption_viewport_ != nullptr)
            lv_obj_scroll_to_y(assistant_caption_viewport_, 0, LV_ANIM_OFF);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch06 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    WatchActivityService* activity_service_ = nullptr;
    WatchTimerService timer_service_;
    WatchAlarmService alarm_service_;
    WatchRtc rtc_;
    esp_timer_handle_t rtc_sync_timer_ = nullptr;
    esp_timer_handle_t pwr_poll_timer_ = nullptr;
    WatchConfigService config_service_;
    esp_timer_handle_t config_start_timer_ = nullptr;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    PowerSaveTimer* power_save_timer_;

    WatchConfigService::Config ReadWatchConfig() {
        Settings preferences("watchui");
        Settings audio_settings("audio");
        const int face = preferences.GetInt("face", 0);
        static const char* faces[] = {"luxe", "sport", "classic", "cute"};
        return {
            .face = faces[face >= 0 && face < 4 ? face : 3],
            .brightness = backlight_ != nullptr ? backlight_->brightness() : 70,
            .volume = audio_settings.GetInt("output_volume", 55),
            .do_not_disturb = preferences.GetBool("dnd", false),
        };
    }

    bool ApplyWatchConfig(const WatchConfigService::Config& config) {
        auto* codec = GetAudioCodec();
        if (display_ == nullptr || backlight_ == nullptr || codec == nullptr)
            return false;
        static const char* face_names[] = {"金色雅韵", "活力运动", "经典指针", "软糖"};
        const char* face_name = config.face == "luxe"    ? face_names[0]
                                : config.face == "sport"  ? face_names[1]
                                : config.face == "cute"   ? face_names[3]
                                                          : face_names[2];
        if (!display_->SetWatchFace(face_name))
            return false;
        backlight_->SetBrightness(static_cast<uint8_t>(config.brightness), true);
        codec->SetOutputVolume(config.volume);
        Settings preferences("watchui", true);
        preferences.SetBool("dnd", config.do_not_disturb);
        timer_service_.SetDoNotDisturb(config.do_not_disturb);
        return true;
    }

    // One place to switch the always-on black clock on/off, used by the power
    // key, the sleep timer and the AOD tap.
    void SetWatchAod(bool on) {
        if (display_ == nullptr)
            return;
        display_->SetPowerSaveMode(on);
        if (backlight_ == nullptr)
            return;
        if (on)
            backlight_->SetBrightness(20);
        else
            backlight_->RestoreBrightness();
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() { SetWatchAod(true); });
        power_save_timer_->OnExitSleepMode([this]() { SetWatchAod(false); });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
        pmic_->EnablePowerKeyShortPressIrq();
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        // spi_bus_config_t defaults unused pins to 0, which is the BOOT
        // button on this board. The QSPI bus owns only SCLK + 4 data lines.
        buscfg.mosi_io_num = GPIO_NUM_NC;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        // The display QSPI bus is initialized before this point and its
        // gpio-claim bookkeeping can still mark GPIO0 as reserved. Release
        // that stale reservation, then create the real BOOT button.
        esp_gpio_revoke(1ULL << BOOT_BUTTON_GPIO);
        boot_button_.~Button();
        new (&boot_button_) Button(BOOT_BUTTON_GPIO);
        boot_button_.OnClick([this]() {
            WatchSystemLog::Event("input", "power_button_short_press");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (power_save_timer_ != nullptr)
                power_save_timer_->WakeUp();
            if (display_ == nullptr)
                return;
            auto* watch = static_cast<CustomLcdDisplay*>(display_);
            // Apple-Watch-like crown: from the face open the app grid; from a
            // page return home. Long press starts the assistant.
            if (watch->HasWatchPage())
                watch->OpenWatchApp("表盘");
            else
                watch->OpenWatchApp("应用");
        });

        boot_button_.OnLongPress([this]() {
            WatchSystemLog::Event("input", "power_button_long_press");
            if (power_save_timer_ != nullptr)
                power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            const auto state = app.GetDeviceState();
            auto* watch = display_ != nullptr ? static_cast<CustomLcdDisplay*>(display_) : nullptr;
            const bool visible = watch != nullptr && watch->IsAssistantOverlayVisible();
            ESP_LOGI(TAG, "BOOT long state=%d overlay=%d", static_cast<int>(state), visible);
            if (visible && (state == kDeviceStateListening ||
                            state == kDeviceStateSpeaking ||
                            state == kDeviceStateConnecting)) {
                app.StopListening();
                app.AbortSpeaking(kAbortReasonNone);
            } else {
                // Let HandleStartListeningEvent deal with stale states.
                if (watch != nullptr)
                    watch->AllowVoiceOverlay();
                app.StartListening();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        // PWR is wired to AXP2101 PWRON. Short press is exposed through the
        // PMIC IRQ status register; long press remains the hardware power-off.
        esp_timer_create_args_t pwr_args = {
            .callback =
                [](void* arg) {
                    auto* self =
                        static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (self->pmic_ == nullptr || !self->pmic_->ConsumePowerKeyShortPress())
                        return;
                    const bool was_aod = self->display_ != nullptr &&
                                         static_cast<CustomLcdDisplay*>(self->display_)
                                             ->IsAodActive();
                    if (was_aod) {
                        if (self->power_save_timer_ != nullptr)
                            self->power_save_timer_->WakeUp();
                        self->SetWatchAod(false);
                        return;
                    }
                    // Physical media key: while music is playing, PWR stops it
                    // immediately (generation bump flushes queued PCM).
                    if (WatchMusicPlayer::GetInstance().IsPlaying()) {
                        WatchMusicPlayer::GetInstance().Stop();
                        return;
                    }
                    // Otherwise short press drops to the black always-on clock
                    // (time + date only); press again or tap to wake.
                    if (self->power_save_timer_ != nullptr)
                        self->power_save_timer_->WakeUp();
                    self->SetWatchAod(true);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "watch_pwr",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&pwr_args, &pwr_poll_timer_) == ESP_OK)
            esp_timer_start_periodic(pwr_poll_timer_, 200 * 1000);
    }

    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x16, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
        display_->SetAodWakeHandler([this]() {
            Application::GetInstance().Schedule([this]() {
                if (power_save_timer_ != nullptr)
                    power_save_timer_->WakeUp();
                SetWatchAod(false);
            });
        });
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_9,
            .int_gpio_num = GPIO_NUM_38,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        auto& music = WatchMusicPlayer::GetInstance();
        Property music_query("query", kPropertyTypeString, std::string());
        music_query.SetMaxLength(64);
        Property music_track_id("track_id", kPropertyTypeString, std::string());
        music_track_id.SetMaxLength(8);
        AddLoggedWatchTool(mcp_server, "self.watch.music.search",
                           "在手表 SD 卡中搜索可播放的音乐。当前支持 MP3 和 WAV 文件，文件放在 SD "
                           "卡根目录或 Music 文件夹中。",
                           PropertyList({music_query}),
                           [&music](const PropertyList& properties) -> ReturnValue {
                               return music.Search(properties["query"].value<std::string>());
                           });
        AddLoggedWatchTool(mcp_server, "self.watch.music.play",
                           "播放 SD 卡中的音乐。优先传入搜索结果的曲目编号 track_id；也可传入歌名 "
                           "query，若匹配多首会返回编号供用户确认。",
                           PropertyList({music_query, music_track_id}),
                           [&music](const PropertyList& properties) -> ReturnValue {
                               const auto track_id = properties["track_id"].value<std::string>();
                               if (!track_id.empty())
                                   return music.PlayTrack(track_id);
                               return music.Play(properties["query"].value<std::string>());
                           });
        AddLoggedWatchTool(mcp_server, "self.watch.music.pause", "暂停 SD 卡音乐。", PropertyList(),
                           [&music](const PropertyList&) -> ReturnValue { return music.Pause(); });
        AddLoggedWatchTool(mcp_server, "self.watch.music.resume", "继续播放已暂停的 SD 卡音乐。",
                           PropertyList(),
                           [&music](const PropertyList&) -> ReturnValue { return music.Resume(); });
        AddLoggedWatchTool(mcp_server, "self.watch.music.next", "播放 SD 卡中的下一首音乐。", PropertyList(),
                           [&music](const PropertyList&) -> ReturnValue { return music.Next(); });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.music.previous", "播放 SD 卡中的上一首音乐。", PropertyList(),
            [&music](const PropertyList&) -> ReturnValue { return music.Previous(); });
        AddLoggedWatchTool(mcp_server, "self.watch.music.stop", "停止 SD 卡音乐播放。", PropertyList(),
                           [&music](const PropertyList&) -> ReturnValue { return music.Stop(); });
        AddLoggedWatchTool(mcp_server, "self.watch.music.status", "查询 SD 卡音乐播放器当前状态。",
                           PropertyList(),
                           [&music](const PropertyList&) -> ReturnValue { return music.Status(); });
        Property step_goal("goal", kPropertyTypeInteger, 8000, 1000, 50000);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.activity.get_steps", "查询手表今日步数和每日步数目标。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (activity_service_ == nullptr || !activity_service_->Available())
                    return "计步传感器暂不可用，请检查手表 IMU。";
                return "今天已走 " + std::to_string(activity_service_->TodaySteps()) +
                       " 步，每日目标 " + std::to_string(activity_service_->Goal()) + " 步。";
            });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.activity.set_goal", "设置手表每日步数目标，范围 1000 到 50000 步。",
            PropertyList({step_goal}), [this](const PropertyList& properties) -> ReturnValue {
                if (activity_service_ == nullptr)
                    return "活动服务尚未初始化。";
                const auto goal = static_cast<uint32_t>(properties["goal"].value<int>());
                return activity_service_->SetGoal(goal)
                           ? "每日步数目标已设为 " + std::to_string(goal) + " 步。"
                           : "计步传感器不可用，或目标超出范围。";
            });
        Property duration("duration_seconds", kPropertyTypeInteger, 300, 1, 86400);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.timer.start", "启动手表倒计时，时间范围 1 秒到 24 小时。",
            PropertyList({duration}), [this](const PropertyList& properties) -> ReturnValue {
                const auto seconds =
                    static_cast<uint32_t>(properties["duration_seconds"].value<int>());
                return timer_service_.Start(seconds)
                           ? "已开始倒计时 " + std::to_string(seconds) + " 秒。"
                           : "倒计时设置失败，时间范围是 1 秒到 24 小时。";
            });
        AddLoggedWatchTool(mcp_server, "self.watch.timer.cancel", "取消当前手表倒计时。", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               return timer_service_.Cancel() ? "倒计时已取消。"
                                                              : "当前没有运行中的倒计时。";
                           });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.timer.status", "查询手表倒计时剩余时间。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return timer_service_.Status(); });
        Property alarm_hour("hour", kPropertyTypeInteger, 7, 0, 23);
        Property alarm_minute("minute", kPropertyTypeInteger, 0, 0, 59);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.alarm.set", "设置并开启每日闹钟，使用 24 小时制。",
            PropertyList({alarm_hour, alarm_minute}), [this](const PropertyList& properties) -> ReturnValue {
                const auto hour = properties["hour"].value<int>();
                const auto minute = properties["minute"].value<int>();
                return alarm_service_.SetAlarm(hour, minute)
                           ? "已设置 " + alarm_service_.Status()
                           : "闹钟时间无效，请使用 00:00 到 23:59。";
            });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.alarm.disable", "关闭每日闹钟。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                alarm_service_.SetEnabled(false);
                return "每日闹钟已关闭。";
            });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.alarm.status", "查询每日闹钟的时间和开关状态。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return alarm_service_.Status(); });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.system.status", "查询手表当前时间、电量、语音状态、音频模式和内置应用状态。",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                auto& application = Application::GetInstance();
                const char* voice_state = "待机";
                switch (application.GetDeviceState()) {
                    case kDeviceStateConnecting:
                        voice_state = "正在连接大肥鱼";
                        break;
                    case kDeviceStateListening:
                        voice_state = "正在聆听";
                        break;
                    case kDeviceStateSpeaking:
                        voice_state = "大肥鱼正在回答";
                        break;
                    case kDeviceStateWifiConfiguring:
                        voice_state = "Wi-Fi 配网中";
                        break;
                    case kDeviceStateFatalError:
                        voice_state = "发生错误";
                        break;
                    default:
                        break;
                }
                const char* audio_mode = application.GetAecMode() == kAecOnDeviceSide
                                             ? "设备端 AEC 实时语音"
                                             : application.GetAecMode() == kAecOnServerSide
                                                   ? "服务端 AEC"
                                                   : "非全双工语音";
                std::string result = "语音状态：" + std::string(voice_state) + "；音频模式：" +
                                     audio_mode + "。";
                const auto now = std::time(nullptr);
                if (now >= 1704067200) {
                    std::tm local = {};
                    char time_text[32];
                    if (localtime_r(&now, &local) != nullptr &&
                        std::strftime(time_text, sizeof(time_text), "%m月%d日 %H:%M", &local) > 0)
                        result += "当前时间 " + std::string(time_text) + "。";
                }
                int battery_level = 0;
                bool charging = false;
                bool discharging = false;
                if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
                    result += "电量 " + std::to_string(battery_level) + "%";
                    result += charging ? "，正在充电。" : discharging ? "，正在使用电池。" : "。";
                }
                result += "音乐：" + WatchMusicPlayer::GetInstance().Status();
                result += " " + timer_service_.Status();
                result += " " + alarm_service_.Status();
                return result;
            });
        Property brightness_percent("percent", kPropertyTypeInteger, 70, 5, 100);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.settings.set_brightness", "设置手表屏幕亮度，范围 5 到 100。",
            PropertyList({brightness_percent}), [](const PropertyList& properties) -> ReturnValue {
                auto* backlight = Board::GetInstance().GetBacklight();
                if (backlight == nullptr)
                    return "屏幕亮度控制暂不可用。";
                const int percent = properties["percent"].value<int>();
                backlight->SetBrightness(percent, true);
                return "屏幕亮度已设为 " + std::to_string(percent) + "%。";
            });
        Property volume_percent("percent", kPropertyTypeInteger, 55, 0, 100);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.settings.set_volume", "设置手表语音音量，范围 0 到 100。",
            PropertyList({volume_percent}), [](const PropertyList& properties) -> ReturnValue {
                auto* codec = Board::GetInstance().GetAudioCodec();
                if (codec == nullptr)
                    return "语音音量控制暂不可用。";
                const int percent = properties["percent"].value<int>();
                codec->SetOutputVolume(percent);
                return "语音音量已设为 " + std::to_string(percent) + "%。";
            });
        Property dnd_enabled("enabled", kPropertyTypeBoolean, false);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.settings.set_dnd", "开启或关闭勿扰模式。开启后倒计时结束只显示提醒，不播放提示音。",
            PropertyList({dnd_enabled}), [this](const PropertyList& properties) -> ReturnValue {
                const bool enabled = properties["enabled"].value<bool>();
                Settings preferences("watchui", true);
                preferences.SetBool("dnd", enabled);
                timer_service_.SetDoNotDisturb(enabled);
                return enabled ? "勿扰模式已开启。" : "勿扰模式已关闭。";
            });
        AddLoggedWatchTool(mcp_server, 
            "self.watch.settings.status", "查询手表屏幕亮度、语音音量和勿扰模式。",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                auto* backlight = Board::GetInstance().GetBacklight();
                auto* codec = Board::GetInstance().GetAudioCodec();
                Settings preferences("watchui", true);
                return "屏幕亮度 " +
                       std::to_string(backlight != nullptr ? backlight->brightness() : 0) +
                       "%，语音音量 " +
                       std::to_string(codec != nullptr ? codec->output_volume() : 0) +
                       "%，勿扰模式" +
                       (preferences.GetBool("dnd", false) ? "已开启。" : "已关闭。");
            });
        Property face_name("name", kPropertyTypeString, std::string());
        face_name.SetMaxLength(24);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.face.set", "切换手表中文表盘，可选：鎏金雅韵、活力运动、经典指针、软糖。",
            PropertyList({face_name}), [this](const PropertyList& properties) -> ReturnValue {
                const auto name = properties["name"].value<std::string>();
                return display_->SetWatchFace(name)
                           ? "已切换到" + name + "表盘。"
                           : "表盘名称无效，请选择鎏金雅韵、活力运动、经典指针或软糖。";
            });
        Property app_name("name", kPropertyTypeString, std::string());
        app_name.SetMaxLength(16);
        AddLoggedWatchTool(mcp_server, 
            "self.watch.app.open", "打开手表内置应用：大肥鱼、音乐、活动、闹钟、计时器、应用或设置。",
            PropertyList({app_name}), [this](const PropertyList& properties) -> ReturnValue {
                const auto name = properties["name"].value<std::string>();
                return display_->OpenWatchApp(name)
                           ? "已打开" + name + "。"
                           : "无法打开该应用，可选大肥鱼、音乐、活动、闹钟、计时器、应用或设置。";
            });
        AddLoggedWatchTool(mcp_server, "self.system.reconfigure_wifi",
                           "结束当前对话并进入 Wi-Fi 配网模式。执行前必须先征得用户确认。",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });
    }

public:
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                switch (event) {
                    case NetworkEvent::Connecting:
                        WatchSystemLog::Event("network", "connecting");
                        break;
                    case NetworkEvent::Connected:
                        WatchSystemLog::Event("network", "connected");
                        {
                            char sram_info[48];
                            std::snprintf(sram_info, sizeof(sram_info), "sram_free=%u sram_min=%u",
                                          static_cast<unsigned>(
                                              heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                                          static_cast<unsigned>(
                                              heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
                            WatchSystemLog::Event("system", "sram_after_boot", sram_info);
                        }
                        WatchDebugServer::GetInstance().Start();
                        // Covers the case where boot happened in config mode and
                        // the deferred settings server was skipped for port 80.
                        config_service_.Start();
                        break;
                    case NetworkEvent::Disconnected:
                        WatchSystemLog::Event("network", "disconnected");
                        break;
                    case NetworkEvent::WifiConfigModeEnter:
                        WatchSystemLog::Event("network", "config_mode_entered");
                        break;
                    case NetworkEvent::WifiConfigModeExit:
                        WatchSystemLog::Event("network", "config_mode_exited");
                        break;
                    case NetworkEvent::ModemErrorNoSim:
                    case NetworkEvent::ModemErrorRegDenied:
                    case NetworkEvent::ModemErrorInitFailed:
                    case NetworkEvent::ModemErrorTimeout:
                        WatchSystemLog::Event("network", "connection_error");
                        break;
                    default:
                        break;
                }
                if (event == NetworkEvent::Disconnected || event == NetworkEvent::WifiConfigModeEnter)
                    config_service_.Stop();
                if (callback)
                    callback(event, data);
            });
    }

    void EnterWifiConfigMode() {
        WatchSystemLog::Event("network", "wifi_config_requested");
        config_service_.Stop();
        WifiBoard::EnterWifiConfigMode();
    }

    WaveshareEsp32s3TouchAMOLED2inch06() : boot_button_(GPIO_NUM_NC) {
        WatchSystemLog::Initialize();
        char boot_detail[24];
        std::snprintf(boot_detail, sizeof(boot_detail), "reset_reason=%d",
                      static_cast<int>(esp_reset_reason()));
        WatchSystemLog::Event("system", "boot", boot_detail);
        {
            // Persistent boot counter: a growing count between user reports makes
            // spontaneous reboots obvious when reading /log.
            Settings diagnostics("sysdbg", true);
            const int boots = diagnostics.GetInt("boot_count", 0) + 1;
            diagnostics.SetInt("boot_count", boots);
            char boot_info[112];
            std::snprintf(boot_info, sizeof(boot_info), "boots=%d heap=%u psram=%u sram=%u",
                          boots, static_cast<unsigned>(esp_get_free_heap_size()),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            WatchSystemLog::Event("system", "boot_detail", boot_info);
            ESP_LOGI(TAG, "%s", boot_info);
        }
        setenv("TZ", "CST-8", 1);
        tzset();
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        if (rtc_.Initialize(i2c_bus_)) {
            rtc_.SyncSystemFromRtc();
            esp_timer_create_args_t rtc_sync_args = {
                .callback =
                    [](void* arg) {
                        static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg)
                            ->rtc_.SyncRtcFromSystem();
                    },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "watch_rtc_sync",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&rtc_sync_args, &rtc_sync_timer_) == ESP_OK)
                esp_timer_start_periodic(rtc_sync_timer_, 60LL * 1000 * 1000);
        } else
            WatchSystemLog::Event("rtc", "initialization_failed");
        InitializeSpi();
        InitializeSH8601Display();
        InitializeTouch();
        InitializeButtons();
        activity_service_ = new WatchActivityService(i2c_bus_);
        activity_service_->Initialize();
        if (display_ != nullptr)
            static_cast<CustomLcdDisplay*>(display_)->SetActivityService(activity_service_);
        if (display_ != nullptr)
            static_cast<CustomLcdDisplay*>(display_)->SetTimerService(&timer_service_);
        if (display_ != nullptr)
            static_cast<CustomLcdDisplay*>(display_)->SetAlarmService(&alarm_service_);
        Settings preferences("watchui", true);
        timer_service_.SetDoNotDisturb(preferences.GetBool("dnd", false));
        alarm_service_.Initialize();
        InitializeTools();
        WatchMusicPlayer::GetInstance().Initialize();
        Application::GetInstance().SetVoiceMemoOpenCallback([this] {
            if (display_ != nullptr)
                static_cast<CustomLcdDisplay*>(display_)->OpenWatchApp("语音备忘录");
        });
        config_service_.SetHandlers([this] { return ReadWatchConfig(); },
                                    [this](const WatchConfigService::Config& config) {
                                        return ApplyWatchConfig(config);
                                    });
        config_service_.SetSnapshotHandler([this](std::string& jpeg) {
            if (display_ == nullptr)
                return false;
            return static_cast<CustomLcdDisplay*>(display_)->SnapshotWatchToJpeg(jpeg, 78);
        });
        WatchDebugServer::GetInstance().SetScreenProvider([this](std::string& jpeg) {
            if (display_ == nullptr)
                return false;
            // Higher quality than the config portal: these captures are used to
            // inspect icon rendering, so JPEG artefacts would be misleading.
            return static_cast<CustomLcdDisplay*>(display_)->SnapshotWatchToJpeg(jpeg, 92);
        });
        WatchDebugServer::GetInstance().SetNavigateHandler([this](const std::string& page) {
            Application::GetInstance().Schedule([this, page] {
                if (display_ == nullptr)
                    return;
                auto* watch = static_cast<CustomLcdDisplay*>(display_);
                // The debug endpoint runs on the main task without the LVGL
                // lock; every UI mutation must take it.
                DisplayLockGuard guard(watch);
                if (!guard)
                    return;
                // A remote navigation must not be lost behind the always-on clock.
                if (power_save_timer_ != nullptr)
                    power_save_timer_->WakeUp();
                SetWatchAod(false);
                // Debug-only deep links used to verify the media pages.
                if (page.rfind("photo:", 0) == 0) {
                    watch->DebugOpenMedia(false, std::atoi(page.c_str() + 6));
                    return;
                }
                if (page.rfind("video:", 0) == 0) {
                    watch->DebugOpenMedia(true, std::atoi(page.c_str() + 6));
                    return;
                }
                watch->OpenWatchApp(page);
            });
        });
        config_service_.SetNavigateHandler([this](const std::string& page) {
            if (display_ == nullptr)
                return false;
            // Never take the LVGL lock from the HTTP task: a screenshot can be
            // holding it. Defer the page switch to the application/LVGL loop.
            Application::GetInstance().Schedule([display = display_, page] {
                if (display != nullptr)
                    static_cast<CustomLcdDisplay*>(display)->OpenWatchApp(page);
            });
            return true;
        });
        // Start the local control channel after lwIP is initialized. Starting
        // httpd from the board constructor crashes in tcpip_send_msg_wait_sem
        // because the TCP/IP mbox does not exist yet. A short one-shot timer
        // keeps the persisted control-center session available after reboot.
        esp_timer_create_args_t config_timer_args = {
            .callback =
                [](void* arg) {
                    // In WiFi config mode the provisioning AP already owns
                    // port 80; starting a second httpd there just logs
                    // "error in listen (112)". Start it once connected instead.
                    if (Application::GetInstance().GetDeviceState() ==
                        kDeviceStateWifiConfiguring) {
                        return;
                    }
                    static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg)
                        ->config_service_.Start();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "watch_webcfg",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&config_timer_args, &config_start_timer_) == ESP_OK)
            esp_timer_start_once(config_start_timer_, 5 * 1000 * 1000);
        if (display_ != nullptr) {
            display_->SetConfigPairingHandler([this] { return config_service_.BeginPairing(); });
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch06);
