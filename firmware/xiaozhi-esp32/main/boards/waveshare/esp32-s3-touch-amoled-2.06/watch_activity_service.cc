#include "watch_activity_service.h"

#include "settings.h"
#include "watch_system_log.h"

#include <esp_err.h>
#include <esp_log.h>

#include <ctime>
#include <cstdio>

namespace {
constexpr char kTag[] = "WatchActivity";
constexpr uint8_t kQmiAddressCandidates[] = {0x6A, 0x6B};
constexpr uint32_t kStepCounterMask = 0x00FFFFFF;
constexpr uint32_t kMaxReasonableStepDelta = 1000;

uint32_t CurrentDateKey() {
    const time_t now = std::time(nullptr);
    if (now < 1704067200)
        return 0;
    std::tm local = {};
    if (localtime_r(&now, &local) == nullptr)
        return 0;
    return static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 +
                                 local.tm_mday);
}
}  // namespace

WatchActivityService::WatchActivityService(i2c_master_bus_handle_t bus) : bus_(bus) {
    mutex_ = xSemaphoreCreateMutex();
    Settings preferences("watchact");
    today_steps_ = static_cast<uint32_t>(preferences.GetInt("steps", 0));
    goal_ = static_cast<uint32_t>(preferences.GetInt("goal", 8000));
    date_key_ = static_cast<uint32_t>(preferences.GetInt("date", 0));
}

bool WatchActivityService::Initialize() {
    if (mutex_ == nullptr || bus_ == nullptr)
        return false;
    for (const uint8_t address : kQmiAddressCandidates) {
        if (i2c_master_probe(bus_, address, 50) != ESP_OK)
            continue;
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus_, &config, &device_) == ESP_OK)
            break;
    }
    uint8_t chip_id = 0;
    if (device_ == nullptr || !ReadRegister(0x00, &chip_id, 1) || chip_id != 0x05 ||
        !ConfigurePedometer()) {
        if (device_ != nullptr) {
            i2c_master_bus_rm_device(device_);
            device_ = nullptr;
        }
        ESP_LOGW(kTag, "QMI8658 pedometer unavailable; activity page will show sensor status");
        WatchSystemLog::Event("activity", "sensor_unavailable");
        return false;
    }

    available_ = true;
    uint32_t raw_steps = 0;
    if (ReadSteps(raw_steps)) {
        previous_raw_steps_ = raw_steps;
        have_raw_baseline_ = true;
    }
    const uint32_t current_date = CurrentDateKey();
    if (current_date != 0 && date_key_ != 0 && current_date != date_key_)
        today_steps_ = 0;
    if (current_date != 0)
        date_key_ = current_date;
    SaveState();
    if (xTaskCreate(PollTask, "watch_steps", 3072, this, 3, nullptr) != pdPASS) {
        available_ = false;
        ESP_LOGE(kTag, "Unable to start step counter task");
        return false;
    }
    ESP_LOGI(kTag, "QMI8658 pedometer ready");
    WatchSystemLog::Event("activity", "sensor_ready", "QMI8658");
    return true;
}

bool WatchActivityService::ReadRegister(uint8_t reg, uint8_t* data, size_t length) const {
    return device_ != nullptr &&
           i2c_master_transmit_receive(device_, &reg, 1, data, length, 50) == ESP_OK;
}

bool WatchActivityService::WriteRegister(uint8_t reg, uint8_t value) const {
    if (device_ == nullptr)
        return false;
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(device_, data, sizeof(data), 50) == ESP_OK;
}

