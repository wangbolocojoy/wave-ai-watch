#include "watch_music_player.h"

#include "application.h"
#include "audio/audio_service.h"
#include "board.h"
#include "boards/waveshare/esp32-s3-touch-amoled-2.06/watch_system_log.h"

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <sdmmc_cmd.h>

#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <new>

#define TAG "WatchMusic"
#define MUSIC_ROOT "/sdcard"
constexpr uint32_t kWatchMusicSource = 0x574D5553;
// The library is /sdcard/Music plus three nested levels
// (e.g. Music/歌手/专辑/CD1/歌.mp3). No other folder is ever indexed.
constexpr unsigned kMusicScanDepth = 4;
constexpr size_t kMaxIndexedTracks = 2000;
#define RATE_CVT_CFG(src, dst)                                                   \
    (esp_ae_rate_cvt_cfg_t) {                                                    \
        .src_rate = (uint32_t)(src), .dest_rate = (uint32_t)(dst), .channel = 1, \
        .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2,                     \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED                             \
    }

namespace {
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}
std::string Title(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(path.substr(slash == std::string::npos ? 0 : slash + 1));
}
std::string DisplayName(std::string_view path) {
    std::string name = Title(path);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name.resize(dot);
    return name;
}
uint32_t BigEndian32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
}
uint32_t SyncSafe32(const uint8_t* bytes) {
    return (uint32_t(bytes[0] & 0x7f) << 21) | (uint32_t(bytes[1] & 0x7f) << 14) |
           (uint32_t(bytes[2] & 0x7f) << 7) | uint32_t(bytes[3] & 0x7f);
}
uint32_t LittleEndian32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}
void AppendUtf8(std::string& result, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}
std::string DecodeId3Text(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 2)
        return {};
    const uint8_t encoding = bytes[0];
    std::string text;
    if (encoding == 3) {
        text.assign(reinterpret_cast<const char*>(bytes.data() + 1), bytes.size() - 1);
    } else if (encoding == 0) {
        for (size_t i = 1; i < bytes.size() && bytes[i] != 0; ++i)
            AppendUtf8(text, bytes[i]);
    } else if (encoding == 1 || encoding == 2) {
        size_t offset = 1;
        bool little_endian = false;
        if (encoding == 1 && bytes.size() >= 3) {
            if (bytes[1] == 0xff && bytes[2] == 0xfe) {
                little_endian = true;
                offset = 3;
            } else if (bytes[1] == 0xfe && bytes[2] == 0xff) {
                offset = 3;
            }
        }
        while (offset + 1 < bytes.size()) {
            uint16_t unit = little_endian
                                ? uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8)
                                : (uint16_t(bytes[offset]) << 8) | uint16_t(bytes[offset + 1]);
            offset += 2;
            if (unit == 0)
                break;
            uint32_t codepoint = unit;
            if (unit >= 0xd800 && unit <= 0xdbff && offset + 1 < bytes.size()) {
                const uint16_t low = little_endian
                                         ? uint16_t(bytes[offset]) |
                                               (uint16_t(bytes[offset + 1]) << 8)
                                         : (uint16_t(bytes[offset]) << 8) |
                                               uint16_t(bytes[offset + 1]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    offset += 2;
                }
            }
            AppendUtf8(text, codepoint);
        }
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ' || text.back() == '\r' ||
                             text.back() == '\n'))
        text.pop_back();
    return text;
}
uint32_t TrackHash(std::string_view path) {
    uint32_t hash = 2166136261u;
    for (const auto byte : path) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16777619u;
    }
    return hash;
}
std::string TrackId(std::string_view path) {
    char id[9];
    std::snprintf(id, sizeof(id), "%08x", static_cast<unsigned int>(TrackHash(path)));
    return id;
}
// The card holds exactly one music folder at its root. FAT lookups are
// case-insensitive, so "Music", "music" and "MUSIC" all resolve here.
constexpr const char* kMusicFolderName = "Music";
// Older cards used the Chinese folder name. It is renamed to Music on the
// first scan, so the card ends up with exactly one library folder.
constexpr const char* kLegacyMusicFolderName = "音乐";
// Resolves the music folder on the card, renaming a legacy folder when needed.
// Leaves `result` empty when the card has no music folder at all.
bool FindMusicFolder(std::string& result) {
    DIR* directory = opendir(MUSIC_ROOT);
    if (directory == nullptr)
        return false;
    const auto wanted = Lower(kMusicFolderName);
    const auto legacy = Lower(kLegacyMusicFolderName);
    std::string music_path;
    std::string legacy_path;
    std::string available;
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string full = std::string(MUSIC_ROOT) + "/" + entry->d_name;
        struct stat info = {};
        if (stat(full.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        if (available.size() < 160) {
            available += entry->d_name;
            available += "  ";
        }
        const auto name = Lower(entry->d_name);
        if (music_path.empty() && name == wanted)
            music_path = full;
        else if (legacy_path.empty() && name == legacy)
            legacy_path = full;
    }
    closedir(directory);
    if (music_path.empty() && !legacy_path.empty()) {
        const std::string target = std::string(MUSIC_ROOT) + "/" + kMusicFolderName;
        if (rename(legacy_path.c_str(), target.c_str()) == 0) {
            ESP_LOGI(TAG, "Renamed %s to %s", legacy_path.c_str(), target.c_str());
            music_path = target;
        } else {
            ESP_LOGW(TAG, "Rename %s to %s failed (%s); scanning the old name", legacy_path.c_str(),
                     target.c_str(), std::strerror(errno));
            music_path = std::move(legacy_path);
        }
    }
    if (music_path.empty()) {
        ESP_LOGW(TAG, "No %s folder at %s (folders: %s)", kMusicFolderName, MUSIC_ROOT,
                 available.c_str());
        return false;
    }
    result = std::move(music_path);
    return true;
}
}  // namespace

