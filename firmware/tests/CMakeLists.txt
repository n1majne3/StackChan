cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES C CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

# quota_fetch.cpp is compiled into the test directly (like sim_quota does):
# stubs/ supplies board.h + mooncake_log.h, cJSON is the real ESP-IDF source.
add_executable(quota_summary_test
    quota_summary_test.cpp
    $ENV{HOME}/esp/esp-idf/components/json/cJSON/cJSON.c
)

target_include_directories(quota_summary_test PRIVATE
    ../sim_quota/stubs
    ../main/apps/app_quota
    $ENV{HOME}/esp/esp-idf/components/json/cJSON
)

target_compile_features(quota_summary_test PRIVATE cxx_std_17)

target_compile_definitions(quota_summary_test PRIVATE
    CONFIG_QUOTA_HIDE="OpenCode Zen")

add_test(NAME quota_summary_test COMMAND $<TARGET_FILE:quota_summary_test>)
