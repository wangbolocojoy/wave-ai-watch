#include "watch_config_service.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "settings.h"
#include "watch_system_log.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace {
constexpr char kTag[] = "WatchConfig";
constexpr int64_t kPairingLifetimeUs = 60LL * 1000 * 1000;
constexpr int64_t kTokenLifetimeUs = 30LL * 60 * 1000 * 1000;
constexpr size_t kRequestBodyLimit = 1024;

void SendJson(httpd_req_t* request, int status, const char* payload) {
    httpd_resp_set_status(request, status == 200 ? "200 OK" :
                                        status == 401 ? "401 Unauthorized" :
                                        status == 403 ? "403 Forbidden" :
                                        status == 413 ? "413 Payload Too Large" :
                                        status == 429 ? "429 Too Many Requests" :
                                        status == 503 ? "503 Service Unavailable" :
                                                        "400 Bad Request");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_sendstr(request, payload);
}

bool ReadInteger(cJSON* root, const char* key, int minimum, int maximum, int& value) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < minimum || item->valuedouble > maximum ||
        item->valuedouble != static_cast<double>(static_cast<int>(item->valuedouble)))
        return false;
    value = item->valueint;
    return true;
}
}  // namespace

void WatchConfigService::SetHandlers(ReadConfig read_config, ApplyConfig apply_config) {
    read_config_ = std::move(read_config);
    apply_config_ = std::move(apply_config);
}

void WatchConfigService::SetSnapshotHandler(Snapshot snapshot) {
    snapshot_ = std::move(snapshot);
}

void WatchConfigService::SetNavigateHandler(Navigate navigate) {
    navigate_ = std::move(navigate);
}

bool WatchConfigService::Start() {
    std::lock_guard<std::mutex> server_lock(server_mutex_);
    if (server_ != nullptr)
        return true;

    if (token_.empty()) {
        Settings preferences("watchcfg", true);
        const auto saved_token = preferences.GetString("token", "");
        if (saved_token.size() == 32) {
            token_ = saved_token;
            token_expires_us_ = esp_timer_get_time() + kTokenLifetimeUs;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32770;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 6;
    config.stack_size = 16384;
    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ESP_LOGW(kTag, "Unable to start the local settings service yet");
        WatchSystemLog::Event("config", "http_start_failed");
        return false;
    }

    httpd_uri_t pair_uri = {
        .uri = "/api/watch/pair",
        .method = HTTP_POST,
        .handler = PairHandler,
        .user_ctx = this,
    };
    httpd_uri_t config_get_uri = {
        .uri = "/api/watch/config",
        .method = HTTP_GET,
        .handler = ConfigHandler,
        .user_ctx = this,
    };
    httpd_uri_t config_put_uri = {
        .uri = "/api/watch/config",
        .method = HTTP_PUT,
        .handler = ConfigHandler,
        .user_ctx = this,
    };
    httpd_uri_t screen_uri = {
        .uri = "/api/watch/screen",
        .method = HTTP_GET,
        .handler = ScreenHandler,
        .user_ctx = this,
    };
    httpd_uri_t navigate_uri = {
        .uri = "/api/watch/navigate",
        .method = HTTP_POST,
        .handler = NavigateHandler,
        .user_ctx = this,
    };
    httpd_uri_t disconnect_uri = {
        .uri = "/api/watch/disconnect",
        .method = HTTP_POST,
        .handler = DisconnectHandler,
        .user_ctx = this,
    };
    if (httpd_register_uri_handler(server_, &pair_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &config_get_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &config_put_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &screen_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &navigate_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &disconnect_uri) != ESP_OK) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGE(kTag, "Unable to register settings endpoints");
        return false;
    }
    ESP_LOGI(kTag, "Local settings service started");
    WatchSystemLog::Event("config", "http_started");
    return true;
}

void WatchConfigService::Stop() {
    {
        std::lock_guard<std::mutex> server_lock(server_mutex_);
        if (server_ != nullptr) {
            httpd_stop(server_);
            server_ = nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pairing_code_.clear();
    token_.clear();
    pairing_expires_us_ = 0;
    token_expires_us_ = 0;
    pairing_failures_ = 0;
    pairing_open_ = false;
}

std::string WatchConfigService::BeginPairing() {
    const auto ip = LocalIp();
    if (ip.empty())
        return "请先连接 Wi-Fi";
    if (!Start())
        return "服务暂不可用";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pairing_code_.clear();
        pairing_open_ = true;
        pairing_expires_us_ = esp_timer_get_time() + kPairingLifetimeUs;
        pairing_failures_ = 0;
        token_.clear();
        token_expires_us_ = 0;
    }
    return ip + "  已开启配对，60 秒内电脑可直接连接";
}

std::string WatchConfigService::LocalIp() const {
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr)
        return {};
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
        return {};
    char address[16];
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
    return address;
}