WatchMusicPlayer& WatchMusicPlayer::GetInstance() {
    static WatchMusicPlayer player;
    return player;
}

bool WatchMusicPlayer::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ == nullptr &&
        xTaskCreate(TaskEntry, "watch_music", 4096, this, 2, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start music task");
        return false;
    }
    if (!scan_started_) {
        scan_started_ = true;
        scan_finished_ = false;
        scan_in_progress_ = true;
        if (xTaskCreate(ScanTaskEntry, "watch_music_scan", 6144, this, 1, &scan_task_) != pdPASS) {
            scan_in_progress_ = false;
            scan_finished_ = true;
            ESP_LOGE(TAG, "Unable to start music library scan");
            WatchSystemLog::Event("music", "scan_task_failed");
        }
    }
    return task_ != nullptr;
}

bool WatchMusicPlayer::Rescan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scan_task_ != nullptr)
            return false;
        scan_started_ = false;
        scan_finished_ = false;
        scan_in_progress_ = false;
    }
    return Initialize();
}

void WatchMusicPlayer::ScanTaskEntry(void* context) {
    auto* player = static_cast<WatchMusicPlayer*>(context);
    player->ScanLibrary();
    {
        std::lock_guard<std::mutex> lock(player->mutex_);
        player->scan_task_ = nullptr;
    }
    vTaskDelete(nullptr);
}

