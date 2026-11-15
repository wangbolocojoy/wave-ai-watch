#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "watch_ui.h"

#define FACE_COUNT 3

static const lv_color_t ink = LV_COLOR_MAKE(8, 13, 22);
static const lv_color_t panel = LV_COLOR_MAKE(19, 27, 39);
static const lv_color_t muted = LV_COLOR_MAKE(139, 152, 171);
static const lv_color_t white = LV_COLOR_MAKE(244, 247, 252);
static const lv_color_t cyan = LV_COLOR_MAKE(80, 222, 220);
static const lv_color_t lime = LV_COLOR_MAKE(188, 239, 130);

static const char *face_names[FACE_COUNT] = {"鎏金", "运动", "经典"};
static lv_obj_t *screen;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static uint8_t current_face;
static bool showing_home;
static bool gesture_registered;
static bool dnd_enabled;
static int brightness_percent = 72;
static int volume_percent = 55;
static lv_obj_t *brightness_slider;
static lv_obj_t *volume_slider;
static lv_obj_t *analog_hour_hand;
static lv_obj_t *analog_minute_hand;
static lv_point_precise_t analog_hour_points[2];
static lv_point_precise_t analog_minute_points[2];
static lv_obj_t *assistant_pupils[2];
static lv_timer_t *assistant_eyes_timer;
static uint8_t assistant_gaze_frame;
static const char *demo_tracks[] = {"清晨微光", "海风来信", "星夜漫步"};
static const char *demo_artists[] = {"WAVE 原创曲库", "WAVE 原创曲库", "WAVE 原创曲库"};
static lv_obj_t *music_track_label;
static lv_obj_t *music_status_label;
static lv_obj_t *music_toggle_icon;
static lv_obj_t *music_rows[3];
static uint8_t music_track_index;
static bool music_playing;
static const lv_color_t gold = LV_COLOR_MAKE(224, 190, 116);
static const lv_color_t paper = LV_COLOR_MAKE(241, 233, 213);
static const lv_color_t forest = LV_COLOR_MAKE(30, 65, 54);

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width,
                       int height, int font_size, lv_color_t color)
{
    bool contains_cjk = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) { contains_cjk = true; break; }
    }
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_text_font(obj, contains_cjk ? &lv_font_source_han_sans_sc_14_cjk :
                              font_size >= 36 ? &lv_font_montserrat_48 :
                              font_size >= 20 ? &lv_font_montserrat_24 :
                                                &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_style_text_letter_space(obj, font_size >= 36 ? 1 : 0, 0);
    return obj;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h,
                      lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 22, 0);
    return obj;
}

static void clear_screen(void)
{
    if (screen == NULL) screen = lv_obj_create(NULL);
    else lv_obj_clean(screen);
    clock_label = NULL;
    date_label = NULL;
    analog_hour_hand = NULL;
    analog_minute_hand = NULL;
    assistant_pupils[0] = NULL;
    assistant_pupils[1] = NULL;
    music_track_label = NULL;
    music_status_label = NULL;
    music_toggle_icon = NULL;
    for (int i = 0; i < 3; i++) music_rows[i] = NULL;
    if (assistant_eyes_timer != NULL) {
        lv_timer_delete(assistant_eyes_timer);
        assistant_eyes_timer = NULL;
    }
    lv_obj_set_style_bg_color(screen, ink, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);
}

