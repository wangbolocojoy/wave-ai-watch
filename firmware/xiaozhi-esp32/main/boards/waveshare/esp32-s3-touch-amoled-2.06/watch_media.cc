#include "watch_media.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <esp_timer.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "jpeg_decoder.h"
#include "esp_jpeg_dec.h"

namespace {
constexpr char kTag[] = "WatchMedia";

#define RATE_CVT_CFG(src, dst)                                                   \
    (esp_ae_rate_cvt_cfg_t) {                                                    \
        .src_rate = (uint32_t)(src), .dest_rate = (uint32_t)(dst), .channel = 1, \
        .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2,                     \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED                             \
    }

// Bumped on every playback so queued video audio can be invalidated.
std::atomic<uint32_t> video_audio_generation_{0};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

bool IsDirectory(const std::string& path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool HasExtension(const std::string& path, const char* extension) {
    const auto lower = Lower(path);
    const size_t length = std::strlen(extension);
    return lower.size() > length && lower.compare(lower.size() - length, length, extension) == 0;
}

std::vector<MediaFile> ListDirectory(const std::string& directory, const char* extension,
                                     size_t limit) {
    std::vector<MediaFile> files;
    if (!IsDirectory(directory))
        return files;
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr)
        return files;
    while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] == '.')
            continue;
        MediaFile file;
        file.path = directory + "/" + entry->d_name;
        if (!HasExtension(file.path, extension))
            continue;
        struct stat info = {};
        if (stat(file.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        file.name = entry->d_name;
        file.size = static_cast<long>(info.st_size);
        files.push_back(std::move(file));
    }
    closedir(dir);
    std::sort(files.begin(), files.end(),
              [](const MediaFile& left, const MediaFile& right) { return left.name < right.name; });
    if (files.size() > limit)
        files.resize(limit);
    return files;
}

std::string FirstExistingDirectory(const std::vector<const char*>& candidates) {
    for (const char* candidate : candidates) {
        if (IsDirectory(candidate))
            return candidate;
    }
    return {};
}

uint32_t ReadLe32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

void* PsramAlloc(size_t size) {
    // esp_new_jpeg requires 16-byte aligned output buffers.
    void* pointer = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pointer == nullptr)
        pointer = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);
    return pointer;
}
}  // namespace