void WatchMusicPlayer::ScanLibrary() {
    bool mounted = false;
    // The 2.06 board wires the TF slot to the SDMMC 1-bit peripheral:
    // CMD=GPIO1, CLK=GPIO2, D0=GPIO3 (the same pins the Waveshare BSP uses).
    // SPI mode with GPIO17 as CS is not wired on this revision.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_2;
    slot.cmd = GPIO_NUM_1;
    slot.d0 = GPIO_NUM_3;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    slot.flags = 0;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 8;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = nullptr;
    const auto error = esp_vfs_fat_sdmmc_mount(MUSIC_ROOT, &host, &slot, &mount, &card);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "No SD card mounted: %s", esp_err_to_name(error));
        WatchSystemLog::Event("storage", "mount_failed", esp_err_to_name(error));
    } else {
        mounted = true;
        bus_initialized_ = true;
        ESP_LOGI(TAG, "SD card mounted at %s", MUSIC_ROOT);
    }

    std::vector<PsramString> tracks;
    std::unordered_map<std::string, TrackMetadata> metadata;
    bool truncated = false;
    std::string library_root;
    if (mounted) {
        if (FindMusicFolder(library_root)) {
            ESP_LOGI(TAG, "Scanning music folder: %s", library_root.c_str());
            if (Lower(Title(library_root)) != Lower(kMusicFolderName))
                ESP_LOGW(TAG, "Legacy folder name in use; rename it to %s", kMusicFolderName);
            ScanDirectory(library_root.c_str(), kMusicScanDepth, tracks, metadata, truncated);
        } else {
            // Only /sdcard/Music is a valid library. Without it the list stays
            // empty: the card root and other folders are never walked, so
            // recordings, logs and backups cannot become songs.
            ESP_LOGW(TAG, "No %s folder; music library stays empty", kMusicFolderName);
        }
        std::sort(tracks.begin(), tracks.end());
    }
    const auto track_count = tracks.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mounted_ = mounted;
        tracks_ = std::move(tracks);
        metadata_ = std::move(metadata);
        library_root_ = library_root.empty() ? std::string() : Title(library_root);
    }
    scan_in_progress_ = false;
    scan_finished_ = true;
    library_truncated_ = truncated;
    if (mounted) {
        WatchSystemLog::StorageReady();
        WatchSystemLog::Event("storage", "mounted", "SDMMC");
    }
    char detail[96];
    const std::string expected_root = std::string(MUSIC_ROOT) + "/" + kMusicFolderName;
    const char* scanned = "no SD card";
    if (mounted)
        scanned = library_root.empty() ? expected_root.c_str() : library_root.c_str();
    std::snprintf(detail, sizeof(detail), "%lu tracks | %s%s",
                  static_cast<unsigned long>(track_count), scanned,
                  mounted && library_root.empty() ? " (missing)" : "");
    WatchSystemLog::Event("music", "scan_finished", detail);
    if (truncated) {
        ESP_LOGW(TAG, "Music index limit reached (%u); additional tracks were not indexed",
                 static_cast<unsigned>(kMaxIndexedTracks));
    }
    ESP_LOGI(TAG, "Music library scan finished: %lu indexed tracks%s",
             static_cast<unsigned long>(track_count), truncated ? "+" : "");
}

void WatchMusicPlayer::ScanDirectory(
    const char* path, unsigned depth, std::vector<PsramString>& tracks,
    std::unordered_map<std::string, TrackMetadata>& metadata, bool& truncated) const {
    if (!depth)
        return;
    DIR* directory = opendir(path);
    if (!directory)
        return;
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string full = std::string(path) + "/" + entry->d_name;
        struct stat info = {};
        if (stat(full.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            ScanDirectory(full.c_str(), depth - 1, tracks, metadata, truncated);
        } else {
            const auto ext = Lower(full);
            if ((ext.size() >= 4 && ext.substr(ext.size() - 4) == ".wav") ||
                (ext.size() >= 4 && ext.substr(ext.size() - 4) == ".mp3")) {
                // 900+ track libraries: reading ID3 tags for every file
                // exhausts the small internal DMA heap (sdmmc_cmd 0x101).
                // Filenames are enough for the watch list; metadata is lazy.
                if (tracks.size() < kMaxIndexedTracks)
                    tracks.emplace_back(full.c_str());
                else
                    truncated = true;
            }
        }
    }
    closedir(directory);
}