static void update_clock(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char time_text[8];
    char date_text[32];
    strftime(time_text, sizeof(time_text), "%H:%M", &local);
    static const char *weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    snprintf(date_text, sizeof(date_text), "%d月%d日 周%s", local.tm_mon + 1,
             local.tm_mday, weekdays[local.tm_wday]);
    if (clock_label != NULL) lv_label_set_text(clock_label, time_text);
    if (date_label != NULL) lv_label_set_text(date_label, date_text);
    if (analog_hour_hand != NULL && analog_minute_hand != NULL) {
        double minute_angle = ((double)local.tm_min + local.tm_sec / 60.0) * (M_PI / 30.0) - M_PI / 2.0;
        double hour_angle = ((double)(local.tm_hour % 12) + local.tm_min / 60.0) * (M_PI / 6.0) - M_PI / 2.0;
        analog_hour_points[0] = (lv_point_precise_t){129, 129};
        analog_hour_points[1] = (lv_point_precise_t){129 + (lv_value_precise_t)(cos(hour_angle) * 48),
                                                     129 + (lv_value_precise_t)(sin(hour_angle) * 48)};
        analog_minute_points[0] = (lv_point_precise_t){129, 129};
        analog_minute_points[1] = (lv_point_precise_t){129 + (lv_value_precise_t)(cos(minute_angle) * 76),
                                                       129 + (lv_value_precise_t)(sin(minute_angle) * 76)};
        lv_line_set_points(analog_hour_hand, analog_hour_points, 2);
        lv_line_set_points(analog_minute_hand, analog_minute_points, 2);
    }
}

static void show_watch_face(void);

static void cycle_face(lv_event_t *event)
{
    (void)event;
    current_face = (current_face + 1) % FACE_COUNT;
    show_watch_face();
}

static void face_gesture(lv_event_t *event)
{
    (void)event;
    if (!showing_home) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT) current_face = (current_face + 1) % FACE_COUNT;
    else if (direction == LV_DIR_RIGHT) current_face = (current_face + FACE_COUNT - 1) % FACE_COUNT;
    else return;
    show_watch_face();
}

static void show_detail(const char *title, const char *headline,
                        const char *subline);
static void show_tools(void);
static void show_assistant(void);
static void show_music(void);

static void back_to_home(lv_event_t *event)
{
    (void)event;
    show_watch_face();
}

static void app_clicked(lv_event_t *event)
{
    const char *name = lv_event_get_user_data(event);
    if (name == NULL) return;
    if (strcmp(name, "步数") == 0) {
        show_detail("今日活动", "6,842 步", "目标 8,000 步  ·  今日 +12%");
    } else if (strcmp(name, "音乐") == 0) {
        show_music();
    } else {
        show_tools();
    }
}

static void assistant_clicked(lv_event_t *event)
{
    (void)event;
    show_assistant();
}

static void back_button(void)
{
    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_pos(back, 22, 22);
    lv_obj_set_size(back, 46, 42);
    lv_obj_set_style_bg_color(back, panel, 0);
    lv_obj_set_style_radius(back, 15, 0);
    lv_obj_add_event_cb(back, back_to_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    lv_obj_set_style_text_color(arrow, white, 0);
}

static void slider_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    lv_obj_t *value_label = lv_event_get_user_data(event);
    int value_number = (int)lv_slider_get_value(slider);
    if (slider == brightness_slider) brightness_percent = value_number;
    else if (slider == volume_slider) volume_percent = value_number;
    char value[8];
    snprintf(value, sizeof(value), "%d%%", value_number);
    lv_label_set_text(value_label, value);
}

static void dnd_changed(lv_event_t *event)
{
    dnd_enabled = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
}

static void animate_assistant_eyes(lv_timer_t *timer)
{
    (void)timer;
    if (assistant_pupils[0] == NULL || assistant_pupils[1] == NULL) return;
    static const int gaze_x[] = {32, 48, 40, 22, 36, 48, 32, 22};
    static const int gaze_y[] = {27, 31, 39, 31, 25, 35, 39, 29};
    int frame = assistant_gaze_frame++ % 8;
    for (int i = 0; i < 2; i++) {
        lv_obj_set_pos(assistant_pupils[i], gaze_x[frame], gaze_y[frame]);
        if (frame == 6) lv_obj_set_size(assistant_pupils[i], 48, 7);
        else lv_obj_set_size(assistant_pupils[i], 48, 54);
    }
}

