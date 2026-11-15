#include "settings.h"

#include <cstdlib>
#include <map>
#include <mutex>

namespace {
std::mutex g_settings_mutex;
std::map<std::string, std::map<std::string, std::string>>& Store() {
    static std::map<std::string, std::map<std::string, std::string>> store;
    return store;
}
}

Settings::Settings(const char* name, bool read_write) : namespace_(name == nullptr ? "" : name) {
    (void)read_write;
}

int Settings::GetInt(const char* key, int default_value) const {
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    auto ns = Store().find(namespace_);
    if (ns == Store().end()) return default_value;
    auto it = ns->second.find(key == nullptr ? "" : key);
    if (it == ns->second.end()) return default_value;
    return std::atoi(it->second.c_str());
}

bool Settings::GetBool(const char* key, bool default_value) const {
    return GetInt(key, default_value ? 1 : 0) != 0;
}

std::string Settings::GetString(const char* key, const std::string& default_value) const {
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    auto ns = Store().find(namespace_);
    if (ns == Store().end()) return default_value;
    auto it = ns->second.find(key == nullptr ? "" : key);
    return it == ns->second.end() ? default_value : it->second;
}

void Settings::SetInt(const char* key, int value) {
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    Store()[namespace_][key == nullptr ? "" : key] = std::to_string(value);
}

void Settings::SetBool(const char* key, bool value) {
    SetInt(key, value ? 1 : 0);
}

void Settings::SetString(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    Store()[namespace_][key == nullptr ? "" : key] = value;
}
