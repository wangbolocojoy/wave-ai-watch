#pragma once

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>

class WatchActivityService {
public:
    explicit WatchActivityService(i2c_master_bus_handle_t bus);
    bool Initialize();
    uint32_t TodaySteps() const;
    uint32_t Goal() const;
    bool Available() const;
    bool SetGoal(uint32_t goal);

private:
    static void PollTask(void* context);
    bool ReadSteps(uint32_t& steps) const;
    bool WriteRegister(uint8_t reg, uint8_t value) const;
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t length) const;
    bool ConfigurePedometer();
    void Poll();
    bool SaveState();

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    uint32_t today_steps_ = 0;
    uint32_t goal_ = 8000;
    uint32_t previous_raw_steps_ = 0;
    uint32_t date_key_ = 0;
    bool available_ = false;
    bool have_raw_baseline_ = false;
};