static void show_assistant(void)
{
    showing_home = false;
    clear_screen();
    back_button();
    label(screen, "小智语音助手", 82, 29, 270, 30, 20, white);

    lv_obj_t *eye_pair = card(screen, 34, 96, 342, 190, lv_color_hex(0x153244));
    lv_obj_set_style_radius(eye_pair, 42, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = lv_obj_create(eye_pair);
        lv_obj_remove_style_all(eye);
        lv_obj_set_size(eye, 126, 128);
        lv_obj_set_pos(eye, 28 + i * 160, 27);
        lv_obj_set_style_radius(eye, 46, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0xe8fbff), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_t *pupil = lv_obj_create(eye);
        lv_obj_remove_style_all(pupil);
        lv_obj_set_size(pupil, 48, 54);
        lv_obj_set_pos(pupil, 32, 27);
        lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pupil, lv_color_hex(0x102c44), 0);
        lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
        lv_obj_t *shine = lv_obj_create(pupil);
        lv_obj_remove_style_all(shine);
        lv_obj_set_size(shine, 14, 14);
        lv_obj_set_pos(shine, 9, 8);
        lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(shine, white, 0);
        lv_obj_set_style_bg_opa(shine, LV_OPA_COVER, 0);
        assistant_pupils[i] = pupil;
    }
    lv_obj_t *mood = label(screen, "正在聆听", 34, 310, 342, 30, 20, cyan);
    lv_obj_set_style_text_align(mood, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(mood, &lv_font_source_han_sans_sc_14_cjk, 0);
    assistant_gaze_frame = 0;
    assistant_eyes_timer = lv_timer_create(animate_assistant_eyes, 650, NULL);

    lv_obj_t *connection = card(screen, 22, 356, 366, 54, panel);
    label(connection, "尚未连接 · 请先在设置中完成账号配置", 12, 16, 342, 22, 14, muted);
    label(screen, "眼神动画预览；接入小智后将随对话状态变化。", 22, 414,
          366, 22, 14, muted);
}

static void update_music(void)
{
    if (music_track_label == NULL || music_status_label == NULL) return;
    lv_label_set_text(music_track_label, demo_tracks[music_track_index]);
    lv_label_set_text(music_status_label, music_playing ? "模拟播放中" : "已暂停");
    if (music_toggle_icon != NULL)
        lv_label_set_text(music_toggle_icon, music_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    for (int i = 0; i < 3; i++) {
        if (music_rows[i] != NULL)
            lv_obj_set_style_bg_color(music_rows[i], i == music_track_index ?
                                      lv_color_hex(0x173b42) : panel, 0);
    }
}

static void select_music_track(lv_event_t *event)
{
    music_track_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    music_playing = true;
    update_music();
}

static void music_control(lv_event_t *event)
{
    uint32_t action = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    if (action == 0) music_track_index = (music_track_index + 2) % 3;
    else if (action == 1) music_playing = !music_playing;
    else music_track_index = (music_track_index + 1) % 3;
    if (action != 1) music_playing = true;
    update_music();
}

static void show_music(void)
{
    showing_home = false;
    clear_screen();
    back_button();
    label(screen, "SD 卡音乐", 82, 29, 270, 30, 20, white);
    label(screen, "模拟曲库 · MP3 / WAV", 22, 82, 366, 22, 14, muted);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_button_create(screen);
        music_rows[i] = row;
        lv_obj_set_pos(row, 22, 116 + i * 57);
        lv_obj_set_size(row, 366, 48);
        lv_obj_set_style_bg_color(row, i == music_track_index ?
                                  lv_color_hex(0x173b42) : panel, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, select_music_track, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *track = lv_label_create(row);
        lv_label_set_text(track, demo_tracks[i]);
        lv_obj_align(track, LV_ALIGN_LEFT_MID, 12, -5);
        lv_obj_set_style_text_color(track, white, 0);
        lv_obj_set_style_text_font(track, &lv_font_source_han_sans_sc_14_cjk, 0);
        lv_obj_t *artist = lv_label_create(row);
        lv_label_set_text(artist, demo_artists[i]);
        lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 12);
        lv_obj_set_style_text_color(artist, muted, 0);
        lv_obj_set_style_text_font(artist, &lv_font_source_han_sans_sc_14_cjk, 0);
    }

    music_track_label = label(screen, demo_tracks[music_track_index], 22, 302, 260, 28, 18, white);
    music_status_label = label(screen, music_playing ? "模拟播放中" : "已暂停",
                               284, 305, 104, 22, 14, cyan);
    lv_obj_t *controls = card(screen, 22, 345, 366, 58, panel);
    const char *symbols[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = lv_button_create(controls);
        lv_obj_set_size(button, 72, 42);
        lv_obj_align(button, LV_ALIGN_LEFT_MID, 37 + i * 88, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x263846), 0);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, music_control, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *icon = lv_label_create(button);
        lv_label_set_text(icon, i == 1 && music_playing ? LV_SYMBOL_PAUSE : symbols[i]);
        lv_obj_center(icon);
        lv_obj_set_style_text_color(icon, i == 1 ? cyan : white, 0);
        if (i == 1) music_toggle_icon = icon;
    }
    label(screen, "也可以对小智说：播放《海风来信》", 22, 420, 366, 24, 14, muted);
}

