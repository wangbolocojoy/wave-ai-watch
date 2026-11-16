#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <new>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Decoded media lives in PSRAM: the LVGL heap and internal RAM are far too
// small for a full-screen RGB565 frame.
struct PsramByteAllocator {
    using value_type = uint8_t;
    template <typename U>
    struct rebind {
        using other = PsramByteAllocator;
    };
    PsramByteAllocator() = default;
    template <typename U>
    PsramByteAllocator(const PsramByteAllocator&) {}
    uint8_t* allocate(size_t n) {
        void* pointer = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pointer == nullptr)
            pointer = malloc(n);
        if (pointer == nullptr)
            abort();
        return static_cast<uint8_t*>(pointer);
    }
    void deallocate(uint8_t* pointer, size_t) { heap_caps_free(pointer); }
    bool operator==(const PsramByteAllocator&) const { return true; }
    bool operator!=(const PsramByteAllocator&) const { return false; }
};
using PsramBytes = std::vector<uint8_t, PsramByteAllocator>;

struct MediaFile {
    std::string path;
    std::string name;
    long size = 0;
};

/**
 * TF-card media helpers for the photo viewer and the MJPEG video player.
 *
 * Photos live in /sdcard/Photos (falling back to /sdcard/Pictures or
 * /sdcard/DCIM), videos in /sdcard/Video (falling back to /sdcard/Movies).
 * Everything is read-only: nothing is written to the card.
 */
namespace WatchMedia {

bool SdCardReady();
std::string PhotoDirectory();
std::string VideoDirectory();
std::vector<MediaFile> ListPhotos();
std::vector<MediaFile> ListVideos();

// Lists JPG/JPEG files inside one folder (used by the payment QR page).
std::vector<MediaFile> ListImagesIn(const std::string& directory);

// Decodes a JPEG into little-endian RGB565, downscaling until the image fits in
// `max_bytes`. Returns false when the file is unreadable or cannot be decoded.
bool DecodeJpegFile(const std::string& path, size_t max_bytes, PsramBytes& out, int& width,
                    int& height);

// Decodes an in-memory JPEG (e.g. artwork embedded in flash) into a 16-byte
// aligned PSRAM RGB565 buffer. The caller owns `out` and frees it with
// heap_caps_free().
bool DecodeJpegMemory(const uint8_t* data, size_t size, uint8_t** out, int& width, int& height);

/**
 * MJPEG-in-AVI player. Frames are decoded on a background task and handed to
 * the callback, which runs on that task and owns any LVGL locking it needs.
 */
class VideoPlayer {
public:
    using FrameCallback = std::function<void(const uint8_t* rgb565, int width, int height)>;

    VideoPlayer() = default;
    ~VideoPlayer();

    bool Play(const std::string& path, FrameCallback on_frame);
    void Stop();
    bool IsPlaying() const { return playing_.load(); }
    int Fps() const { return fps_; }

private:
    struct Job {
        VideoPlayer* player = nullptr;
        std::string path;
        FrameCallback on_frame;
    };
    static void TaskEntry(void* context);
    void Run(Job job);

    std::atomic_bool playing_{false};
    std::atomic_bool stop_requested_{false};
    TaskHandle_t task_ = nullptr;
    int fps_ = 15;
};

}  // namespace WatchMedia