std::string WatchConfigService::NewToken() const {
    char token[33];
    std::snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    return token;
}

esp_err_t WatchConfigService::PairHandler(httpd_req_t* request) {
    return static_cast<WatchConfigService*>(request->user_ctx)->Pair(request);
}

esp_err_t WatchConfigService::ConfigHandler(httpd_req_t* request) {
    return static_cast<WatchConfigService*>(request->user_ctx)->HandleConfig(request);
}

esp_err_t WatchConfigService::ScreenHandler(httpd_req_t* request) {
    return static_cast<WatchConfigService*>(request->user_ctx)->HandleScreen(request);
}

esp_err_t WatchConfigService::NavigateHandler(httpd_req_t* request) {
    return static_cast<WatchConfigService*>(request->user_ctx)->HandleNavigate(request);
}

esp_err_t WatchConfigService::DisconnectHandler(httpd_req_t* request) {
    return static_cast<WatchConfigService*>(request->user_ctx)->Disconnect(request);
}

bool WatchConfigService::ReadJsonBody(httpd_req_t* request, char* buffer, size_t capacity) {
    if (request->content_len == 0 || request->content_len >= capacity ||
        request->content_len > kRequestBodyLimit)
        return false;
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, buffer + received,
                                         request->content_len - received);
        if (count <= 0)
            return false;
        received += static_cast<size_t>(count);
    }
    buffer[received] = '\0';
    return true;
}

esp_err_t WatchConfigService::Pair(httpd_req_t* request) {
    char body[kRequestBodyLimit + 1];
    if (!ReadJsonBody(request, body, sizeof(body))) {
        SendJson(request, 413, "{\"error\":\"配对请求无效\"}");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body);
    cJSON* code = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "code");
    const int64_t now = esp_timer_get_time();
    bool paired = false;
    std::string issued_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool code_ok =
            !cJSON_IsString(code) || code->valuestring == nullptr ||
            code->valuestring[0] == '\0' || pairing_code_ == code->valuestring;
        if (pairing_failures_ < 5 && now < pairing_expires_us_ &&
            (pairing_open_ || code_ok)) {
            paired = true;
            pairing_open_ = false;
            pairing_code_.clear();
            pairing_expires_us_ = 0;
            issued_token = NewToken();
            token_ = issued_token;
            token_expires_us_ = now + kTokenLifetimeUs;
            Settings preferences("watchcfg", true);
            preferences.SetString("token", issued_token);
        } else if (pairing_failures_ < 5) {
            ++pairing_failures_;
        }
    }
    cJSON_Delete(root);
    if (!paired) {
        WatchSystemLog::Event("config", "pair_rejected");
        SendJson(request, 403, "{\"error\":\"配对码错误、过期或已失效\"}");
        return ESP_OK;
    }
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "token", issued_token.c_str());
    WatchSystemLog::Event("config", "paired");
    char* json = cJSON_PrintUnformatted(response);
    SendJson(request, 200, json == nullptr ? "{}" : json);
    cJSON_free(json);
    cJSON_Delete(response);
    return ESP_OK;
}

bool WatchConfigService::IsAuthorized(httpd_req_t* request) {
    const size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (length < 8 || length > 64)
        return false;
    char authorization[65];
    if (httpd_req_get_hdr_value_str(request, "Authorization", authorization,
                                    sizeof(authorization)) != ESP_OK ||
        std::strncmp(authorization, "Bearer ", 7) != 0)
        return false;
    const int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_.empty() || now >= token_expires_us_ || authorization + 7 != token_) {
        if (now >= token_expires_us_) {
            token_.clear();
            token_expires_us_ = 0;
        }
        WatchSystemLog::Event("config", "authorization_rejected", request->uri);
        return false;
    }
    token_expires_us_ = now + kTokenLifetimeUs;
    return true;
}

