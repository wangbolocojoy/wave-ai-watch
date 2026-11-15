#pragma once

#include <cstdint>

class WatchActivityService {
public:
    explicit WatchActivityService(void* bus = nullptr) : bus_(bus) {}
    bool Initialize() { return true; }
    uint32_t TodaySteps() const { return steps_; }
    uint32_t Goal() const { return goal_; }
    bool Available() const { return available_; }
    bool SetGoal(uint32_t goal) {
        if (goal < 1000 || goal > 50000) return false;
        goal_ = goal;
        return true;
    }
    void SetSteps(uint32_t steps) { steps_ = steps; }
    void SetAvailable(bool available) { available_ = available; }

private:
    void* bus_ = nullptr;
    uint32_t steps_ = 6842;
    uint32_t goal_ = 8000;
    bool available_ = true;
};
