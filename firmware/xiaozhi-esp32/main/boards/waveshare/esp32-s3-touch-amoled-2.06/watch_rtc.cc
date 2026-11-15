#include "watch_rtc.h"

#include <esp_log.h>
#include <sys/time.h>

#include <cstring>

namespace {
constexpr char kTag[] = "WatchRtc";
constexpr uint8_t kAddress = 0x51;
constexpr uint8_t kControl1 = 0x00;
constexpr uint8_t kSeconds = 0x04;
constexpr int kTransactionTimeoutMs = 50;

uint8_t ToBcd(int value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

int FromBcd(uint8_t value) {
    return ((value >> 4) * 10) + (value & 0x0F);
}

bool Plausible(const std::tm& local) {
    return local.tm_year + 1900 >= 2024 && local.tm_year + 1900 <= 2099 &&
           local.tm_mon >= 0 && local.tm_mon < 12 && local.tm_mday >= 1 &&
           local.tm_mday <= 31 && local.tm_hour >= 0 && local.tm_hour < 24 &&
           local.tm_min >= 0 && local.tm_min < 60 && local.tm_sec >= 0 &&
           local.tm_sec < 60;
}
}  // namespace

bool WatchRtc::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr)
        return false;
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        return false;
    }
    uint8_t probe = 0;
    if (!ReadRegisters(0x03, &probe, 1)) {
        ESP_LOGW(kTag, "PCF85063 not found at 0x%02x", kAddress);
        return false;
    }
    StartOscillator();
    available_ = true;
    ESP_LOGI(kTag, "PCF85063 RTC ready");
    return true;
}

bool WatchRtc::WriteRegister(uint8_t reg, const uint8_t* data, size_t length) {
    if (device_ == nullptr || length > 7)
        return false;
    uint8_t buffer[8] = {};
    buffer[0] = reg;
    std::memcpy(buffer + 1, data, length);
    return i2c_master_transmit(device_, buffer, length + 1, kTransactionTimeoutMs) == ESP_OK;
}

bool WatchRtc::ReadRegisters(uint8_t reg, uint8_t* data, size_t length) {
    if (device_ == nullptr || length == 0 || length > 8)
        return false;
    return i2c_master_transmit(device_, &reg, 1, kTransactionTimeoutMs) == ESP_OK &&
           i2c_master_receive(device_, data, length, kTransactionTimeoutMs) == ESP_OK;
}

void WatchRtc::StartOscillator() {
    uint8_t control = 0;
    if (!ReadRegisters(kControl1, &control, 1))
        return;
    // Clear STOP and force 24-hour mode.
    control &= static_cast<uint8_t>(~(1u << 5));
    control &= static_cast<uint8_t>(~(1u << 1));
    WriteRegister(kControl1, &control, 1);
}

bool WatchRtc::Read(std::tm& local) {
    if (!available_)
        return false;
    uint8_t data[7] = {};
    if (!ReadRegisters(kSeconds, data, sizeof(data)))
        return false;
    if (data[0] & 0x80) {
        ESP_LOGW(kTag, "RTC oscillator was stopped; time is invalid");
        return false;
    }
    std::tm value = {};
    value.tm_sec = FromBcd(data[0] & 0x7F);
    value.tm_min = FromBcd(data[1] & 0x7F);
    value.tm_hour = FromBcd(data[2] & 0x3F);
    value.tm_mday = FromBcd(data[3] & 0x3F);
    value.tm_mon = FromBcd(data[5] & 0x1F) - 1;
    value.tm_year = FromBcd(data[6]) + 100;
    value.tm_isdst = -1;
    if (!Plausible(value))
        return false;
    local = value;
    return true;
}

bool WatchRtc::Write(const std::tm& local) {
    if (!available_ || !Plausible(local))
        return false;
    const int year = local.tm_year + 1900;
    std::tm copy = local;
    std::mktime(&copy);  // normalize tm_wday
    uint8_t data[7] = {
        static_cast<uint8_t>(ToBcd(local.tm_sec) & 0x7F),
        ToBcd(local.tm_min),
        ToBcd(local.tm_hour),
        ToBcd(local.tm_mday),
        static_cast<uint8_t>(copy.tm_wday == 0 ? 6 : copy.tm_wday - 1),
        ToBcd(local.tm_mon + 1),
        ToBcd(year % 100),
    };
    if (!WriteRegister(kSeconds, data, sizeof(data)))
        return false;
    StartOscillator();
    return true;
}

bool WatchRtc::SyncSystemFromRtc() {
    std::tm local = {};
    if (!Read(local))
        return false;
    const auto now = std::time(nullptr);
    if (now >= 1704067200)
        return true;  // Network/system time is already newer; keep it.
    local.tm_isdst = -1;
    const auto timestamp = std::mktime(&local);
    if (timestamp <= 0)
        return false;
    timeval tv = {.tv_sec = timestamp, .tv_usec = 0};
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(kTag, "settimeofday failed");
        return false;
    }
    ESP_LOGI(kTag, "Restored system clock from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
             local.tm_min, local.tm_sec);
    return true;
}

bool WatchRtc::SyncRtcFromSystem() {
    const auto now = std::time(nullptr);
    if (now < 1704067200)
        return false;
    std::tm local = {};
    if (localtime_r(&now, &local) == nullptr)
        return false;
    return Write(local);
}
