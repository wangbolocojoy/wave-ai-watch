#include "watch_debug_server.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <sys/stat.h>

#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "application.h"
#include "settings.h"
#include "watch_music_player.h"
#include "watch_system_log.h"
#include "wifi_manager.h"

#include <esp_random.h>
#include <cstdlib>

namespace {
constexpr char kTag[] = "WatchDebug";
constexpr int kPort = 8080;
constexpr char kLogPath[] = "/sdcard/logs/system.log";
constexpr char kPreviousLogPath[] = "/sdcard/logs/system.1.log";

httpd_handle_t server = nullptr;
char access_token[17] = {};

const char* ResetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

// Minimal JSON string escaping: SSIDs may legally contain quotes and backslashes.
std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char c : input) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

esp_err_t ServeFile(httpd_req_t* request, const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        httpd_resp_set_status(request, "404 Not Found");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        httpd_resp_sendstr(request, "log not found");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char buffer[512];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(request, buffer, read) != ESP_OK) {
            std::fclose(file);
            return ESP_FAIL;
        }
    }
    std::fclose(file);
    httpd_resp_send_chunk(request, nullptr, 0);
    return ESP_OK;
}

esp_err_t HandleLog(httpd_req_t* request) { return ServeFile(request, kLogPath); }
esp_err_t HandlePreviousLog(httpd_req_t* request) { return ServeFile(request, kPreviousLogPath); }

std::string UrlDecode(const char* input) {
    std::string out;
    for (const char* p = input; *p != '\0'; ++p) {
        if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = {p[1], p[2], 0};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            p += 2;
        } else if (*p == '+') {
            out.push_back(' ');
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

bool TokenMatches(httpd_req_t* request) {
    char query[96] = {};
    char token[20] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
        return false;
    if (httpd_query_key_value(query, "t", token, sizeof(token)) != ESP_OK)
        return false;
    return access_token[0] != '\0' && std::strcmp(token, access_token) == 0;
}

esp_err_t HandleScreen(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    auto& server_instance = WatchDebugServer::GetInstance();
    std::string jpeg;
    if (!server_instance.Snapshot(jpeg)) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no screen");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send(request, jpeg.data(), jpeg.size());
    return ESP_OK;
}

esp_err_t HandleNavigate(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    char query[128] = {};
    char page[48] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "page", page, sizeof(page)) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "page missing");
        return ESP_OK;
    }
    WatchDebugServer::GetInstance().Navigate(UrlDecode(page));
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, "ok");
    return ESP_OK;
}

esp_err_t HandleUpload(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    char query[160] = {};
    char relative[96] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", relative, sizeof(relative)) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "path missing");
        return ESP_OK;
    }
    if (std::strstr(relative, "..") != nullptr ||
        (std::strncmp(relative, "Photos/", 7) != 0 && std::strncmp(relative, "Video/", 6) != 0)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "only Photos/ or Video/");
        return ESP_OK;
    }
    const std::string full = std::string("/sdcard/") + relative;
    const size_t slash = full.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string directory = full.substr(0, slash);
        if (mkdir(directory.c_str(), 0775) != 0) {
            struct stat info = {};
            if (stat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
                httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
                return ESP_OK;
            }
        }
    }
    FILE* file = std::fopen(full.c_str(), "wb");
    if (file == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }
    char buffer[1024];
    int remaining = static_cast<int>(request->content_len);
    bool ok = remaining > 0 && remaining <= 8 * 1024 * 1024;
    while (ok && remaining > 0) {
        const int want = std::min<int>(remaining, sizeof(buffer));
        const int received = httpd_req_recv(request, buffer, want);
        if (received <= 0) {
            ok = false;
            break;
        }
        if (std::fwrite(buffer, 1, received, file) != static_cast<size_t>(received)) {
            ok = false;
            break;
        }
        remaining -= received;
    }
    std::fclose(file);
    if (!ok) {
        std::remove(full.c_str());
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, "ok");
    return ESP_OK;
}

esp_err_t HandleMem(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    multi_heap_info_t internal = {};
    multi_heap_info_t psram = {};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "internal: free=%u largest=%u min=%u allocated=%u\n"
                  "psram: free=%u largest=%u min=%u allocated=%u\n"
                  "tasks: %u",
                  static_cast<unsigned>(internal.total_free_bytes),
                  static_cast<unsigned>(internal.largest_free_block),
                  static_cast<unsigned>(internal.minimum_free_bytes),
                  static_cast<unsigned>(internal.total_allocated_bytes),
                  static_cast<unsigned>(psram.total_free_bytes),
                  static_cast<unsigned>(psram.largest_free_block),
                  static_cast<unsigned>(psram.minimum_free_bytes),
                  static_cast<unsigned>(psram.total_allocated_bytes),
                  static_cast<unsigned>(uxTaskGetNumberOfTasks()));
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, buffer);
    return ESP_OK;
}

esp_err_t HandleFile(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    char query[160] = {};
    char relative[96] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", relative, sizeof(relative)) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "path missing");
        return ESP_OK;
    }
    const std::string decoded = UrlDecode(relative);
    if (decoded.find("..") != std::string::npos) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    return ServeFile(request, (std::string("/sdcard/") + decoded).c_str());
}

