#pragma once

#include <functional>
#include <string>
#include <deque>

class Display;

enum AbortReason { kAbortReasonNone };
enum DeviceState { kDeviceStateIdle };

class Application {
public:
    static Application& GetInstance();

    void SetDisplay(Display* display) { display_ = display; }
    void Schedule(std::function<void()> task);
    void ProcessScheduled();

    void StartListening();
    void StopListening();
    void AbortSpeaking(AbortReason reason);
    bool SetDeviceState(DeviceState state);
    void SimulateSpeaking(const std::string& text = "你好，我在这里");
    void StopAssistant();
    const std::string& State() const { return state_; }

private:
    Display* display_ = nullptr;
    std::deque<std::function<void()>> tasks_;
    std::string state_ = "idle";
};
