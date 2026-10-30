cmake_minimum_required(VERSION 3.16)
project(quota_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(FW ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

# lv_conf.h lives in this dir; lv_conf_internal.h picks it up via LV_CONF_INCLUDE_SIMPLE
add_subdirectory(${FW}/managed_components/lvgl__lvgl ${CMAKE_BINARY_DIR}/lvgl)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 REQUIRED)
target_link_libraries(lvgl PUBLIC SDL2::SDL2)
target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})

add_executable(quota_sim
    sim_main.cpp
    ${FW}/main/apps/app_quota/quota_view.cpp
    ${FW}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c
    ${FW}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_16_4.c
    ${FW}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_14_1.c
    $ENV{HOME}/esp/esp-idf/components/json/cJSON/cJSON.c
)
# stubs/ first: provides board.h + mooncake_log.h for quota_fetch.cpp
target_include_directories(quota_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${FW}/main/apps/app_quota
    $ENV{HOME}/esp/esp-idf/components/json/cJSON
)
# mirror sdkconfig.defaults.local so exclusion logic runs identically
target_compile_definitions(quota_sim PRIVATE
    CONFIG_QUOTA_MAGPIE_EXCLUDE="OpenCode Zen")
target_link_libraries(quota_sim PRIVATE lvgl SDL2::SDL2)
