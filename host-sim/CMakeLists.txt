cmake_minimum_required(VERSION 3.20)
project(waveshare_watch_host_harness C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(lvgl
    URL https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v9.3.0
    URL_HASH SHA256=4933becfd3603b29158a5d04138139582836ef2bc17beb6c39dccda9cb0d32e7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lvgl)

# The LVGL library itself must be compiled against the host config (TinyTTF,
# stdio FS and the fallback CJK font). This include path is intentionally
# public so both lvgl and the host executable see the same lv_conf.h.
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 REQUIRED)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
target_include_directories(lvgl SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(BOARD_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-amoled-2.06/esp32-s3-touch-amoled-2.06.cc)
set(GENERATED_UI ${CMAKE_CURRENT_BINARY_DIR}/generated/watch_display_ui.h)

add_custom_command(
    OUTPUT ${GENERATED_UI}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_watch_ui.py
            ${BOARD_SOURCE} ${GENERATED_UI}
    DEPENDS ${BOARD_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_watch_ui.py
    COMMENT "Extracting the firmware CustomLcdDisplay view"
    VERBATIM
)
add_custom_target(watch_ui_generated DEPENDS ${GENERATED_UI})

set(NOTO_FONTS_DIR "" CACHE PATH "Path to the XiaoZhi noto-fonts component")
if(NOT NOTO_FONTS_DIR)
    set(_local_noto_fonts
        ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/xiaozhi-esp32/managed_components/78__xiaozhi-fonts)
    if(EXISTS "${_local_noto_fonts}/src/font_material_symbols_30_4.c")
        set(NOTO_FONTS_DIR "${_local_noto_fonts}")
    else()
        FetchContent_Declare(noto_fonts
            URL https://codeload.github.com/78/xiaozhi-fonts/tar.gz/d45dbc64052d57048f20ab1770074172ce9eb53b
            URL_HASH SHA256=06ccee9ef0a167d2afe8597c2df415a49ae439b21307679006ec9edf70713d47
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_GetProperties(noto_fonts)
        if(NOT noto_fonts_POPULATED)
            FetchContent_Populate(noto_fonts)
        endif()
        set(NOTO_FONTS_DIR "${noto_fonts_SOURCE_DIR}")
    endif()
endif()
if(NOT EXISTS "${NOTO_FONTS_DIR}/src/font_material_symbols_30_4.c" OR
   NOT EXISTS "${NOTO_FONTS_DIR}/include/material_symbols.h")
    message(FATAL_ERROR
        "noto-fonts assets were not found in '${NOTO_FONTS_DIR}'. "
        "Set -DNOTO_FONTS_DIR=/path/to/xiaozhi-fonts.")
endif()

add_executable(watch-host
    src/main.cc
    src/lcd_display_host.cc
    shim/board.cc
    shim/settings.cc
    shim/application.cc
    shim/watch_timer_service.cc
    shim/watch_alarm_service.cc
    shim/watch_music_player.cc
    ${NOTO_FONTS_DIR}/src/font_material_symbols_30_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-amoled-2.06/whale_girl_image.c
)
add_dependencies(watch-host watch_ui_generated)
target_compile_definitions(watch-host PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_include_directories(watch-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${NOTO_FONTS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-amoled-2.06
    ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(watch-host PRIVATE lvgl ${SDL2_LIBRARIES})

