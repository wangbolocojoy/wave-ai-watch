#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class WatchTimerService {
public:
    WatchTimerService() = default;
    ~WatchTimerService();

    bool Start(uint32_t duration_seconds);
    bool Cancel();
    bool Active() const;
    bool Expired() const;
    void SetDoNotDisturb(bool enabled);
    uint32_t RemainingSeconds() const;
    std::string Status() const;

private:
    static void TimerCallback(void* context);
    void OnTimer();

    mutable std::mutex mutex_;
    esp_timer_handle_t timer_ = nullptr;
    std::atomic<int64_t> deadline_us_{0};
    std::atomic<bool> active_{false};
    std::atomic<bool> expired_{false};
    std::atomic<bool> do_not_disturb_{false};
};
