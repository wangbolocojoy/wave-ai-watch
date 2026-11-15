#pragma once

#include <esp_timer.h>

#include <cstdint>
#include <mutex>
#include <string>

class WatchAlarmService {
public:
    WatchAlarmService() = default;
    ~WatchAlarmService();

    bool Initialize();
    bool SetAlarm(int hour, int minute, bool enabled = true);
    bool SetEnabled(bool enabled);
    bool Enabled() const;
    uint8_t Hour() const;
    uint8_t Minute() const;
    std::string Status() const;

private:
    static void TimerCallback(void* context);
    void CheckAlarm();

    mutable std::mutex mutex_;
    esp_timer_handle_t timer_ = nullptr;
    uint16_t minute_of_day_ = 7 * 60;
    int32_t last_fired_day_ = -1;
    bool enabled_ = false;
};