static void show_tools(void)
{
    showing_home = false;
    clear_screen();
    back_button();
    label(screen, "手表设置", 82, 29, 290, 30, 20, white);

    lv_obj_t *assistant = lv_button_create(screen);
    lv_obj_set_pos(assistant, 22, 88);
    lv_obj_set_size(assistant, 366, 80);
    lv_obj_set_style_bg_color(assistant, lv_color_hex(0x142C3A), 0);
    lv_obj_set_style_radius(assistant, 20, 0);
    lv_obj_set_style_shadow_width(assistant, 0, 0);
    lv_obj_add_event_cb(assistant, assistant_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *assistant_text = lv_label_create(assistant);
    lv_label_set_text(assistant_text, "小智语音助手   ·   尚未连接\n模型、音色和人设设置");
    lv_obj_align(assistant_text, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_color(assistant_text, white, 0);
    lv_obj_set_style_text_font(assistant_text, &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_line_space(assistant_text, 8, 0);

    label(screen, "屏幕亮度", 22, 192, 210, 24, 14, muted);
    brightness_slider = lv_slider_create(screen);
    lv_obj_set_pos(brightness_slider, 22, 222);
    lv_obj_set_size(brightness_slider, 300, 14);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider, panel, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, cyan, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, white, LV_PART_KNOB);
    lv_obj_t *brightness_value = lv_label_create(screen);
    char brightness_text[8];
    snprintf(brightness_text, sizeof(brightness_text), "%d%%", brightness_percent);
    lv_label_set_text(brightness_value, brightness_text);
    lv_obj_set_pos(brightness_value, 338, 216);
    lv_obj_set_style_text_color(brightness_value, white, 0);
    lv_obj_add_event_cb(brightness_slider, slider_changed, LV_EVENT_VALUE_CHANGED, brightness_value);

    label(screen, "语音音量", 22, 262, 210, 24, 14, muted);
    volume_slider = lv_slider_create(screen);
    lv_obj_set_pos(volume_slider, 22, 292);
    lv_obj_set_size(volume_slider, 300, 14);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, panel, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, lime, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, white, LV_PART_KNOB);
    lv_obj_t *volume_value = lv_label_create(screen);
    char volume_text[8];
    snprintf(volume_text, sizeof(volume_text), "%d%%", volume_percent);
    lv_label_set_text(volume_value, volume_text);
    lv_obj_set_pos(volume_value, 338, 286);
    lv_obj_set_style_text_color(volume_value, white, 0);
    lv_obj_add_event_cb(volume_slider, slider_changed, LV_EVENT_VALUE_CHANGED, volume_value);

    label(screen, "勿扰模式", 22, 342, 250, 30, 14, white);
    lv_obj_t *dnd = lv_switch_create(screen);
    lv_obj_set_pos(dnd, 328, 338);
    if (dnd_enabled) lv_obj_add_state(dnd, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dnd, dnd_changed, LV_EVENT_VALUE_CHANGED, NULL);
    label(screen, "设备选项保存在手表；小智账号在官方控制台配置。",
          22, 386, 366, 38, 14, muted);
}

static void make_status_bar(void)
{
    label(screen, "波浪智能手表", 22, 22, 210, 24, 14, muted);

    lv_obj_t *face = lv_button_create(screen);
    lv_obj_set_pos(face, 276, 17);
    lv_obj_set_size(face, 112, 34);
    lv_obj_set_style_bg_color(face, panel, 0);
    lv_obj_set_style_radius(face, 14, 0);
    lv_obj_set_style_shadow_width(face, 0, 0);
    lv_obj_add_event_cb(face, cycle_face, LV_EVENT_CLICKED, NULL);
    lv_obj_t *name = lv_label_create(face);
    char face_text[24];
    snprintf(face_text, sizeof(face_text), "%02u / %02u  %s", current_face + 1,
             FACE_COUNT, face_names[current_face]);
    lv_label_set_text(name, face_text);
    lv_obj_center(name);
    lv_obj_set_style_text_font(name, &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_color(name, current_face == 0 ? gold :
                                      current_face == 1 ? cyan : forest, 0);
}

static void make_clock(int y, int font_size, lv_color_t color, bool centered)
{
    clock_label = lv_label_create(screen);
    lv_label_set_text(clock_label, "09:41");
    lv_obj_set_pos(clock_label, 22, y);
    lv_obj_set_size(clock_label, 366, font_size >= 48 ? 82 : 58);
    lv_obj_set_style_text_font(clock_label, font_size >= 48 ? &lv_font_montserrat_48 :
                                                              &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clock_label, color, 0);
    lv_obj_set_style_text_letter_space(clock_label, font_size >= 48 ? 2 : 1, 0);
    if (centered) lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void make_date(int y, bool centered)
{
    date_label = lv_label_create(screen);
    lv_label_set_text(date_label, "11月12日 周四");
    lv_obj_set_pos(date_label, 26, y);
    lv_obj_set_size(date_label, 358, 24);
    lv_obj_set_style_text_font(date_label, &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_color(date_label, muted, 0);
    if (centered) lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void make_app_button(const char *symbol, const char *name,
                            int x, lv_color_t tint)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_pos(button, x, 424);
    lv_obj_set_size(button, 112, 58);
    lv_obj_set_style_bg_color(button, panel, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, app_clicked, LV_EVENT_CLICKED, (void *)name);

    lv_obj_t *icon = lv_label_create(button);
    lv_label_set_text(icon, symbol);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(icon, tint, 0);
    lv_obj_t *title = lv_label_create(button);
    lv_label_set_text(title, name);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_text_color(title, white, 0);
    lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_14_cjk, 0);
}

static void make_app_bar(void)
{
    make_app_button(LV_SYMBOL_CHARGE, "步数", 22, lime);
    make_app_button(LV_SYMBOL_AUDIO, "音乐", 149, cyan);
    make_app_button(LV_SYMBOL_SETTINGS, "设置", 276, white);
}

static void show_luxe(void)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x090909), 0);
    label(screen, "鎏金雅韵", 22, 75, 366, 24, 14, gold);
    make_clock(109, 48, paper, false);
    make_date(184, false);
    lv_obj_t *rule = card(screen, 22, 225, 366, 1, lv_color_hex(0x57492f));
    lv_obj_set_style_radius(rule, 0, 0);
    label(screen, "上海", 26, 250, 92, 24, 14, muted);
    label(screen, "18°", 26, 278, 140, 48, 36, gold);
    label(screen, "晴间多云", 174, 286, 190, 24, 14, paper);

    lv_obj_t *steps = card(screen, 22, 344, 174, 62, lv_color_hex(0x17140e));
    lv_obj_set_style_border_color(steps, lv_color_hex(0x57492f), 0);
    lv_obj_set_style_border_width(steps, 1, 0);
    label(steps, "今日步数", 12, 8, 146, 20, 14, muted);
    label(steps, "6,842", 12, 31, 146, 28, 20, gold);
    lv_obj_t *battery = card(screen, 214, 344, 174, 62, lv_color_hex(0x17140e));
    lv_obj_set_style_border_color(battery, lv_color_hex(0x57492f), 0);
    lv_obj_set_style_border_width(battery, 1, 0);
    label(battery, "电池电量", 12, 8, 146, 20, 14, muted);
    label(battery, "87%", 12, 31, 146, 28, 20, paper);
}