namespace WatchMedia {

bool SdCardReady() {
    return IsDirectory("/sdcard");
}

std::string PhotoDirectory() {
    return FirstExistingDirectory({"/sdcard/Photos", "/sdcard/Pictures", "/sdcard/DCIM"});
}

std::string VideoDirectory() {
    return FirstExistingDirectory({"/sdcard/Video", "/sdcard/Videos", "/sdcard/Movies"});
}

std::vector<MediaFile> ListPhotos() {
    std::vector<MediaFile> photos;
    const std::string root = PhotoDirectory();
    if (root.empty())
        return photos;
    for (const char* extension : {".jpg", ".jpeg"}) {
        auto batch = ListDirectory(root, extension, 200);
        photos.insert(photos.end(), batch.begin(), batch.end());
    }
    std::sort(photos.begin(), photos.end(),
              [](const MediaFile& left, const MediaFile& right) { return left.name < right.name; });
    return photos;
}

std::vector<MediaFile> ListVideos() {
    std::vector<MediaFile> videos;
    const std::string root = VideoDirectory();
    if (root.empty())
        return videos;
    for (const char* extension : {".avi", ".mjpg"}) {
        auto batch = ListDirectory(root, extension, 200);
        videos.insert(videos.end(), batch.begin(), batch.end());
    }
    std::sort(videos.begin(), videos.end(),
              [](const MediaFile& left, const MediaFile& right) { return left.name < right.name; });
    return videos;
}

namespace {
// JD_FASTDECODE=2 wants a 65 KB scratch buffer; keeping it in PSRAM avoids
// nearly exhausting internal SRAM, which only has ~66 KB free once MultiNet is
// loaded.
constexpr size_t kJpegWorkBufferSize = 65472;

uint8_t* JpegWorkBuffer() {
    static uint8_t* buffer = nullptr;
    if (buffer == nullptr)
        buffer = static_cast<uint8_t*>(PsramAlloc(kJpegWorkBufferSize));
    return buffer;
}

void ConfigureJpeg(esp_jpeg_image_cfg_t& config, const uint8_t* data, size_t size, uint8_t* work) {
    config = {};
    config.indata = const_cast<uint8_t*>(data);
    config.indata_size = static_cast<uint32_t>(size);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    // esp_jpeg's RGB565 conversion already writes low byte first (little-endian),
    // which is exactly what LVGL reads; enabling the swap produced garbled
    // colours (bright green/rainbow) for every decoded JPEG.
    config.flags.swap_color_bytes = 0;
    if (work != nullptr) {
        config.advanced.working_buffer = work;
        config.advanced.working_buffer_size = kJpegWorkBufferSize;
    }
}
}  // namespace

bool DecodeJpegMemory(const uint8_t* data, size_t size, uint8_t** out, int& width, int& height) {
    if (out == nullptr)
        return false;
    *out = nullptr;
    width = height = 0;
    if (data == nullptr || size == 0)
        return false;
    esp_jpeg_image_cfg_t config;
    ConfigureJpeg(config, data, size, JpegWorkBuffer());
    esp_jpeg_image_output_t info = {};
    config.out_scale = JPEG_IMAGE_SCALE_0;
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK || info.output_len == 0)
        return false;
    auto* pixels = static_cast<uint8_t*>(PsramAlloc(info.output_len));
    if (pixels == nullptr)
        return false;
    config.outbuf = pixels;
    config.outbuf_size = static_cast<uint32_t>(info.output_len);
    if (esp_jpeg_decode(&config, &info) != ESP_OK) {
        heap_caps_free(pixels);
        return false;
    }
    *out = pixels;
    width = info.width;
    height = info.height;
    return true;
}

