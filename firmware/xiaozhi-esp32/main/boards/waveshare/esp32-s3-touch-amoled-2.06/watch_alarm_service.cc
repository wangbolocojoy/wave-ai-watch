#include "watch_alarm_service.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"
#include "settings.h"
#include "watch_system_log.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
constexpr char kTag[] = "WatchAlarm";
constexpr uint64_t kCheckIntervalUs = 10ULL * 1000 * 1000;
}

WatchAlarmService::~WatchAlarmService() {
    if (timer_ != nullptr)
        esp_timer_delete(timer_);
}

bool WatchAlarmService::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings preferences("watchalarm");
        const auto hour = preferences.GetInt("hour", 7);
        const auto minute = preferences.GetInt("minute", 0);
        minute_of_day_ = static_cast<uint16_t>((hour >= 0 && hour < 24 ? hour : 7) * 60 +
                                               (minute >= 0 && minute < 60 ? minute : 0));
        enabled_ = preferences.GetBool("enabled", false);
        last_fired_day_ = preferences.GetInt("fired_day", -1);
    }

    const esp_timer_create_args_t args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "watch_alarm",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer_) != ESP_OK) {
        ESP_LOGE(kTag, "Unable to create daily alarm timer");
        WatchSystemLog::Event("alarm", "timer_create_failed");
        return false;
    }
    if (esp_timer_start_periodic(timer_, kCheckIntervalUs) != ESP_OK) {
        ESP_LOGE(kTag, "Unable to start daily alarm timer");
        esp_timer_delete(timer_);
        timer_ = nullptr;
        WatchSystemLog::Event("alarm", "timer_start_failed");
        return false;
    }
    WatchSystemLog::Event("alarm", "initialized");
    return true;
}

bool WatchAlarmService::SetAlarm(int hour, int minute, bool enabled) {
    if (hour > 23 || minute > 59) {
        WatchSystemLog::Event("alarm", "set_rejected");
        return false;
    }
    if (hour < 0 || minute < 0) {
        WatchSystemLog::Event("alarm", "set_rejected");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (minute_of_day_ != hour * 60 + minute)
            last_fired_day_ = -1;
        minute_of_day_ = static_cast<uint16_t>(hour * 60 + minute);
        enabled_ = enabled;
        Settings preferences("watchalarm", true);
        preferences.SetInt("hour", hour);
        preferences.SetInt("minute", minute);
        preferences.SetBool("enabled", enabled);
        preferences.SetInt("fired_day", last_fired_day_);
    }
    char detail[8];
    std::snprintf(detail, sizeof(detail), "%02d:%02d", hour, minute);
    WatchSystemLog::Event("alarm", enabled ? "set_enabled" : "set_disabled", detail);
    return true;
}

bool WatchAlarmService::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    Settings preferences("watchalarm", true);
    preferences.SetBool("enabled", enabled);
    WatchSystemLog::Event("alarm", enabled ? "enabled" : "disabled");
    return true;
}

bool WatchAlarmService::Enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

uint8_t WatchAlarmService::Hour() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint8_t>(minute_of_day_ / 60);
}

uint8_t WatchAlarmService::Minute() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint8_t>(minute_of_day_ % 60);
}

std::string WatchAlarmService::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char time_text[6];
    const int hour = std::min<int>(23, minute_of_day_ / 60);
    const int minute = std::min<int>(59, minute_of_day_ % 60);
    std::snprintf(time_text, sizeof(time_text), "%02d:%02d", hour, minute);
    return std::string("每日闹钟 ") + time_text + (enabled_ ? "，已开启。" : "，已关闭。");
}

void WatchAlarmService::TimerCallback(void* context) {
    static_cast<WatchAlarmService*>(context)->CheckAlarm();
}

void WatchAlarmService::CheckAlarm() {
    const std::time_t now = std::time(nullptr);
    if (now < 1704067200)
        return;
    std::tm local = {};
    if (localtime_r(&now, &local) == nullptr)
        return;

    const int32_t day_key = (local.tm_year + 1900) * 366 + local.tm_yday;
    bool should_fire = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || local.tm_hour * 60 + local.tm_min != minute_of_day_ ||
            day_key == last_fired_day_)
            return;
        last_fired_day_ = day_key;
        should_fire = true;
        Settings preferences("watchalarm", true);
        preferences.SetInt("fired_day", day_key);
    }
    if (!should_fire)
        return;
    WatchSystemLog::Event("alarm", "triggered");

    Application::GetInstance().Schedule([] {
        Settings preferences("watchui");
        if (!preferences.GetBool("dnd", false))
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
        if (auto* display = Board::GetInstance().GetDisplay())
            display->ShowNotification("闹钟响了");
    });
}
