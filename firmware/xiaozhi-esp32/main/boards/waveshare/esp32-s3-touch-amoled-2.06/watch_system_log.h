#pragma once

class WatchSystemLog {
public:
    static void Initialize();
    static void StorageReady();
    static void Event(const char* area, const char* action, const char* detail = nullptr);
};
