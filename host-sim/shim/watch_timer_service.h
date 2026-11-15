#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class WatchTimerService {
public:
    bool Start(uint32_t duration_seconds);
    bool Cancel();
    bool Active() const { Tick(); return active_; }
    bool Expired() const { Tick(); return expired_; }
    void SetDoNotDisturb(bool enabled) { do_not_disturb_ = enabled; }
    uint32_t RemainingSeconds() const;
    std::string Status() const;

private:
    void Tick() const;
    mutable bool active_ = false;
    mutable bool expired_ = false;
    bool do_not_disturb_ = false;
    std::chrono::steady_clock::time_point ends_at_{};
};