static void show_sport(void)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05090d), 0);
    label(screen, "活力运动", 22, 67, 366, 24, 14, cyan);
    make_date(93, true);
    make_clock(120, 48, white, true);

    lv_obj_t *arc = lv_arc_create(screen);
    lv_obj_set_size(arc, 224, 224);
    lv_obj_set_pos(arc, 93, 206);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 72);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x15232b), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, cyan, LV_PART_INDICATOR);
    label(screen, "6,842", 111, 277, 188, 44, 36, white);
    label(screen, "今日步数", 111, 322, 188, 22, 14, muted);
    label(screen, "目标 8,000 步", 111, 348, 188, 22, 14, lime);
    label(screen, "上海  18°     电量 87%", 22, 390, 366, 22, 14, muted);
}

static void show_classic(void)
{
    lv_obj_set_style_bg_color(screen, paper, 0);
    const lv_color_t dial_ink = lv_color_hex(0x283f35);
    lv_obj_t *dial = lv_obj_create(screen);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, 258, 258);
    lv_obj_set_pos(dial, 76, 70);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0xf8f2e5), 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dial, dial_ink, 0);
    lv_obj_set_style_border_width(dial, 3, 0);
    label(dial, "12", 104, 12, 48, 24, 14, dial_ink);
    label(dial, "3", 218, 108, 24, 24, 14, dial_ink);
    label(dial, "6", 112, 219, 24, 24, 14, dial_ink);
    label(dial, "9", 14, 108, 24, 24, 14, dial_ink);
    label(dial, "经典指针", 49, 175, 160, 22, 14, dial_ink);
    analog_hour_hand = lv_line_create(dial);
    analog_minute_hand = lv_line_create(dial);
    analog_hour_points[0] = (lv_point_precise_t){129, 129};
    analog_hour_points[1] = (lv_point_precise_t){129, 82};
    analog_minute_points[0] = (lv_point_precise_t){129, 129};
    analog_minute_points[1] = (lv_point_precise_t){129, 54};
    lv_line_set_points(analog_hour_hand, analog_hour_points, 2);
    lv_line_set_points(analog_minute_hand, analog_minute_points, 2);
    lv_obj_set_style_line_width(analog_hour_hand, 5, 0);
    lv_obj_set_style_line_width(analog_minute_hand, 3, 0);
    lv_obj_set_style_line_color(analog_hour_hand, dial_ink, 0);
    lv_obj_set_style_line_color(analog_minute_hand, lv_color_hex(0xb76845), 0);
    lv_obj_set_style_line_rounded(analog_hour_hand, true, 0);
    lv_obj_set_style_line_rounded(analog_minute_hand, true, 0);
    lv_obj_t *pivot = lv_obj_create(dial);
    lv_obj_remove_style_all(pivot);
    lv_obj_set_size(pivot, 13, 13);
    lv_obj_set_pos(pivot, 122, 122);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pivot, lv_color_hex(0xb76845), 0);
    lv_obj_set_style_bg_opa(pivot, LV_OPA_COVER, 0);
    make_date(348, true);
    label(screen, "上海  ·  晴间多云  ·  18°", 22, 378, 366, 22, 14, dial_ink);
}

static void show_watch_face(void)
{
    showing_home = true;
    clear_screen();
    if (!gesture_registered) {
        lv_obj_add_event_cb(screen, face_gesture, LV_EVENT_GESTURE, NULL);
        gesture_registered = true;
    }
    make_status_bar();

    switch (current_face) {
        case 1: show_sport(); break;
        case 2: show_classic(); break;
        default: show_luxe(); break;
    }

    make_app_bar();
    update_clock(NULL);
}

static void show_detail(const char *title, const char *headline,
                        const char *subline)
{
    showing_home = false;
    clear_screen();
    back_button();

    label(screen, title, 82, 29, 270, 30, 20, white);
    lv_obj_t *hero = card(screen, 22, 104, 366, 184, panel);
    label(hero, headline, 24, 36, 320, 58, 36, cyan);
    label(hero, subline, 24, 104, 320, 42, 14, muted);
    label(screen, "点击左上角返回", 22, 322, 330, 28, 14, muted);
}

void watch_ui_create(void)
{
    show_watch_face();
    lv_timer_create(update_clock, 1000, NULL);
}
