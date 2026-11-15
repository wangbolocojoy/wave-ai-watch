#pragma once

#include <map>
#include <string>

class Settings {
public:
    explicit Settings(const char* name, bool read_write = false);
    int GetInt(const char* key, int default_value = 0) const;
    bool GetBool(const char* key, bool default_value = false) const;
    std::string GetString(const char* key, const std::string& default_value = {}) const;
    void SetInt(const char* key, int value);
    void SetBool(const char* key, bool value);
    void SetString(const char* key, const std::string& value);

private:
    std::string namespace_;
};
