#include "watch_timer_service.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"
#include "watch_system_log.h"

#include <esp_log.h>

#include <cstdio>

namespace {
constexpr char kTag[] = "WatchTimer";
constexpr uint32_t kMinDurationSeconds = 1;
constexpr uint32_t kMaxDurationSeconds = 86400;
}  // namespace

WatchTimerService::~WatchTimerService() {
    if (timer_ != nullptr)
        esp_timer_delete(timer_);
}

bool WatchTimerService::Start(uint32_t duration_seconds) {
    if (duration_seconds < kMinDurationSeconds || duration_seconds > kMaxDurationSeconds) {
        WatchSystemLog::Event("timer", "start_rejected");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (timer_ == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = TimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "watch_timer",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &timer_) != ESP_OK) {
            ESP_LOGE(kTag, "Unable to create countdown timer");
            WatchSystemLog::Event("timer", "create_failed");
            return false;
        }
    } else {
        esp_timer_stop(timer_);
    }

    expired_ = false;
    active_ = true;
    const int64_t remaining_us = static_cast<int64_t>(duration_seconds) * 1000000;
    deadline_us_ = esp_timer_get_time() + remaining_us;
    if (esp_timer_start_once(timer_, remaining_us) != ESP_OK) {
        active_ = false;
        deadline_us_ = 0;
        WatchSystemLog::Event("timer", "start_failed");
        return false;
    }
    char detail[24];
    std::snprintf(detail, sizeof(detail), "%lu seconds", static_cast<unsigned long>(duration_seconds));
    WatchSystemLog::Event("timer", "started", detail);
    return true;
}

bool WatchTimerService::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_)
        return false;
    if (timer_ != nullptr)
        esp_timer_stop(timer_);
    active_ = false;
    expired_ = false;
    deadline_us_ = 0;
    WatchSystemLog::Event("timer", "cancelled");
    return true;
}

bool WatchTimerService::Active() const { return active_.load(); }

bool WatchTimerService::Expired() const { return expired_.load(); }

void WatchTimerService::SetDoNotDisturb(bool enabled) { do_not_disturb_ = enabled; }

uint32_t WatchTimerService::RemainingSeconds() const {
    if (!active_)
        return 0;
    const int64_t remaining_us = deadline_us_.load() - esp_timer_get_time();
    if (remaining_us <= 0)
        return 0;
    return static_cast<uint32_t>((remaining_us + 999999) / 1000000);
}

std::string WatchTimerService::Status() const {
    if (Active()) {
        const uint32_t seconds = RemainingSeconds();
        return "计时中：" + std::to_string(seconds / 60) + "分" + std::to_string(seconds % 60) +
               "秒后结束。";
    }
    return Expired() ? "倒计时已结束。" : "当前没有运行中的倒计时。";
}

void WatchTimerService::TimerCallback(void* context) {
    static_cast<WatchTimerService*>(context)->OnTimer();
}

void WatchTimerService::OnTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_)
        return;
    const int64_t remaining_us = deadline_us_.load() - esp_timer_get_time();
    if (remaining_us > 0) {
        // A callback from a replaced countdown may already have been queued.
        esp_timer_start_once(timer_, remaining_us);
        return;
    }
    active_ = false;
    expired_ = true;
    deadline_us_ = 0;
    WatchSystemLog::Event("timer", "expired");
    const bool do_not_disturb = do_not_disturb_.load();
    Application::GetInstance().Schedule([do_not_disturb] {
        if (!do_not_disturb)
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
        if (auto* display = Board::GetInstance().GetDisplay())
            display->ShowNotification("倒计时结束");
    });
}
