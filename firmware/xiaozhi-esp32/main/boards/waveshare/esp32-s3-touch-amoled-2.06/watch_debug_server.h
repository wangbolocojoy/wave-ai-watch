#pragma once

/**
 * Read-only diagnostics over the local network.
 *
 *   GET /log      /sdcard/logs/system.log
 *   GET /log.1    /sdcard/logs/system.1.log
 *   GET /info     JSON snapshot: reset reason, uptime, heap/PSRAM, WiFi, music
 *
 * The server is started once WiFi is connected and listens on port 8080. It
 * only serves local diagnostics; it accepts no writes and no commands.
 */
#include <functional>
#include <string>

class WatchDebugServer {
public:
    static WatchDebugServer& GetInstance();

    void Start();

    // Remote inspection for development. The endpoints below require the
    // per-boot token printed on the serial console, so a LAN peer cannot use
    // them without local access.
    //   GET  /screen?t=<token>                 JPEG of the watch UI
    //   GET  /nav?t=<token>&page=<name>        open a watch page by name
    //   POST /upload?t=<token>&path=<Photos/x.jpg>   body = file bytes
    void SetScreenProvider(std::function<bool(std::string&)> provider) {
        screen_provider_ = std::move(provider);
    }
    void SetNavigateHandler(std::function<void(const std::string&)> handler) {
        navigate_handler_ = std::move(handler);
    }

    bool Snapshot(std::string& jpeg) { return screen_provider_ && screen_provider_(jpeg); }
    void Navigate(const std::string& page) {
        if (navigate_handler_)
            navigate_handler_(page);
    }

private:
    WatchDebugServer() = default;

    std::function<bool(std::string&)> screen_provider_;
    std::function<void(const std::string&)> navigate_handler_;
};
