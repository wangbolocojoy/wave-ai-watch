#include "watch_music_player.h"

#include <algorithm>

namespace {
struct DemoLock { int index; const char* id; const char* title; const char* artist; uint32_t duration_ms; };
constexpr DemoLock kTracks[] = {
    {0, "00000001", "清晨微光", "WAVE 原创曲库", 214000},
    {1, "00000002", "海风来信", "WAVE 原创曲库", 236000},
    {2, "00000003", "星夜漫步", "WAVE 原创曲库", 198000},
};
constexpr int kTrackCount = static_cast<int>(sizeof(kTracks) / sizeof(kTracks[0]));
}

WatchMusicPlayer& WatchMusicPlayer::GetInstance() {
    static WatchMusicPlayer player;
    return player;
}

bool WatchMusicPlayer::Initialize() {
    return true;
}
bool WatchMusicPlayer::Rescan() { return Initialize(); }


void WatchMusicPlayer::UpdateProgress() const {
    if (!playing_ || paused_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_).count();
    position_ms_ += static_cast<uint32_t>(std::max<int64_t>(0, delta));
    last_tick_ = now;
}

std::pair<uint32_t, uint32_t> WatchMusicPlayer::Progress() const {
    UpdateProgress();
    return {position_ms_, kTracks[std::clamp(current_, 0, kTrackCount - 1)].duration_ms};
}

std::string WatchMusicPlayer::Status() const {
    const auto& track = kTracks[std::clamp(current_, 0, kTrackCount - 1)];
    if (playing_ && !paused_) return std::string("正在播放：") + track.artist + " - " + track.title;
    if (playing_ && paused_) return std::string("已暂停：") + track.title;
    return "当前没有播放音乐。";
}

size_t WatchMusicPlayer::TrackCount() const { return kTrackCount; }

std::vector<std::pair<std::string, std::string>> WatchMusicPlayer::Library(size_t offset,
                                                                                 size_t limit) const {
    std::vector<std::pair<std::string, std::string>> library;
    for (int i = static_cast<int>(offset); i < kTrackCount && library.size() < limit; ++i) {
        const auto& track = kTracks[i];
        library.emplace_back(track.id, std::string(track.artist) + " - " + track.title);
    }
    return library;
}

std::string WatchMusicPlayer::CurrentTrackId() const {
    return kTracks[std::clamp(current_, 0, kTrackCount - 1)].id;
}

std::string WatchMusicPlayer::PlayTrack(const std::string& track_id) {
    for (int i = 0; i < kTrackCount; ++i) {
        if (track_id == kTracks[i].id) {
            current_ = i;
            playing_ = true;
            paused_ = false;
            assistant_paused_ = false;
            position_ms_ = 0;
            last_tick_ = std::chrono::steady_clock::now();
            return std::string("开始播放：") + kTracks[i].title;
        }
    }
    return "曲目编号已失效。";
}

std::string WatchMusicPlayer::PlayOrResume() {
    if (playing_ && paused_) {
        paused_ = false;
        assistant_paused_ = false;
        last_tick_ = std::chrono::steady_clock::now();
        return std::string("继续播放：") + kTracks[current_].title;
    }
    if (!playing_) return PlayTrack(kTracks[current_].id);
    return Status();
}

std::string WatchMusicPlayer::Pause() {
    if (!playing_) return "当前没有播放音乐。";
    paused_ = true;
    assistant_paused_ = false;
    return std::string("已暂停：") + kTracks[current_].title;
}

std::string WatchMusicPlayer::Next() {
    current_ = (current_ + 1) % kTrackCount;
    playing_ = true;
    paused_ = false;
    assistant_paused_ = false;
    position_ms_ = 0;
    last_tick_ = std::chrono::steady_clock::now();
    return std::string("下一首：") + kTracks[current_].title;
}

std::string WatchMusicPlayer::Previous() {
    current_ = (current_ - 1 + kTrackCount) % kTrackCount;
    playing_ = true;
    paused_ = false;
    assistant_paused_ = false;
    position_ms_ = 0;
    last_tick_ = std::chrono::steady_clock::now();
    return std::string("上一首：") + kTracks[current_].title;
}

std::string WatchMusicPlayer::Stop() {
    playing_ = false;
    paused_ = false;
    assistant_paused_ = false;
    position_ms_ = 0;
    return "音乐已停止。";
}

void WatchMusicPlayer::SetAssistantSessionActive(bool active) {
    if (active && playing_ && !paused_) {
        paused_ = true;
        assistant_paused_ = true;
    } else if (!active && assistant_paused_) {
        paused_ = false;
        assistant_paused_ = false;
    }
}

bool WatchMusicPlayer::SetShuffle(bool enabled) { shuffle_ = enabled; return shuffle_; }
bool WatchMusicPlayer::ToggleShuffle() { shuffle_ = !shuffle_; return shuffle_; }
bool WatchMusicPlayer::ShuffleEnabled() const { return shuffle_; }

bool WatchMusicPlayer::IsPlaying() const { return playing_ && !paused_; }
