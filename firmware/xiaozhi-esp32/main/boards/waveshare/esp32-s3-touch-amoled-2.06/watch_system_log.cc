#include "watch_system_log.h"

#include <esp_timer.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char kLogDirectory[] = "/sdcard/logs";
constexpr char kLogPath[] = "/sdcard/logs/system.log";
constexpr char kPreviousLogPath[] = "/sdcard/logs/system.1.log";
constexpr size_t kMaxLogBytes = 256 * 1024;
constexpr size_t kQueueLength = 64;
constexpr char kTag[] = "WatchSysLog";

struct LogRecord {
    char timestamp[28];
    char area[20];
    char action[32];
    char detail[112];
};

QueueHandle_t log_queue = nullptr;
TaskHandle_t log_task = nullptr;
std::atomic<bool> storage_ready = false;
std::atomic<uint32_t> dropped_records = 0;

void CopyField(char* output, size_t capacity, const char* input) {
    std::snprintf(output, capacity, "%s", input == nullptr ? "" : input);
    for (char* c = output; *c != '\0'; ++c) {
        if (*c == '\t' || *c == '\r' || *c == '\n')
            *c = ' ';
    }
}

void SetTimestamp(char* output, size_t capacity) {
    const std::time_t now = std::time(nullptr);
    if (now >= 1704067200) {
        std::tm local = {};
        if (localtime_r(&now, &local) != nullptr &&
            std::strftime(output, capacity, "%Y-%m-%d %H:%M:%S", &local) != 0)
            return;
    }
    const auto uptime_ms = static_cast<unsigned long long>(esp_timer_get_time() / 1000);
    std::snprintf(output, capacity, "+%llu.%03llus", uptime_ms / 1000, uptime_ms % 1000);
}

bool WriteRecord(const LogRecord& record) {
    if (mkdir(kLogDirectory, 0775) != 0) {
        struct stat directory = {};
        if (stat(kLogDirectory, &directory) != 0 || !S_ISDIR(directory.st_mode))
            return false;
    }

    char line[224];
    const int length = std::snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\n", record.timestamp,
                                     record.area, record.action, record.detail);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
        return false;

    struct stat file_info = {};
    if (stat(kLogPath, &file_info) == 0 &&
        static_cast<size_t>(file_info.st_size) + static_cast<size_t>(length) > kMaxLogBytes) {
        unlink(kPreviousLogPath);
        if (rename(kLogPath, kPreviousLogPath) != 0)
            return false;
    }

    FILE* file = std::fopen(kLogPath, "ab");
    if (file == nullptr)
        return false;
    const bool written = std::fwrite(line, 1, static_cast<size_t>(length), file) ==
                             static_cast<size_t>(length) &&
                         std::fflush(file) == 0;
    std::fclose(file);
    return written;
}

void LogTask(void*) {
    LogRecord record = {};
    while (true) {
        if (!storage_ready.load()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const auto dropped = uxQueueMessagesWaiting(log_queue) == 0
                                 ? dropped_records.exchange(0)
                                 : 0;
        if (dropped != 0) {
            SetTimestamp(record.timestamp, sizeof(record.timestamp));
            CopyField(record.area, sizeof(record.area), "system");
            CopyField(record.action, sizeof(record.action), "log_records_dropped");
            std::snprintf(record.detail, sizeof(record.detail), "%lu",
                          static_cast<unsigned long>(dropped));
            while (!WriteRecord(record))
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (xQueueReceive(log_queue, &record, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;
        while (!WriteRecord(record))
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}  // namespace

void WatchSystemLog::Initialize() {
    if (log_task != nullptr)
        return;
    log_queue = xQueueCreate(kQueueLength, sizeof(LogRecord));
    if (log_queue == nullptr) {
        ESP_LOGE(kTag, "Unable to create system log queue");
        return;
    }
    if (xTaskCreate(LogTask, "watch_syslog", 4096, nullptr, 1, &log_task) != pdPASS) {
        vQueueDelete(log_queue);
        log_queue = nullptr;
        ESP_LOGE(kTag, "Unable to start system log writer");
    }
}

void WatchSystemLog::StorageReady() {
    storage_ready = true;
    if (log_task != nullptr)
        xTaskNotifyGive(log_task);
}

void WatchSystemLog::Event(const char* area, const char* action, const char* detail) {
    if (log_queue == nullptr)
        return;
    LogRecord record = {};
    SetTimestamp(record.timestamp, sizeof(record.timestamp));
    CopyField(record.area, sizeof(record.area), area);
    CopyField(record.action, sizeof(record.action), action);
    CopyField(record.detail, sizeof(record.detail), detail);
    if (xQueueSend(log_queue, &record, 0) != pdTRUE)
        ++dropped_records;
    if (storage_ready.load() && log_task != nullptr)
        xTaskNotifyGive(log_task);
}