WatchMusicPlayer::TrackMetadata WatchMusicPlayer::ReadMetadata(const std::string& path) const {
    TrackMetadata metadata;
    metadata.title = Title(path);
    const auto dot = metadata.title.find_last_of('.');
    if (dot != std::string::npos)
        metadata.title.resize(dot);

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        return metadata;
    const auto extension = Lower(path);
    if (extension.size() >= 4 && extension.substr(extension.size() - 4) == ".mp3") {
        uint8_t header[10] = {};
        if (fread(header, 1, sizeof(header), file) == sizeof(header) &&
            std::memcmp(header, "ID3", 3) == 0 && header[3] >= 2 && header[3] <= 4) {
            const uint32_t tag_size = SyncSafe32(header + 6);
            const bool version_2 = header[3] == 2;
            const size_t frame_header_size = version_2 ? 6 : 10;
            const size_t id_size = version_2 ? 3 : 4;
            size_t remaining = tag_size;
            if ((header[5] & 0x40) != 0) {
                remaining = 0;  // Extended headers are uncommon; fall back to the file name.
            }
            while (remaining >= frame_header_size && remaining <= 1024 * 1024) {
                uint8_t frame_header[10] = {};
                if (fread(frame_header, 1, frame_header_size, file) != frame_header_size)
                    break;
                remaining -= frame_header_size;
                if (frame_header[0] == 0)
                    break;
                uint32_t frame_size = version_2
                                          ? (uint32_t(frame_header[3]) << 16) |
                                                (uint32_t(frame_header[4]) << 8) | frame_header[5]
                                          : (header[3] == 4 ? SyncSafe32(frame_header + 4)
                                                            : BigEndian32(frame_header + 4));
                if (frame_size > remaining)
                    break;
                const std::string frame_id(reinterpret_cast<char*>(frame_header), id_size);
                const bool title_frame = frame_id == "TIT2" || frame_id == "TT2";
                const bool artist_frame = frame_id == "TPE1" || frame_id == "TP1";
                const bool album_frame = frame_id == "TALB" || frame_id == "TAL";
                const bool duration_frame = frame_id == "TLEN" || frame_id == "TLE";
                if ((title_frame || artist_frame || album_frame || duration_frame) &&
                    frame_size > 0 && frame_size <= 2048) {
                    std::vector<uint8_t> text(frame_size);
                    if (fread(text.data(), 1, text.size(), file) != text.size())
                        break;
                    const auto value = DecodeId3Text(text);
                    if (title_frame && !value.empty())
                        metadata.title = value;
                    else if (artist_frame)
                        metadata.artist = value;
                    else if (album_frame)
                        metadata.album = value;
                    else if (duration_frame) {
                        char* end = nullptr;
                        const auto duration = std::strtoul(value.c_str(), &end, 10);
                        if (end != value.c_str() && duration <= UINT32_MAX)
                            metadata.duration_ms = static_cast<uint32_t>(duration);
                    }
                } else if (fseek(file, static_cast<long>(frame_size), SEEK_CUR) != 0) {
                    break;
                }
                remaining -= frame_size;
            }
        }
    } else if (extension.size() >= 4 && extension.substr(extension.size() - 4) == ".wav") {
        uint8_t riff[12] = {};
        if (fread(riff, 1, sizeof(riff), file) == sizeof(riff) &&
            std::memcmp(riff, "RIFF", 4) == 0 && std::memcmp(riff + 8, "WAVE", 4) == 0) {
            uint32_t byte_rate = 0;
            uint32_t data_size = 0;
            uint8_t chunk[8] = {};
            while (fread(chunk, 1, sizeof(chunk), file) == sizeof(chunk)) {
                const uint32_t chunk_size = LittleEndian32(chunk + 4);
                if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
                    uint8_t format[16] = {};
                    if (fread(format, 1, sizeof(format), file) != sizeof(format))
                        break;
                    byte_rate = LittleEndian32(format + 8);
                    if (fseek(file, static_cast<long>(chunk_size - sizeof(format)) + (chunk_size & 1),
                              SEEK_CUR) != 0)
                        break;
                } else if (std::memcmp(chunk, "data", 4) == 0) {
                    data_size = chunk_size;
                    break;
                } else if (fseek(file, static_cast<long>(chunk_size) + (chunk_size & 1), SEEK_CUR) !=
                           0) {
                    break;
                }
            }
            if (byte_rate > 0 && data_size > 0)
                metadata.duration_ms = static_cast<uint32_t>(
                    std::min<uint64_t>(uint64_t(data_size) * 1000 / byte_rate, UINT32_MAX));
        }
    }
    fclose(file);
    return metadata;
}

std::string WatchMusicPlayer::DisplayNameLocked(const PsramString& path) const {
    return DisplayName(std::string_view(path.c_str()));
}

std::string WatchMusicPlayer::SearchTextLocked(const PsramString& path) const {
    return std::string(path.c_str());
}