bool DecodeJpegFile(const std::string& path, size_t max_bytes, PsramBytes& out, int& width,
                    int& height) {
    out.clear();
    width = height = 0;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long file_size = std::ftell(file);
    std::rewind(file);
    if (file_size <= 0 || file_size > 8 * 1024 * 1024) {
        std::fclose(file);
        ESP_LOGW(kTag, "JPEG too large or empty: %ld", file_size);
        return false;
    }
    auto* input = static_cast<uint8_t*>(PsramAlloc(static_cast<size_t>(file_size)));
    if (input == nullptr) {
        std::fclose(file);
        return false;
    }
    const size_t read = std::fread(input, 1, static_cast<size_t>(file_size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(file_size)) {
        free(input);
        return false;
    }

    esp_jpeg_image_cfg_t config;
    ConfigureJpeg(config, input, read, JpegWorkBuffer());
    esp_jpeg_image_output_t info = {};
    bool ok = false;
    for (const auto scale : {JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4,
                             JPEG_IMAGE_SCALE_1_8}) {
        config.out_scale = scale;
        if (esp_jpeg_get_image_info(&config, &info) != ESP_OK)
            continue;
        if (info.output_len > max_bytes)
            continue;
        out.resize(info.output_len);
        config.outbuf = out.data();
        config.outbuf_size = static_cast<uint32_t>(out.size());
        if (esp_jpeg_decode(&config, &info) == ESP_OK && info.output_len <= out.size()) {
            out.resize(info.output_len);
            width = info.width;
            height = info.height;
            ok = true;
        }
        break;
    }
    free(input);
    if (!ok)
        ESP_LOGW(kTag, "Unable to decode %s (limit %u bytes)", path.c_str(),
                 static_cast<unsigned>(max_bytes));
    return ok;
}

VideoPlayer::~VideoPlayer() {
    Stop();
    while (task_ != nullptr)
        vTaskDelay(pdMS_TO_TICKS(20));
}

bool VideoPlayer::Play(const std::string& path, FrameCallback on_frame) {
    if (playing_.load())
        return false;
    auto* job = new (std::nothrow) Job{this, path, std::move(on_frame)};
    if (job == nullptr)
        return false;
    stop_requested_.store(false);
    playing_.store(true);
    // Internal RAM is scarce once MultiNet is loaded; the decoder does not DMA,
    // so the task stack can live in PSRAM.
    if (xTaskCreateWithCaps(TaskEntry, "watch_video", 8192, job, 2, &task_,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        task_ = nullptr;
        playing_.store(false);
        delete job;
        return false;
    }
    return true;
}

void VideoPlayer::Stop() { stop_requested_.store(true); }

void VideoPlayer::TaskEntry(void* context) {
    auto* job = static_cast<Job*>(context);
    VideoPlayer* player = job->player;
    Job local{player, job->path, std::move(job->on_frame)};
    delete job;
    player->Run(std::move(local));
    player->task_ = nullptr;
    player->playing_.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void VideoPlayer::Run(Job job) {
    FILE* file = std::fopen(job.path.c_str(), "rb");
    if (file == nullptr) {
        playing_.store(false);
        return;
    }
    uint8_t riff[12] = {};
    if (std::fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "AVI ", 4) != 0) {
        ESP_LOGW(kTag, "Not an AVI file: %s", job.path.c_str());
        std::fclose(file);
        playing_.store(false);
        return;
    }

    int micro_per_frame = 0;
    long movi_offset = -1;
    // Audio stream discovered in the hdrl lists. The scan treats LIST chunks as
    // transparent so nested strl/strh/strf are seen without a recursive parser.
    int audio_channels = 0;
    int audio_sample_rate = 0;
    int audio_bits = 0;
    bool current_stream_is_audio = false;
    uint8_t chunk[8] = {};
    while (!stop_requested_.load() && std::fread(chunk, 1, sizeof(chunk), file) == sizeof(chunk)) {
        const uint32_t size = ReadLe32(chunk + 4);
        if (std::memcmp(chunk, "LIST", 4) == 0) {
            uint8_t type[4] = {};
            if (std::fread(type, 1, sizeof(type), file) != sizeof(type))
                break;
            if (std::memcmp(type, "movi", 4) == 0) {
                movi_offset = std::ftell(file);
                break;
            }
            continue;
        }
        if (std::memcmp(chunk, "avih", 4) == 0) {
            uint8_t header[40] = {};
            const size_t want = std::min<size_t>(size, sizeof(header));
            if (std::fread(header, 1, want, file) != want)
                break;
            micro_per_frame = static_cast<int>(ReadLe32(header));
            std::fseek(file, static_cast<long>(size - want), SEEK_CUR);
            continue;
        }
        if (std::memcmp(chunk, "strh", 4) == 0) {
            uint8_t stream_header[8] = {};
            const size_t want = std::min<size_t>(size, sizeof(stream_header));
            if (std::fread(stream_header, 1, want, file) != want)
                break;
            current_stream_is_audio = std::memcmp(stream_header, "auds", 4) == 0;
            std::fseek(file, static_cast<long>(size - want), SEEK_CUR);
            continue;
        }
        if (std::memcmp(chunk, "strf", 4) == 0 && current_stream_is_audio && size >= 16) {
            uint8_t format[18] = {};
            const size_t want = std::min<size_t>(size, sizeof(format));
            if (std::fread(format, 1, want, file) != want)
                break;
            const uint16_t format_tag = static_cast<uint16_t>(ReadLe32(format) & 0xFFFF);
            audio_channels = format[2] | (format[3] << 8);
            audio_sample_rate = static_cast<int>(ReadLe32(format + 4));
            audio_bits = format[14] | (format[15] << 8);
            if (format_tag != 1)  // only linear PCM is supported
                audio_channels = 0;
            std::fseek(file, static_cast<long>(size - want), SEEK_CUR);
            continue;
        }
        std::fseek(file, static_cast<long>(size) + (size & 1), SEEK_CUR);
    }

    fps_ = micro_per_frame > 0 ? static_cast<int>(1000000 / micro_per_frame) : 15;
    fps_ = std::clamp(fps_, 1, 30);
    if (movi_offset < 0) {
        ESP_LOGW(kTag, "No movi list in %s", job.path.c_str());
        std::fclose(file);
        playing_.store(false);
        return;
    }
    std::fseek(file, movi_offset, SEEK_SET);
    const int frame_delay = 1000 / fps_;
    ESP_LOGI(kTag, "Video %s at %d fps, audio %d Hz %d ch %d bit", job.path.c_str(), fps_,
             audio_sample_rate, audio_channels, audio_bits);

    auto& audio = Application::GetInstance().GetAudioService();
    const uint32_t audio_generation = ++video_audio_generation_;
    constexpr uint32_t kVideoAudioSource = 0x5649444F;  // 'VIDO'
    const bool has_audio = audio_channels > 0 && audio_sample_rate >= 8000 && audio_bits == 16;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    if (has_audio) {
        const int output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        if (audio_sample_rate != output_rate) {
            auto config = RATE_CVT_CFG(audio_sample_rate, output_rate);
            if (esp_ae_rate_cvt_open(&config, &resampler) != ESP_AE_ERR_OK) {
                ESP_LOGW(kTag, "Video audio resampler unavailable, playing silently");
            }
        }
        audio.SetPcmPlaybackSourceGeneration(kVideoAudioSource, audio_generation);
        audio.SetPcmPlaybackSourcePaused(kVideoAudioSource, false);
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
        ESP_LOGW(kTag, "Unable to open JPEG decoder");
        if (resampler != nullptr)
            esp_ae_rate_cvt_close(resampler);
        std::fclose(file);
        playing_.store(false);
        return;
    }

    std::vector<uint8_t> frame;
    std::vector<uint8_t> audio_chunk;
    std::vector<int16_t> audio_pcm;
    uint8_t* pixels = nullptr;
    size_t pixels_capacity = 0;
    int frame_width = 0;
    int frame_height = 0;
    int frame_index = 0;
    const int64_t start_us = esp_timer_get_time();
    while (!stop_requested_.load()) {
        if (std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk))
            break;
        const uint32_t size = ReadLe32(chunk + 4);
        const bool is_frame = chunk[0] == '0' && chunk[1] == '0' && chunk[2] == 'd' &&
                              chunk[3] >= 'b' && chunk[3] <= 'c';
        const bool is_audio = chunk[2] == 'w' && chunk[3] == 'b';
        if (!is_frame && !is_audio) {
            if (std::memcmp(chunk, "idx1", 4) == 0)
                break;
            std::fseek(file, static_cast<long>(size) + (size & 1), SEEK_CUR);
            continue;
        }
        if (is_audio) {
            if (!has_audio || size < 2 || size > 256 * 1024) {
                std::fseek(file, static_cast<long>(size) + (size & 1), SEEK_CUR);
                continue;
            }
            audio_chunk.resize(size);
            if (std::fread(audio_chunk.data(), 1, size, file) != size)
                break;
            if (size & 1)
                std::fseek(file, 1, SEEK_CUR);
            auto* samples = reinterpret_cast<const int16_t*>(audio_chunk.data());
            const size_t sample_count = size / 2;
            audio_pcm.resize(sample_count);
            if (audio_channels == 1) {
                std::memcpy(audio_pcm.data(), samples, size);
            } else {
                // Downmix to mono: the watch plays a single channel.
                const size_t frames = sample_count / static_cast<size_t>(audio_channels);
                audio_pcm.resize(frames);
                for (size_t i = 0; i < frames; ++i) {
                    int sum = 0;
                    for (int channel = 0; channel < audio_channels; ++channel)
                        sum += samples[i * static_cast<size_t>(audio_channels) + channel];
                    audio_pcm[i] = static_cast<int16_t>(sum / audio_channels);
                }
            }
            if (resampler != nullptr) {
                uint32_t capacity = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(
                    resampler, static_cast<uint32_t>(audio_pcm.size()), &capacity);
                if (capacity > 0) {
                    std::vector<int16_t> converted(capacity);
                    uint32_t actual = capacity;
                    if (esp_ae_rate_cvt_process(
                            resampler, static_cast<esp_ae_sample_t>(audio_pcm.data()),
                            static_cast<uint32_t>(audio_pcm.size()),
                            static_cast<esp_ae_sample_t>(converted.data()), &actual) == ESP_AE_ERR_OK &&
                        actual > 0) {
                        converted.resize(actual);
                        audio_pcm.swap(converted);
                    }
                }
            }
            // Blocking push paces the reader to real playback speed.
            audio.PushPcmFromSource(std::move(audio_pcm), kVideoAudioSource, audio_generation, true);
            continue;
        }
        if (size < 64 || size > 2 * 1024 * 1024) {
            std::fseek(file, static_cast<long>(size) + (size & 1), SEEK_CUR);
            continue;
        }
        frame.resize(size);
        if (std::fread(frame.data(), 1, size, file) != size)
            break;
        if (size & 1)
            std::fseek(file, 1, SEEK_CUR);

        jpeg_dec_io_t io = {};
        io.inbuf = frame.data();
        io.inbuf_len = size;
        jpeg_dec_header_info_t info = {};
        if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK)
            continue;
        int out_len = 0;
        if (jpeg_dec_get_outbuf_len(decoder, &out_len) != JPEG_ERR_OK || out_len <= 0)
            continue;
        int process_count = 0;
        jpeg_dec_get_process_count(decoder, &process_count);
        if (frame_width == 0) {
            ESP_LOGI(kTag, "Frame %ux%u outbuf=%d passes=%d src=%u", info.width, info.height,
                     out_len, process_count, static_cast<unsigned>(size));
        }
        if (pixels_capacity < static_cast<size_t>(out_len)) {
            if (pixels != nullptr)
                heap_caps_free(pixels);
            pixels = static_cast<uint8_t*>(PsramAlloc(static_cast<size_t>(out_len)));
            pixels_capacity = pixels == nullptr ? 0 : static_cast<size_t>(out_len);
        }
        if (pixels == nullptr)
            break;
        io.outbuf = pixels;
        io.out_size = out_len;
        for (int pass = 0; pass < process_count; ++pass) {
            if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
                io.outbuf = nullptr;
                break;
            }
        }
        if (io.outbuf == nullptr)
            continue;
        frame_width = static_cast<int>(info.width);
        frame_height = static_cast<int>(info.height);
        if (job.on_frame)
            job.on_frame(pixels, frame_width, frame_height);

        // Keep video on its own timeline; audio pushes above provide back-pressure.
        ++frame_index;
        const int64_t target_us = start_us + static_cast<int64_t>(frame_index) * 1000000 / fps_;
        const int64_t remaining_us = target_us - esp_timer_get_time();
        if (remaining_us > 1000)
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((remaining_us + 999) / 1000)));
    }
    if (has_audio) {
        audio.SetPcmPlaybackSourcePaused(kVideoAudioSource, true);
        audio.SetPcmPlaybackSourceGeneration(kVideoAudioSource, audio_generation + 1);
    }
    if (resampler != nullptr)
        esp_ae_rate_cvt_close(resampler);
    jpeg_dec_close(decoder);
    std::fclose(file);
    if (pixels != nullptr)
        heap_caps_free(pixels);
    ESP_LOGI(kTag, "Video stopped");
}

}  // namespace WatchMedia
