cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS
           "components")

idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(MINIMAL_BUILD ON)

project(jkbms)

# LVGL hangs in while(1) on a failed allocation, leaving a remote board frozen until
# someone unplugs it; main/lvgl/lv_assert_restart.h (CONFIG_LV_ASSERT_HANDLER_INCLUDE)
# restarts it instead
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/main/lvgl")