std::string WatchMusicPlayer::Search(const std::string& query) {
    if (!Initialize())
        return "未检测到 SD 卡。请插入 SD 卡后重试。";
    if (IsScanning())
        return "SD 卡音乐曲库正在扫描，请稍后再搜索。";
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_)
        return "未检测到 SD 卡。请插入 SD 卡后重试。";
    const auto needle = Lower(query);
    std::string result;
    size_t count = 0;
    for (const auto& path : tracks_) {
        if (!needle.empty() && Lower(SearchTextLocked(path)).find(needle) == std::string::npos)
            continue;
        if (count == 10)
            break;
        result += TrackId(path) + "  " + DisplayNameLocked(path) + "\n";
        ++count;
    }
    return count ? "SD 卡音乐（播放时请使用曲目编号）：\n" + result
                 : "没有找到匹配的 MP3/WAV 音乐。";
}

void WatchMusicPlayer::Select(int index) {
    uint32_t generation;
    std::string title;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tracks_.empty())
            return;
        current_ = (index % static_cast<int>(tracks_.size()) + static_cast<int>(tracks_.size())) %
                   static_cast<int>(tracks_.size());
        manual_playback_ = false;
        manual_path_.clear();
        title = DisplayNameLocked(tracks_[current_]);
        position_ms_ = 0;
        duration_ms_ = 0;
        // The user asked for immediate playback: do not defer music until the
        // assistant conversation ends.
        paused_ = false;
        assistant_paused_music_ = false;
        playing_ = true;
        generation = ++generation_;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetPcmPlaybackSourceGeneration(kWatchMusicSource, generation);
    audio.SetPcmPlaybackSourcePaused(kWatchMusicSource, false);
    WatchSystemLog::Event("music", "play", title.c_str());
    if (task_)
        xTaskNotifyGive(task_);
}

int WatchMusicPlayer::RandomIndexLocked(int avoid) const {
    const int count = static_cast<int>(tracks_.size());
    if (count <= 1)
        return 0;
    int index = static_cast<int>(esp_random() % static_cast<uint32_t>(count));
    if (index == avoid)
        index = (index + 1) % count;
    return index;
}

std::string WatchMusicPlayer::Play(const std::string& query) {
    if (!Initialize())
        return "SD 卡未就绪。";
    if (IsScanning())
        return "SD 卡音乐曲库正在扫描，请稍后再播放。";
    int found = -1;
    std::string found_name;
    std::vector<int> matches;
    const auto needle = Lower(query);
    if (needle.find("随机") != std::string::npos)
        shuffle_ = true;
    const bool generic = needle.empty() || needle.find("音乐") != std::string::npos ||
                         needle.find("听歌") != std::string::npos ||
                         needle.find("随便") != std::string::npos ||
                         needle.find("随机") != std::string::npos ||
                         needle.find("sd卡") != std::string::npos ||
                         needle.find("本地") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (needle.empty() || Lower(SearchTextLocked(tracks_[i])).find(needle) !=
                                      std::string::npos) {
                matches.push_back(static_cast<int>(i));
                if (needle.empty())
                    break;
            }
        }
        if (matches.size() == 1)
            found_name = DisplayNameLocked(tracks_[matches.front()]);
    }
    if (matches.size() == 1)
        found = matches.front();
    else if (matches.size() > 1) {
        std::string result = "找到多首匹配音乐，请先确认曲目编号：\n";
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < matches.size() && i < 10; ++i) {
            const auto& path = tracks_[matches[i]];
            result += TrackId(path) + "  " + DisplayNameLocked(path) + "\n";
        }
        return result;
    }
    if (found < 0 && generic) {
        size_t index = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tracks_.empty())
                return "SD 卡里还没有音乐。";
            index = shuffle_ ? static_cast<size_t>(RandomIndexLocked(-1)) : 0;
            found_name = DisplayNameLocked(tracks_[index]);
        }
        Select(static_cast<int>(index));
        Application::GetInstance().StopListening();
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        return std::string(shuffle_ ? "随机播放：" : "开始播放：") + found_name;
    }
    if (found < 0)
        return "没有找到匹配的 MP3/WAV 音乐。";
    Select(found);
    Application::GetInstance().StopListening();
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    return "开始播放：" + found_name;
}

std::string WatchMusicPlayer::PlayOrResume() {
    if (playing_)
        return Resume();
    return Play("");
}

