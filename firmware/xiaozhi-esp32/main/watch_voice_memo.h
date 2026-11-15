#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct VoiceMemoEntry {
    std::string path;
    std::string name;
    uint32_t duration_ms = 0;
    int64_t modified = 0;
};

class WatchVoiceMemoRecorder {
public:
    static WatchVoiceMemoRecorder& GetInstance();

    bool Start();
    void Append(const std::vector<int16_t>& samples);
    bool Stop();
    bool IsRecording() const { return recording_.load(); }
    uint32_t ElapsedSeconds() const;
    std::string CurrentPath() const;
    std::string LastPath() const;

    // Recordings in /sdcard/Recordings, newest first.
    static std::vector<VoiceMemoEntry> ListRecordings();

private:
    bool WriteHeader(uint32_t data_size);

    mutable std::mutex mutex_;
    FILE* file_ = nullptr;
    std::atomic_bool recording_{false};
    int64_t started_us_ = 0;
    int64_t finished_us_ = 0;
    uint32_t data_bytes_ = 0;
    std::string current_path_;
    std::string last_path_;
};
