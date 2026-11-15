#pragma once

#include <driver/i2c_master.h>

#include <ctime>

class WatchRtc {
public:
    bool Initialize(i2c_master_bus_handle_t bus);
    bool Available() const { return available_; }
    bool Read(std::tm& local);
    bool Write(const std::tm& local);
    bool SyncSystemFromRtc();
    bool SyncRtcFromSystem();

private:
    bool WriteRegister(uint8_t reg, const uint8_t* data, size_t length);
    bool ReadRegisters(uint8_t reg, uint8_t* data, size_t length);
    void StartOscillator();

    i2c_master_dev_handle_t device_ = nullptr;
    bool available_ = false;
};