std::string WatchMusicPlayer::PlayTrack(const std::string& track_id) {
    if (!Initialize())
        return "SD 卡未就绪。";
    if (IsScanning())
        return "SD 卡音乐曲库正在扫描，请稍后再播放。";
    int found = -1;
    std::string found_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (TrackId(tracks_[i]) != track_id)
                continue;
            if (found >= 0)
                break;
            found = static_cast<int>(i);
            found_name = DisplayNameLocked(tracks_[i]);
        }
    }
    if (found < 0)
        return "曲目编号已失效，请重新搜索 SD 卡音乐。";
    Select(found);
    Application::GetInstance().StopListening();
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    return "开始播放：" + found_name;
}

std::string WatchMusicPlayer::Next() {
    if (!Initialize())
        return "SD 卡未就绪。";
    if (IsScanning())
        return "SD 卡音乐曲库正在扫描，请稍后再操作。";
    int index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = shuffle_ ? RandomIndexLocked(current_) : current_ + 1;
    }
    Select(index);
    return Status();
}
std::string WatchMusicPlayer::Previous() {
    if (!Initialize())
        return "SD 卡未就绪。";
    if (IsScanning())
        return "SD 卡音乐曲库正在扫描，请稍后再操作。";
    int index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = current_ < 0 ? static_cast<int>(tracks_.size()) - 1 : current_ - 1;
    }
    Select(index);
    return Status();
}
std::string WatchMusicPlayer::Pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_)
            return "当前没有播放音乐。";
        paused_ = true;
        assistant_paused_music_ = false;
    }
    Application::GetInstance().GetAudioService().SetPcmPlaybackSourcePaused(kWatchMusicSource,
                                                                            true);
    WatchSystemLog::Event("music", "pause");
    return "音乐已暂停。";
}
std::string WatchMusicPlayer::Resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_)
            return "当前没有可继续的音乐。";
        paused_ = false;
        assistant_paused_music_ = false;
    }
    Application::GetInstance().GetAudioService().SetPcmPlaybackSourcePaused(kWatchMusicSource,
                                                                            false);
    WatchSystemLog::Event("music", "resume");
    Application::GetInstance().StopListening();
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    if (task_)
        xTaskNotifyGive(task_);
    return "继续播放。";
}
std::string WatchMusicPlayer::Stop() {
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        paused_ = false;
        assistant_paused_music_ = false;
        manual_playback_ = false;
        manual_path_.clear();
        position_ms_ = 0;
        duration_ms_ = 0;
        generation = ++generation_;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetPcmPlaybackSourceGeneration(kWatchMusicSource, generation);
    audio.SetPcmPlaybackSourcePaused(kWatchMusicSource, false);
    WatchSystemLog::Event("music", "stop");
    ESP_LOGW(TAG, "Stop() called");
    if (task_)
        xTaskNotifyGive(task_);
    return "音乐已停止。";
}
std::string WatchMusicPlayer::Status() const {
    if (IsScanning())
        return "正在扫描 SD 卡音乐...";
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_)
        return "当前没有播放音乐。";
    if (manual_playback_)
        return std::string(paused_ ? "已暂停：" : "正在播放：") + DisplayName(manual_path_);
    if (current_ < 0 || current_ >= static_cast<int>(tracks_.size()))
        return "当前没有播放音乐。";
    return std::string(paused_ ? "已暂停：" : "正在播放：") +
           DisplayNameLocked(tracks_[current_]);
}

bool WatchMusicPlayer::PlayFilePath(const std::string& path) {
    if (path.empty())
        return false;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_path_ = path;
        manual_playback_ = true;
        current_ = -1;
        position_ms_ = 0;
        duration_ms_ = 0;
        paused_ = false;
        assistant_paused_music_ = false;
        playing_ = true;
        generation = ++generation_;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetPcmPlaybackSourceGeneration(kWatchMusicSource, generation);
    audio.SetPcmPlaybackSourcePaused(kWatchMusicSource, false);
    WatchSystemLog::Event("memo", "playback_start", path.c_str());
    if (task_)
        xTaskNotifyGive(task_);
    return true;
}

std::string WatchMusicPlayer::PlayingPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_)
        return {};
    if (manual_playback_)
        return manual_path_;
    if (current_ < 0 || current_ >= static_cast<int>(tracks_.size()))
        return {};
    return std::string(tracks_[current_].c_str());
}

