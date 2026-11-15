#include "watch_voice_memo.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include <esp_timer.h>

namespace {
constexpr char kRecordDirectory[] = "/sdcard/Recordings";
constexpr uint32_t kSampleRate = 16000;

void WriteU16(FILE* file, uint16_t value) {
    const uint8_t bytes[] = {static_cast<uint8_t>(value),
                             static_cast<uint8_t>(value >> 8)};
    std::fwrite(bytes, sizeof(bytes), 1, file);
}

void WriteU32(FILE* file, uint32_t value) {
    const uint8_t bytes[] = {static_cast<uint8_t>(value),
                             static_cast<uint8_t>(value >> 8),
                             static_cast<uint8_t>(value >> 16),
                             static_cast<uint8_t>(value >> 24)};
    std::fwrite(bytes, sizeof(bytes), 1, file);
}
uint32_t ReadU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

// Our memos are 16 kHz mono 16-bit WAV; fall back to that byte rate when the
// header cannot be parsed.
uint32_t WavDurationMs(const std::string& path, long file_size) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return 0;
    uint8_t header[44] = {};
    uint32_t duration_ms = 0;
    if (std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
        std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVE", 4) == 0) {
        const uint32_t byte_rate = ReadU32(header + 28);
        const uint32_t data_size = ReadU32(header + 40);
        if (byte_rate > 0)
            duration_ms = static_cast<uint32_t>(
                std::min<uint64_t>(uint64_t(data_size) * 1000 / byte_rate, UINT32_MAX));
    }
    std::fclose(file);
    if (duration_ms == 0 && file_size > 44)
        duration_ms = static_cast<uint32_t>(
            std::min<uint64_t>(uint64_t(file_size - 44) * 1000 / (kSampleRate * 2), UINT32_MAX));
    return duration_ms;
}
}  // namespace

WatchVoiceMemoRecorder& WatchVoiceMemoRecorder::GetInstance() {
    static WatchVoiceMemoRecorder recorder;
    return recorder;
}

bool WatchVoiceMemoRecorder::WriteHeader(uint32_t data_size) {
    if (file_ == nullptr || std::fseek(file_, 0, SEEK_SET) != 0)
        return false;
    std::fwrite("RIFF", 1, 4, file_);
    WriteU32(file_, data_size + 36);
    std::fwrite("WAVEfmt ", 1, 8, file_);
    WriteU32(file_, 16);
    WriteU16(file_, 1);  // PCM
    WriteU16(file_, 1);  // mono
    WriteU32(file_, kSampleRate);
    WriteU32(file_, kSampleRate * 2);
    WriteU16(file_, 2);
    WriteU16(file_, 16);
    std::fwrite("data", 1, 4, file_);
    WriteU32(file_, data_size);
    return std::fseek(file_, 0, SEEK_END) == 0;
}

bool WatchVoiceMemoRecorder::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr)
        return true;
    struct stat sd = {};
    if (stat("/sdcard", &sd) != 0 || !S_ISDIR(sd.st_mode))
        return false;
    if (mkdir(kRecordDirectory, 0775) != 0) {
        struct stat directory = {};
        if (stat(kRecordDirectory, &directory) != 0 || !S_ISDIR(directory.st_mode))
            return false;
    }

    time_t now = time(nullptr);
    struct tm local = {};
    char filename[48] = {};
    if (now > 1700000000 && localtime_r(&now, &local) != nullptr) {
        std::strftime(filename, sizeof(filename), "memo_%Y%m%d_%H%M%S.wav", &local);
    } else {
        std::snprintf(filename, sizeof(filename), "memo_%llu.wav",
                      static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    }
    current_path_ = std::string(kRecordDirectory) + "/" + filename;
    file_ = std::fopen(current_path_.c_str(), "wb+");
    if (file_ == nullptr) {
        current_path_.clear();
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 8192);
    data_bytes_ = 0;
    started_us_ = esp_timer_get_time();
    finished_us_ = 0;
    if (!WriteHeader(0)) {
        std::fclose(file_);
        file_ = nullptr;
        current_path_.clear();
        return false;
    }
    recording_.store(true);
    return true;
}

void WatchVoiceMemoRecorder::Append(const std::vector<int16_t>& samples) {
    if (!recording_.load() || samples.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_.load() || file_ == nullptr)
        return;
    const size_t count = samples.size();
    const size_t written = std::fwrite(samples.data(), sizeof(int16_t), count, file_);
    data_bytes_ += static_cast<uint32_t>(written * sizeof(int16_t));
    if (written != count || data_bytes_ >= 0xFFFFFF00u)
        recording_.store(false);
}

bool WatchVoiceMemoRecorder::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) {
        recording_.store(false);
        return false;
    }
    recording_.store(false);
    const bool header_ok = WriteHeader(data_bytes_);
    const bool flush_ok = std::fflush(file_) == 0;
    const bool close_ok = std::fclose(file_) == 0;
    finished_us_ = esp_timer_get_time();
    file_ = nullptr;
    if (data_bytes_ > 0)
        last_path_ = current_path_;
    current_path_.clear();
    return header_ok && flush_ok && close_ok && data_bytes_ > 0;
}

uint32_t WatchVoiceMemoRecorder::ElapsedSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t start = started_us_;
    if (start <= 0)
        return 0;
    const int64_t end = recording_.load() ? esp_timer_get_time() : finished_us_;
    if (end <= start)
        return 0;
    return static_cast<uint32_t>((end - start) / 1000000);
}

std::string WatchVoiceMemoRecorder::CurrentPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_path_;
}

std::string WatchVoiceMemoRecorder::LastPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_path_;
}

std::vector<VoiceMemoEntry> WatchVoiceMemoRecorder::ListRecordings() {
    std::vector<VoiceMemoEntry> entries;
    DIR* directory = opendir(kRecordDirectory);
    if (directory == nullptr)
        return entries;
    while (auto* dir_entry = readdir(directory)) {
        if (dir_entry->d_name[0] == '.')
            continue;
        std::string name(dir_entry->d_name);
        if (name.size() < 5)
            continue;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.substr(lower.size() - 4) != ".wav")
            continue;
        VoiceMemoEntry entry;
        entry.path = std::string(kRecordDirectory) + "/" + name;
        struct stat info = {};
        if (stat(entry.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        entry.name = std::move(name);
        entry.modified = static_cast<int64_t>(info.st_mtime);
        entry.duration_ms = WavDurationMs(entry.path, static_cast<long>(info.st_size));
        entries.push_back(std::move(entry));
    }
    closedir(directory);
    std::sort(entries.begin(), entries.end(),
              [](const VoiceMemoEntry& left, const VoiceMemoEntry& right) {
                  if (left.modified != right.modified)
                      return left.modified > right.modified;
                  return left.name > right.name;
              });
    return entries;
}
