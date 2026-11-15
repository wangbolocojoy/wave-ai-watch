#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class WatchMusicPlayer {
public:
    static WatchMusicPlayer& GetInstance();

    bool Initialize();
    bool Rescan();
    bool IsScanning() const { return false; }
    bool IsLibraryTruncated() const { return false; }
    bool IsPlaying() const;
    std::string Status() const;
    bool SetShuffle(bool enabled);
    bool ToggleShuffle();
    bool ShuffleEnabled() const;
    bool shuffle_ = false;
    std::pair<uint32_t, uint32_t> Progress() const;
    size_t TrackCount() const;
    std::vector<std::pair<std::string, std::string>> Library(size_t offset = 0, size_t limit = 30) const;
    std::string CurrentTrackId() const;
    std::string PlayTrack(const std::string& track_id);
    std::string PlayOrResume();
    std::string Pause();
    std::string Next();
    std::string Previous();
    std::string Stop();
    void SetAssistantSessionActive(bool active);

private:
    WatchMusicPlayer() = default;
    void UpdateProgress() const;

    int current_ = 0;
    bool playing_ = false;
    bool paused_ = false;
    bool assistant_paused_ = false;
    mutable uint32_t position_ms_ = 0;
    mutable std::chrono::steady_clock::time_point last_tick_ = std::chrono::steady_clock::now();
};