std::pair<uint32_t, uint32_t> WatchMusicPlayer::Progress() const {
    return {position_ms_.load(), duration_ms_.load()};
}

size_t WatchMusicPlayer::TrackCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_.size();
}

std::string WatchMusicPlayer::LibraryRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return library_root_;
}

bool WatchMusicPlayer::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

std::vector<std::pair<std::string, std::string>> WatchMusicPlayer::Library(size_t offset,
                                                                                 size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> library;
    if (offset >= tracks_.size())
        return library;
    const size_t count = std::min(tracks_.size() - offset, limit);
    library.reserve(count);
    for (size_t i = offset; i < offset + count; ++i) {
        const auto& path = tracks_[i];
        library.emplace_back(TrackId(path), DisplayNameLocked(path));
    }
    return library;
}

std::string WatchMusicPlayer::CurrentTrackId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ < 0 || current_ >= static_cast<int>(tracks_.size()))
        return {};
    return TrackId(tracks_[current_]);
}

std::string WatchMusicPlayer::TrackIdAt(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= tracks_.size())
        return {};
    return TrackId(tracks_[index]);
}

void WatchMusicPlayer::SetAssistantSessionActive(bool active) {
    if (active && (playing_.load() || paused_.load())) {
        // Wake word during playback must stop the song, not resume it after
        // the assistant conversation.
        Stop();
    }
    assistant_session_active_ = active;
}

bool WatchMusicPlayer::SetShuffle(bool enabled) {
    shuffle_ = enabled;
    return shuffle_.load();
}

bool WatchMusicPlayer::ToggleShuffle() {
    shuffle_ = !shuffle_.load();
    return shuffle_.load();
}

void WatchMusicPlayer::TaskEntry(void* context) { static_cast<WatchMusicPlayer*>(context)->Run(); }
void WatchMusicPlayer::Run() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!playing_ || paused_)
            continue;
        const auto generation = generation_.load();
        std::string path;
        bool manual;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            manual = manual_playback_;
            if (manual) {
                path = manual_path_;
            } else {
                if (current_ < 0 || current_ >= static_cast<int>(tracks_.size()))
                    continue;
                path = tracks_[current_].c_str();
            }
        }
        if (PlayAudioFile(path, generation) && generation == generation_.load()) {
            auto& audio = Application::GetInstance().GetAudioService();
            while (generation == generation_.load() && !audio.IsPlaybackIdle())
                vTaskDelay(pdMS_TO_TICKS(10));
            if (generation == generation_.load()) {
                if (manual) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    playing_ = false;
                    paused_ = false;
                    manual_playback_ = false;
                    manual_path_.clear();
                    position_ms_ = 0;
                    duration_ms_ = 0;
                    WatchSystemLog::Event("memo", "playback_finished");
                } else {
                    Next();
                }
            }
        } else if (generation == generation_.load() && !paused_) {
            playing_ = false;
            WatchSystemLog::Event("music", "playback_failed");
        }
    }
}

