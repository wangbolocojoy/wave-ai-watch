#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL.h>
#include <lvgl.h>
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include "application.h"
#include "board.h"
#include "watch_activity_service.h"
#include "watch_alarm_service.h"
#include "watch_music_player.h"
#include "watch_system_log.h"
#include "wifi_manager.h"
#include "wifi_board.h"
#include "watch_timer_service.h"
#include "watch_display_ui.h"

namespace {

bool g_running = true;
CustomLcdDisplay* g_display = nullptr;

void PrintControls() {
    std::printf(
        "\nWAVE firmware UI host harness\n"
        "  1 / 2 / 3 / 4  switch face\n"
        "  m / a / t / l / p / s  open music / activity / timer / alarm / apps / settings\n"
        "  v          idle -> listening -> speaking -> idle\n"
        "  q          quit\n"
        "Click the watch with the mouse for normal UI interactions.\n\n");
}

int EventFilter(void* userdata, SDL_Event* event) {
    (void)userdata;
    if (event->type == SDL_QUIT) {
        g_running = false;
        return 0;
    }
    if (event->type != SDL_KEYDOWN || event->key.repeat) return 1;
    auto& app = Application::GetInstance();
    switch (event->key.keysym.sym) {
        case SDLK_1: if (g_display) g_display->SetWatchFace("鎏金雅韵"); break;
        case SDLK_2: if (g_display) g_display->SetWatchFace("活力运动"); break;
        case SDLK_3: if (g_display) g_display->SetWatchFace("经典指针"); break;
        case SDLK_m: if (g_display) g_display->OpenWatchApp("音乐"); break;
        case SDLK_a: if (g_display) g_display->OpenWatchApp("活动"); break;
        case SDLK_t: if (g_display) g_display->OpenWatchApp("计时器"); break;
        case SDLK_l: if (g_display) g_display->OpenWatchApp("闹钟"); break;
        case SDLK_p: if (g_display) g_display->OpenWatchApp("应用"); break;
        case SDLK_s: if (g_display) g_display->OpenWatchApp("设置"); break;
        case SDLK_v:
            if (app.State() == "idle") app.StartListening();
            else if (app.State() == "listening") app.SimulateSpeaking("你好，我在这里。");
            else app.StopAssistant();
            break;
        case SDLK_q: g_running = false; break;
        default: break;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string start_page;
    std::string start_face;
    std::string start_notify;
    bool start_assistant = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--page=", 0) == 0) start_page = arg.substr(7);
        else if (arg.rfind("--face=", 0) == 0) start_face = arg.substr(7);
        else if (arg.rfind("--notify=", 0) == 0) start_notify = arg.substr(9);
        else if (arg == "--assistant") start_assistant = true;
    }
    PrintControls();
    lv_init();

    lv_display_t* lv_display = lv_sdl_window_create(410, 502);
    if (lv_display == nullptr) {
        std::fprintf(stderr, "Unable to create SDL display\n");
        return 1;
    }
    lv_sdl_window_set_title(lv_display, "WAVE Watch · Firmware UI Host");
    lv_sdl_window_set_zoom(lv_display, 1.5f);
    lv_display_set_default(lv_display);

    lv_indev_t* mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, lv_display);

    static AudioCodec codec;
    static Backlight backlight;
    auto& board = Board::GetInstance();
    board.SetAudioCodec(&codec);
    board.SetBacklight(&backlight);
    board.SetBattery(87, false);

    auto* display = new CustomLcdDisplay(nullptr, nullptr, 410, 502, 0, 0, false, false, false);
    g_display = display;
    board.SetDisplay(display);

    static WatchActivityService activity;
    static WatchTimerService timer;
    static WatchAlarmService alarm;
    display->SetActivityService(&activity);
    display->SetTimerService(&timer);
    display->SetAlarmService(&alarm);
    display->SetConfigPairingHandler([] {
        return std::string("192.168.1.23  配对码 12345678（60 秒有效）");
    });

    auto& app = Application::GetInstance();
    app.SetDisplay(display);
    display->SetupUI();
    app.StopAssistant();

    if (!start_face.empty()) {
        if (start_face == "2" || start_face == "sport") display->SetWatchFace("活力运动");
        else if (start_face == "3" || start_face == "classic") display->SetWatchFace("经典指针");
        else if (start_face == "4" || start_face == "cute") display->SetWatchFace("软糖");
        else display->SetWatchFace("鎏金雅韵");
    }
    if (!start_notify.empty()) display->ShowNotification(start_notify.c_str());
    if (!start_page.empty()) {
        if (start_page == "music") display->OpenWatchApp("音乐");
        else if (start_page == "activity") display->OpenWatchApp("活动");
        else if (start_page == "timer") display->OpenWatchApp("计时器");
        else if (start_page == "alarm") display->OpenWatchApp("闹钟");
        else if (start_page == "apps") display->OpenWatchApp("应用");
        else if (start_page == "settings") display->OpenWatchApp("设置");
        else if (start_page == "notify") display->OpenWatchApp("通知");
    }
    if (start_assistant) app.StartListening();

    SDL_SetEventFilter(EventFilter, nullptr);

    while (g_running) {
        app.ProcessScheduled();
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > 10) wait_ms = 10;
        SDL_Delay(wait_ms);
    }

    SDL_SetEventFilter(nullptr, nullptr);
    return 0;
}
