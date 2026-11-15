#pragma once

#include <esp_http_server.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class WatchConfigService {
public:
    struct Config {
        std::string face;
        int brightness = 70;
        int volume = 55;
        bool do_not_disturb = false;
    };

    using ReadConfig = std::function<Config()>;
    using ApplyConfig = std::function<bool(const Config&)>;
    using Snapshot = std::function<bool(std::string&)>;
    using Navigate = std::function<bool(const std::string&)>;

    void SetHandlers(ReadConfig read_config, ApplyConfig apply_config);
    void SetSnapshotHandler(Snapshot snapshot);
    void SetNavigateHandler(Navigate navigate);
    bool Start();
    void Stop();
    std::string BeginPairing();

private:
    static esp_err_t PairHandler(httpd_req_t* request);
    static esp_err_t ConfigHandler(httpd_req_t* request);
    static esp_err_t ScreenHandler(httpd_req_t* request);
    static esp_err_t NavigateHandler(httpd_req_t* request);
    static esp_err_t DisconnectHandler(httpd_req_t* request);
    esp_err_t Pair(httpd_req_t* request);
    esp_err_t HandleConfig(httpd_req_t* request);
    esp_err_t HandleScreen(httpd_req_t* request);
    esp_err_t HandleNavigate(httpd_req_t* request);
    esp_err_t Disconnect(httpd_req_t* request);
    bool IsAuthorized(httpd_req_t* request);
    bool ReadJsonBody(httpd_req_t* request, char* buffer, size_t capacity);
    std::string LocalIp() const;
    std::string NewToken() const;

    httpd_handle_t server_ = nullptr;
    ReadConfig read_config_;
    ApplyConfig apply_config_;
    Snapshot snapshot_;
    Navigate navigate_;
    std::mutex server_mutex_;
    std::mutex mutex_;
    std::string pairing_code_;
    bool pairing_open_ = false;
    std::string token_;
    int64_t pairing_expires_us_ = 0;
    int64_t token_expires_us_ = 0;
    uint8_t pairing_failures_ = 0;
    std::atomic<bool> snapshot_busy_{false};
};