bool WatchMusicPlayer::PlayAudioFile(const std::string& path, uint32_t generation) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return false;
    long file_size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        file_size = ftell(file);
        rewind(file);
    }
    const auto extension = Lower(path);
    const auto decoder_type =
        extension.size() >= 4 && extension.substr(extension.size() - 4) == ".mp3"
            ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3
            : ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        fclose(file);
        return false;
    }
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        esp_audio_dec_unregister_default();
        fclose(file);
        return false;
    }
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = decoder_type,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto decode_result = esp_audio_simple_dec_open(&decoder_config, &decoder);
    if (decode_result != ESP_AUDIO_ERR_OK) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        fclose(file);
        ESP_LOGW(TAG, "Cannot open decoder for %s: %d", path.c_str(), decode_result);
        return false;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    const int output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    constexpr size_t input_capacity = 1024;
    std::vector<uint8_t> input(input_capacity);
    std::vector<uint8_t> decoded(12288);
    esp_audio_simple_dec_info_t info = {};
    bool have_audio_info = false;
    bool ok = true;
    bool reached_eof = false;
    size_t total_read = 0;
    while (generation == generation_.load() && playing_ && !reached_eof) {
        if (paused_) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        const size_t bytes_read = fread(input.data(), 1, input.size(), file);
        total_read += bytes_read;
        reached_eof = bytes_read < input.size() ||
                      (file_size >= 0 && total_read >= static_cast<size_t>(file_size));
        esp_audio_simple_dec_raw_t raw = {
            .buffer = input.data(),
            .len = static_cast<uint32_t>(bytes_read),
            .eos = reached_eof,
        };
        while (raw.len && generation == generation_.load() && playing_) {
            if (paused_) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            esp_audio_simple_dec_out_t frame = {
                .buffer = decoded.data(),
                .len = static_cast<uint32_t>(decoded.size()),
            };
            decode_result = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (decode_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (frame.needed_size <= decoded.size() || frame.needed_size > 32768) {
                    ok = false;
                    break;
                }
                decoded.resize(frame.needed_size);
                continue;
            }
            if (decode_result != ESP_AUDIO_ERR_OK || raw.consumed > raw.len ||
                frame.decoded_size > decoded.size()) {
                ok = false;
                break;
            }
            if (frame.decoded_size) {
                if (!have_audio_info) {
                    if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                        info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
                        info.sample_rate < 8000 || info.sample_rate > 48000) {
                        ok = false;
                        break;
                    }
                    if (info.sample_rate != static_cast<uint32_t>(output_rate)) {
                        auto config = RATE_CVT_CFG(info.sample_rate, output_rate);
                        if (esp_ae_rate_cvt_open(&config, &resampler) != ESP_AE_ERR_OK) {
                            ok = false;
                            break;
                        }
                    }
                    have_audio_info = true;
                    if (duration_ms_ == 0 && file_size > 0 && info.bitrate > 0) {
                        // esp_audio reports bitrate in kbps; convert file bytes
                        // to milliseconds without the old 1000x overestimate.
                        const auto estimate = static_cast<uint64_t>(file_size) * 8 / info.bitrate;
                        duration_ms_ = static_cast<uint32_t>(
                            std::min<uint64_t>(estimate, std::numeric_limits<uint32_t>::max()));
                    }
                }
                const size_t frame_bytes = info.channel * sizeof(int16_t);
                if (frame.decoded_size % frame_bytes != 0) {
                    ok = false;
                    break;
                }
                const auto* samples = reinterpret_cast<const int16_t*>(frame.buffer);
                const size_t frame_count = frame.decoded_size / frame_bytes;
                position_ms_ += static_cast<uint32_t>(frame_count * 1000 / info.sample_rate);
                std::vector<int16_t> mono(frame_count);
                if (info.channel == 1) {
                    std::memcpy(mono.data(), samples, frame.decoded_size);
                } else {
                    for (size_t i = 0; i < frame_count; ++i)
                        mono[i] = (int32_t(samples[i * 2]) + samples[i * 2 + 1]) / 2;
                }
                if (resampler) {
                    uint32_t capacity = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &capacity);
                    std::vector<int16_t> converted(capacity);
                    uint32_t actual = capacity;
                    if (esp_ae_rate_cvt_process(
                            resampler, reinterpret_cast<esp_ae_sample_t*>(mono.data()), mono.size(),
                            reinterpret_cast<esp_ae_sample_t*>(converted.data()),
                            &actual) != ESP_AE_ERR_OK) {
                        ok = false;
                        break;
                    }
                    converted.resize(actual);
                    if (!converted.empty() &&
                        !audio.PushPcmFromSource(std::move(converted), kWatchMusicSource,
                                                 generation)) {
                        ok = false;
                        break;
                    }
                } else if (!audio.PushPcmFromSource(std::move(mono), kWatchMusicSource,
                                                    generation)) {
                    ok = false;
                    break;
                }
            }
            if (raw.consumed == 0 && frame.decoded_size == 0) {
                ok = false;
                break;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
            // Keep the UI cooperative while decoding large MP3s.
            taskYIELD();
        }
        if (!ok)
            break;
    }
    if (resampler)
        esp_ae_rate_cvt_close(resampler);
    const bool file_ok = !ferror(file);
    if (duration_ms_ == 0 && position_ms_ > 0)
        duration_ms_ = position_ms_.load();
    esp_audio_simple_dec_close(decoder);
    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();
    fclose(file);
    return ok && file_ok && reached_eof && have_audio_info;
}
