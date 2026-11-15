#pragma once
#include <string>
#include <vector>
struct SsidItem { std::string ssid; std::string password; uint8_t channel = 0; };
class SsidManager {
public:
    static SsidManager& GetInstance() { static SsidManager inst; return inst; }
    const std::vector<SsidItem>& GetSsidList() const { return list_; }
    void SetDefaultSsid(int index) { if (index > 0 && index < (int)list_.size()) std::swap(list_[0], list_[index]); }
private:
    std::vector<SsidItem> list_ = {{"Home-WiFi", "password", 6}};
};
class WifiManager {
public:
    static WifiManager& GetInstance() { static WifiManager inst; return inst; }
    bool IsConnected() const { return true; }
    std::string GetSsid() const { return "Home-WiFi"; }
    std::string GetIpAddress() const { return "192.168.1.100"; }
    int GetRssi() const { return -48; }
    std::string GetApSsid() const { return "Xiaozhi-TEST"; }
    void StopStation() {}
    void StartStation() {}
};
