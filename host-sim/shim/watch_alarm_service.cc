#include "watch_alarm_service.h"

#include <cstdio>

bool WatchAlarmService::SetAlarm(int hour, int minute, bool enabled) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    hour_ = hour;
    minute_ = minute;
    enabled_ = enabled;
    return true;
}

bool WatchAlarmService::SetEnabled(bool enabled) {
    enabled_ = enabled;
    return true;
}

std::string WatchAlarmService::Status() const {
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", hour_, minute_);
    return std::string("每日闹钟 ") + text + (enabled_ ? "，已开启。" : "，已关闭。");
}
