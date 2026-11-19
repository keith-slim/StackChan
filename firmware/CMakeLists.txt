# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Keep compiler and linker arguments below Windows command-line limits.
if(CMAKE_GENERATOR MATCHES "^Ninja")
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE TRUE CACHE INTERNAL "Use Ninja response files")
endif()

set(PROJECT_VER "1.6.2")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Auto-load a local sdkconfig overlay (git-ignored) when present. Lets
# self-hosted / custom deployments pin CONFIG_STACKCHAN_SERVER_URL,
# CONFIG_OTA_URL, etc. without touching committed defaults.
# Manual equivalent: SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" idf.py build
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local"
        CACHE STRING "ESP-IDF sdkconfig default files (auto-extended with local overlay)" FORCE)
    message(STATUS "StackChan: detected sdkconfig.defaults.local, applying overlay")
endif()

# Preserve managed-component file contents so Git line-ending conversion does
# not invalidate ESP-IDF's component hashes on Windows.
set(ENV{GIT_CONFIG_COUNT} "1")
set(ENV{GIT_CONFIG_KEY_0} "core.autocrlf")
set(ENV{GIT_CONFIG_VALUE_0} "false")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)

# Only the LVGL component needs the PSRAM allocator hook. A project-wide
# definition would not reliably reach managed components in every IDF build.
idf_build_get_property(_stackchan_components BUILD_COMPONENTS)
set(_stackchan_lvgl_hook_found FALSE)
foreach(_stackchan_component IN LISTS _stackchan_components)
    if(_stackchan_component STREQUAL "lvgl__lvgl" OR _stackchan_component STREQUAL "lvgl")
        idf_component_get_property(_stackchan_lvgl_target ${_stackchan_component} COMPONENT_LIB)
        target_compile_definitions(${_stackchan_lvgl_target} PRIVATE
            LV_MEM_POOL_INCLUDE=\"${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi-esp32/main/display/lvgl_psram_pool.h\")
        set(_stackchan_lvgl_hook_found TRUE)
    endif()
endforeach()
if(NOT _stackchan_lvgl_hook_found)
    message(FATAL_ERROR "LVGL component not found: PSRAM pool hook cannot be configured")
endif()
