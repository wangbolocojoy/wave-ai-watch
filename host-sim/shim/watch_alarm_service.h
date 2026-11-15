#pragma once

#include <cstdint>
#include <string>

class WatchAlarmService {
public:
    bool SetAlarm(int hour, int minute, bool enabled = true);
    bool SetEnabled(bool enabled);
    bool Enabled() const { return enabled_; }
    uint8_t Hour() const { return hour_; }
    uint8_t Minute() const { return minute_; }
    std::string Status() const;

private:
    int hour_ = 7;
    int minute_ = 0;
    bool enabled_ = false;
};
