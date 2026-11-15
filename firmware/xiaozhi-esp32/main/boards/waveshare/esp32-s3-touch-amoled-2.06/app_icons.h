#pragma once

#include <lvgl.h>

static constexpr int kAppIconSize = 108;

#ifdef __cplusplus
extern "C" {
#endif

LV_IMAGE_DECLARE(app_icon_assistant);
LV_IMAGE_DECLARE(app_icon_music);
LV_IMAGE_DECLARE(app_icon_activity);
LV_IMAGE_DECLARE(app_icon_alarm);
LV_IMAGE_DECLARE(app_icon_timer);
LV_IMAGE_DECLARE(app_icon_settings);
LV_IMAGE_DECLARE(app_icon_voice_memo);
LV_IMAGE_DECLARE(app_icon_photos);
LV_IMAGE_DECLARE(app_icon_videos);

#ifdef __cplusplus
}
#endif
