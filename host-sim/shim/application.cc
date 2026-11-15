#include "application.h"

#include "assets/lang_config.h"
#include "display/lcd_display.h"

#include <utility>

Application& Application::GetInstance() {
    static Application application;
    return application;
}

void Application::Schedule(std::function<void()> task) {
    if (task) tasks_.push_back(std::move(task));
}

void Application::ProcessScheduled() {
    while (!tasks_.empty()) {
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        if (task) task();
    }
}

void Application::StartListening() {
    state_ = "listening";
    if (display_ != nullptr) display_->SetStatus(Lang::Strings::LISTENING);
}

void Application::SimulateSpeaking(const std::string& text) {
    state_ = "speaking";
    if (display_ != nullptr) {
        display_->SetStatus(Lang::Strings::SPEAKING);
        display_->SetChatMessage("assistant", text.c_str());
    }
}

void Application::StopAssistant() {
    state_ = "idle";
    if (display_ != nullptr) display_->SetStatus(Lang::Strings::STANDBY);
}

void Application::StopListening() { state_ = "idle"; }
void Application::AbortSpeaking(AbortReason) { state_ = "idle"; }
bool Application::SetDeviceState(DeviceState state) { state_ = state == kDeviceStateIdle ? "idle" : "idle"; return true; }