esp_err_t WatchConfigService::HandleConfig(httpd_req_t* request) {
    if (!IsAuthorized(request)) {
        SendJson(request, 401, "{\"error\":\"请先在手表上开启配对并重新连接\"}");
        return ESP_OK;
    }
    if (request->method == HTTP_GET) {
        if (!read_config_) {
            SendJson(request, 503, "{\"error\":\"配置读取暂不可用\"}");
            return ESP_OK;
        }
        const auto config = read_config_();
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "face", config.face.c_str());
        cJSON_AddNumberToObject(response, "brightness", config.brightness);
        cJSON_AddNumberToObject(response, "volume", config.volume);
        cJSON_AddBoolToObject(response, "doNotDisturb", config.do_not_disturb);
        char* json = cJSON_PrintUnformatted(response);
        SendJson(request, 200, json == nullptr ? "{}" : json);
        cJSON_free(json);
        cJSON_Delete(response);
        return ESP_OK;
    }

    char body[kRequestBodyLimit + 1];
    if (!ReadJsonBody(request, body, sizeof(body))) {
        SendJson(request, 413, "{\"error\":\"配置请求无效\"}");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body);
    Config next;
    cJSON* face = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "face");
    cJSON* dnd = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "doNotDisturb");
    const bool valid = cJSON_IsString(face) && face->valuestring != nullptr &&
                       (std::strcmp(face->valuestring, "luxe") == 0 ||
                        std::strcmp(face->valuestring, "sport") == 0 ||
                        std::strcmp(face->valuestring, "classic") == 0 ||
                        std::strcmp(face->valuestring, "cute") == 0) &&
                       ReadInteger(root, "brightness", 5, 100, next.brightness) &&
                       ReadInteger(root, "volume", 0, 100, next.volume) && cJSON_IsBool(dnd);
    if (valid) {
        next.face = face->valuestring;
        next.do_not_disturb = cJSON_IsTrue(dnd);
    }
    cJSON_Delete(root);
    if (!valid) {
        SendJson(request, 400, "{\"error\":\"配置字段无效\"}");
        return ESP_OK;
    }
    if (!apply_config_ || !apply_config_(next)) {
        WatchSystemLog::Event("config", "apply_failed");
        SendJson(request, 503, "{\"error\":\"手表无法应用这组配置\"}");
        return ESP_OK;
    }
    WatchSystemLog::Event("config", "settings_applied");
    SendJson(request, 200, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t WatchConfigService::HandleScreen(httpd_req_t* request) {
    if (!IsAuthorized(request)) {
        SendJson(request, 401, "{\"error\":\"请先在手表上开启配对并重新连接\"}");
        return ESP_OK;
    }
    if (!snapshot_) {
        SendJson(request, 503, "{\"error\":\"屏幕截图暂不可用\"}");
        return ESP_OK;
    }
    bool expected = false;
    if (!snapshot_busy_.compare_exchange_strong(expected, true)) {
        SendJson(request, 429, "{\"error\":\"截图正在生成\"}");
        return ESP_OK;
    }
    std::string jpeg;
    const bool captured = snapshot_(jpeg) && !jpeg.empty();
    snapshot_busy_.store(false);
    if (!captured) {
        WatchSystemLog::Event("config", "screen_capture_failed");
        SendJson(request, 503, "{\"error\":\"截取屏幕失败\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, jpeg.data(), jpeg.size());
}

esp_err_t WatchConfigService::HandleNavigate(httpd_req_t* request) {
    if (!IsAuthorized(request)) {
        SendJson(request, 401, "{\"error\":\"请先在手表上开启配对并重新连接\"}");
        return ESP_OK;
    }
    char body[kRequestBodyLimit + 1];
    if (!ReadJsonBody(request, body, sizeof(body))) {
        SendJson(request, 413, "{\"error\":\"页面请求无效\"}");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body);
    cJSON* page = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "page");
    std::string page_name;
    if (cJSON_IsString(page) && page->valuestring != nullptr)
        page_name = page->valuestring;
    const bool valid = page_name == "设置" || page_name == "应用" || page_name == "音乐" ||
                       page_name == "活动" || page_name == "倒计时" || page_name == "闹钟" || page_name == "小智" || page_name == "大肥鱼" ||
                       page_name == "表盘" || page_name == "主页" || page_name == "首页" ||
                       page_name == "AOD" || page_name == "唤醒" || page_name == "通知" || page_name == "播放音乐" ||
                       page_name == "语音备忘录" || page_name == "录音" ||
                       page_name == "测试通知";
    cJSON_Delete(root);
    if (!valid || !navigate_ || !navigate_(page_name)) {
        WatchSystemLog::Event("config", "navigate_failed", page_name.c_str());
        SendJson(request, 503, "{\"error\":\"无法打开这个页面\"}");
        return ESP_OK;
    }
    WatchSystemLog::Event("config", "navigated", page_name.c_str());
    SendJson(request, 200, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t WatchConfigService::Disconnect(httpd_req_t* request) {
    if (!IsAuthorized(request)) {
        SendJson(request, 401, "{\"error\":\"配对已过期\"}");
        return ESP_OK;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_.clear();
        token_expires_us_ = 0;
        pairing_open_ = false;
        Settings preferences("watchcfg", true);
        preferences.SetString("token", "");
    }
    WatchSystemLog::Event("config", "disconnected");
    SendJson(request, 200, "{\"ok\":true}");
    return ESP_OK;
}
