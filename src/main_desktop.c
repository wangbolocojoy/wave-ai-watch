#include <stdio.h>
#include <time.h>
#include <SDL.h>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "watch_ui.h"

int main(void)
{
    lv_init();

    lv_display_t *display = lv_sdl_window_create(410, 502);
    if (display == NULL) {
        fprintf(stderr, "Could not create the 410x502 SDL display.\n");
        return 1;
    }

    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, display);
    lv_display_set_default(display);

    watch_ui_create();
    while (1) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > 10) wait_ms = 10;
        SDL_Delay(wait_ms);
    }
}
