cmake_minimum_required(VERSION 3.22)

# Local, git-ignored overrides (Wi-Fi credentials etc.) are applied on top of the defaults.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
endif()

# The project's own components, in place of IDF's main/ and components/:
#   App       the probe application
#   Platform  Black Magic Debug (External/blackmagic) with the ESP32-S3 platform port
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/App
    ${CMAKE_CURRENT_LIST_DIR}/Platform
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mvprobe)