bool WatchActivityService::ConfigurePedometer() {
    // Mirror the vendor QMI8658 sequence: stop sensors, configure the two
    // pedometer parameter blocks through CAL1..CAL4, then enable the engine.
    if (!WriteRegister(0x08, 0x00) || !WriteRegister(0x02, 0x40))
        return false;

    auto send_calibration = [this](const uint8_t* values) {
        for (size_t i = 0; i < 8; ++i) {
            if (!WriteRegister(static_cast<uint8_t>(0x0B + i), values[i]))
                return false;
        }
        // CTRL9 is the command register; 0x0D = configure pedometer.
        if (!WriteRegister(0x0A, 0x0D))
            return false;
        bool completed = false;
        for (int retry = 0; retry < 50; ++retry) {
            uint8_t status = 0;
            if (!ReadRegister(0x2D, &status, 1))
                return false;
            if ((status & 0x80) != 0) {
                completed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return completed && WriteRegister(0x0A, 0x00);
    };

    const uint8_t first_command[8] = {62, 0, 0xCC, 0, 0x66, 0, 2, 1};
    const uint8_t second_command[8] = {250, 0, 25, 10, 0, 4, 2, 2};
    if (!send_calibration(first_command) || !send_calibration(second_command)) {
        ESP_LOGW(kTag, "QMI8658 pedometer calibration command failed");
        return false;
    }

    // ±8 g, 62.5 Hz; enable accelerometer and the pedometer engine.
    if (!WriteRegister(0x03, 0x27) || !WriteRegister(0x08, 0x01) ||
        !WriteRegister(0x09, 0x10)) {
        ESP_LOGW(kTag, "QMI8658 pedometer enable failed");
        return false;
    }
    return true;
}

bool WatchActivityService::ReadSteps(uint32_t& steps) const {
    uint8_t data[3] = {};
    if (!ReadRegister(0x5A, data, sizeof(data)))
        return false;
    steps = (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[1]) << 8) |
            static_cast<uint32_t>(data[0]);
    return true;
}

void WatchActivityService::PollTask(void* context) {
    auto* self = static_cast<WatchActivityService*>(context);
    while (true) {
        self->Poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void WatchActivityService::Poll() {
    uint32_t raw_steps = 0;
    if (!ReadSteps(raw_steps))
        return;
    bool changed = false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
        return;
    if (have_raw_baseline_) {
        const uint32_t delta = (raw_steps - previous_raw_steps_) & kStepCounterMask;
        if (delta <= kMaxReasonableStepDelta && delta != 0) {
            today_steps_ += delta;
            changed = true;
        }
    }
    previous_raw_steps_ = raw_steps;
    have_raw_baseline_ = true;
    const uint32_t current_date = CurrentDateKey();
    if (current_date != 0 && date_key_ != current_date) {
        date_key_ = current_date;
        today_steps_ = 0;
        changed = true;
    }
    xSemaphoreGive(mutex_);

    static uint16_t save_ticks = 0;
    static bool dirty = false;
    dirty = dirty || changed;
    if (++save_ticks >= 300) {
        save_ticks = 0;
        if (dirty && SaveState())
            dirty = false;
    }
}

bool WatchActivityService::SaveState() {
    if (mutex_ != nullptr && xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    const auto steps = today_steps_;
    const auto goal = goal_;
    const auto date = date_key_;
    if (mutex_ != nullptr)
        xSemaphoreGive(mutex_);
    Settings preferences("watchact", true);
    preferences.SetInt("steps", static_cast<int32_t>(steps));
    preferences.SetInt("goal", static_cast<int32_t>(goal));
    preferences.SetInt("date", static_cast<int32_t>(date));
    return true;
}

uint32_t WatchActivityService::TodaySteps() const {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
        return 0;
    const uint32_t steps = today_steps_;
    xSemaphoreGive(mutex_);
    return steps;
}

uint32_t WatchActivityService::Goal() const {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
        return 8000;
    const uint32_t goal = goal_;
    xSemaphoreGive(mutex_);
    return goal;
}

bool WatchActivityService::Available() const { return available_; }

bool WatchActivityService::SetGoal(uint32_t goal) {
    if (goal < 1000 || goal > 50000 || mutex_ == nullptr ||
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        WatchSystemLog::Event("activity", "goal_rejected");
        return false;
    }
    goal_ = goal;
    xSemaphoreGive(mutex_);
    SaveState();
    char detail[20];
    std::snprintf(detail, sizeof(detail), "%lu steps", static_cast<unsigned long>(goal));
    WatchSystemLog::Event("activity", "goal_set", detail);
    return true;
}
