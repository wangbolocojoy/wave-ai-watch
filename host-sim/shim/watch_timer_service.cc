#include "watch_timer_service.h"

void WatchTimerService::Tick() const {
    if (!active_) return;
    if (std::chrono::steady_clock::now() < ends_at_) return;
    active_ = false;
    expired_ = true;
}

bool WatchTimerService::Start(uint32_t duration_seconds) {
    if (duration_seconds < 1 || duration_seconds > 86400) return false;
    active_ = true;
    expired_ = false;
    ends_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
    return true;
}

bool WatchTimerService::Cancel() {
    if (!active_) return false;
    active_ = false;
    expired_ = false;
    return true;
}

uint32_t WatchTimerService::RemainingSeconds() const {
    Tick();
    if (!active_) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        ends_at_ - std::chrono::steady_clock::now()).count();
    return remaining <= 0 ? 0 : static_cast<uint32_t>(remaining);
}

std::string WatchTimerService::Status() const {
    Tick();
    if (active_) return "计时中：" + std::to_string(RemainingSeconds()) + "秒后结束。";
    return expired_ ? "倒计时已结束。" : "当前没有运行中的倒计时。";
}