esp_err_t HandleDelete(httpd_req_t* request) {
    if (!TokenMatches(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    char query[160] = {};
    char relative[96] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", relative, sizeof(relative)) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "path missing");
        return ESP_OK;
    }
    const std::string decoded = UrlDecode(relative);
    if (decoded.find("..") != std::string::npos ||
        (decoded.rfind("Photos/", 0) != 0 && decoded.rfind("Video/", 0) != 0)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "only Photos/ or Video/");
        return ESP_OK;
    }
    const std::string full = std::string("/sdcard/") + decoded;
    if (std::remove(full.c_str()) != 0) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, "ok");
    return ESP_OK;
}

esp_err_t HandleInfo(httpd_req_t* request) {
    auto& wifi = WifiManager::GetInstance();
    auto& music = WatchMusicPlayer::GetInstance();
    Settings diagnostics("sysdbg", true);
    const int boots = diagnostics.GetInt("boot_count", 0);
    const std::string ssid = JsonEscape(wifi.GetSsid());
    const std::string status = JsonEscape(music.Status());
    char json[640];
    std::snprintf(
        json, sizeof(json),
        "{\"version\":\"%s\",\"uptime_ms\":%llu,\"reset\":\"%s\",\"boots\":%d,"
        "\"state\":%d,\"heap_free\":%u,\"heap_min\":%u,\"psram_free\":%u,"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
        "\"music\":{\"tracks\":%u,\"status\":\"%s\"}}",
        esp_app_get_description()->version,
        static_cast<unsigned long long>(esp_timer_get_time() / 1000),
        ResetReasonText(esp_reset_reason()), boots,
        static_cast<int>(Application::GetInstance().GetDeviceState()),
        static_cast<unsigned>(esp_get_free_heap_size()),
        static_cast<unsigned>(esp_get_minimum_free_heap_size()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        wifi.IsConnected() ? "true" : "false", ssid.c_str(), wifi.GetIpAddress().c_str(),
        wifi.GetRssi(), static_cast<unsigned>(music.TrackCount()), status.c_str());
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_sendstr(request, json);
    return ESP_OK;
}
}  // namespace

WatchDebugServer& WatchDebugServer::GetInstance() {
    static WatchDebugServer instance;
    return instance;
}

void WatchDebugServer::Start() {
    if (server != nullptr)
        return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // The default 4 KB httpd stack is too small for the JPEG screenshot path
    // (Task Watchdog caught "stack overflow in task httpd").
    config.stack_size = 10240;
    config.server_port = kPort;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        ESP_LOGW(kTag, "Unable to start debug server on port %d", kPort);
        return;
    }
    httpd_uri_t log_uri = {
        .uri = "/log", .method = HTTP_GET, .handler = HandleLog, .user_ctx = nullptr};
    httpd_uri_t previous_log_uri = {
        .uri = "/log.1", .method = HTTP_GET, .handler = HandlePreviousLog, .user_ctx = nullptr};
    httpd_uri_t info_uri = {
        .uri = "/info", .method = HTTP_GET, .handler = HandleInfo, .user_ctx = nullptr};
    httpd_uri_t screen_uri = {
        .uri = "/screen", .method = HTTP_GET, .handler = HandleScreen, .user_ctx = nullptr};
    httpd_uri_t navigate_uri = {
        .uri = "/nav", .method = HTTP_GET, .handler = HandleNavigate, .user_ctx = nullptr};
    httpd_uri_t upload_uri = {
        .uri = "/upload", .method = HTTP_POST, .handler = HandleUpload, .user_ctx = nullptr};
    httpd_uri_t file_uri = {
        .uri = "/file", .method = HTTP_GET, .handler = HandleFile, .user_ctx = nullptr};
    httpd_uri_t mem_uri = {
        .uri = "/mem", .method = HTTP_GET, .handler = HandleMem, .user_ctx = nullptr};
    httpd_uri_t delete_uri = {
        .uri = "/delete", .method = HTTP_POST, .handler = HandleDelete, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &log_uri);
    httpd_register_uri_handler(server, &previous_log_uri);
    httpd_register_uri_handler(server, &info_uri);
    httpd_register_uri_handler(server, &screen_uri);
    httpd_register_uri_handler(server, &navigate_uri);
    httpd_register_uri_handler(server, &upload_uri);
    httpd_register_uri_handler(server, &file_uri);
    httpd_register_uri_handler(server, &mem_uri);
    httpd_register_uri_handler(server, &delete_uri);
    std::snprintf(access_token, sizeof(access_token), "%08x", static_cast<unsigned>(esp_random()));
    const std::string url = "http://" + WifiManager::GetInstance().GetIpAddress() + ":" +
                            std::to_string(kPort) + "/log";
    ESP_LOGI(kTag, "Debug server ready: %s  token=%s", url.c_str(), access_token);
    WatchSystemLog::Event("debug", "token", access_token);
    WatchSystemLog::Event("debug", "server_started", url.c_str());
}
