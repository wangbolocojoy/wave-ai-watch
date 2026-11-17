#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <esp_heap_caps.h>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct PsramCharAllocator {
    using value_type = char;
    PsramCharAllocator() = default;
    char* allocate(size_t n) {
        void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr)
            std::abort();
        return static_cast<char*>(p);
    }
    void deallocate(char* p, size_t) { heap_caps_free(p); }
    bool operator==(const PsramCharAllocator&) const { return true; }
    bool operator!=(const PsramCharAllocator&) const { return false; }
};
using PsramString = std::basic_string<char, std::char_traits<char>, PsramCharAllocator>;

class WatchMusicPlayer {
public:
    static WatchMusicPlayer& GetInstance();
    bool Initialize();
    bool Rescan();
    bool IsScanning() const { return scan_in_progress_.load(); }
    bool IsMounted() const;
    bool IsLibraryTruncated() const { return library_truncated_.load(); }
    bool IsPlaying() const { return playing_.load() && !paused_.load(); }
    std::string Search(const std::string& query);
    std::string Play(const std::string& query);
    std::string PlayOrResume();
    std::string PlayTrack(const std::string& track_id);
    std::string Next();
    std::string Previous();
    std::string Pause();
    std::string Resume();
    std::string Stop();
    bool SetShuffle(bool enabled);
    bool ToggleShuffle();
    bool ShuffleEnabled() const { return shuffle_.load(); }
    std::string Status() const;
    std::pair<uint32_t, uint32_t> Progress() const;
    size_t TrackCount() const;
    // Leaf name of the folder that was indexed (for example "音乐"), empty
    // when no music folder was found on the card.
    std::string LibraryRoot() const;
    std::vector<std::pair<std::string, std::string>> Library(size_t offset = 0,
                                                             size_t limit = 30) const;
    std::string CurrentTrackId() const;
    // Stable track id of one catalog entry, used by the lazy music list to
    // address rows that are not currently materialized.
    std::string TrackIdAt(size_t index) const;
    // Plays a single file that is not part of the library (voice memos).
    // Playback stops at the end of that file instead of advancing the catalog.
    bool PlayFilePath(const std::string& path);
    // Absolute path of what is playing right now, empty when stopped.
    std::string PlayingPath() const;
    void SetAssistantSessionActive(bool active);
    // Music needs the whole CPU: the always-on wake word / AFE costs enough to
    // starve the PCM queue (audible stutter). Toggled around playback.
    void SetMusicExclusiveAudio(bool exclusive);

private:
    struct TrackMetadata {
        std::string title;
        std::string artist;
        std::string album;
        uint32_t duration_ms = 0;
    };

    WatchMusicPlayer() = default;
    static void TaskEntry(void* context);
    static void ScanTaskEntry(void* context);
    void ScanLibrary();
    void Run();
    void ScanDirectory(const char* path, unsigned depth, std::vector<PsramString>& tracks,
                       std::unordered_map<std::string, TrackMetadata>& metadata,
                       bool& truncated) const;
    TrackMetadata ReadMetadata(const std::string& path) const;
    std::string DisplayNameLocked(const PsramString& path) const;
    std::string SearchTextLocked(const PsramString& path) const;
    bool PlayAudioFile(const std::string& path, uint32_t generation);
    void Select(int index);
    int RandomIndexLocked(int avoid) const;

    mutable std::mutex mutex_;
    std::vector<PsramString> tracks_;
    std::unordered_map<std::string, TrackMetadata> metadata_;
    std::string library_root_;
    std::string manual_path_;
    bool manual_playback_ = false;
    int current_ = -1;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t scan_task_ = nullptr;
    std::atomic<bool> scan_in_progress_{false};
    std::atomic<bool> scan_finished_{false};
    std::atomic<bool> library_truncated_{false};
    bool scan_started_ = false;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> paused_{false};
    std::atomic<bool> playing_{false};
    std::atomic<uint32_t> position_ms_{0};
    std::atomic<uint32_t> duration_ms_{0};
    std::atomic<bool> assistant_session_active_{false};
    std::atomic<bool> assistant_paused_music_{false};
    std::atomic<bool> shuffle_{false};
    bool bus_initialized_ = false;
    bool mounted_ = false;
};
