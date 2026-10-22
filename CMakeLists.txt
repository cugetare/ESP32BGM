# ESP-IDF project for ESP32BGM (Ai-Thinker ESP32 Audio Kit, ESP32-A1S + ES8388)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Re-run CMake when VERSION changes, so the app descriptor never goes stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
project(ESP32BGM)
