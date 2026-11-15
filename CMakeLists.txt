cmake_minimum_required(VERSION 3.20)
project(waveshare_watch_system C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

find_package(SDL2 REQUIRED)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
target_include_directories(lvgl SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})

add_executable(watch-simulator
    src/main_desktop.c
    src/watch_ui.c
)
target_compile_definitions(watch-simulator PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_include_directories(watch-simulator PRIVATE src ${SDL2_INCLUDE_DIRS})
target_link_libraries(watch-simulator PRIVATE lvgl ${SDL2_LIBRARIES})
