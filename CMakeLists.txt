# M5Stack Core2 v1.1 — Friday 语音终端（Friday Work 同款 WebRTC 工作通道 + voice.*）
cmake_minimum_required(VERSION 3.16)
# 版本号单一来源：main/app.h 的 APP_VERSION（根 CMakeLists 在此解析注入 esp_app_desc，
# CI 发 Release 时校验 tag vX.Y.Z 与它一致；OTA 也拿它比对）
file(READ "${CMAKE_CURRENT_LIST_DIR}/main/app.h" _app_h)
string(REGEX MATCH "#define[ \t]+APP_VERSION[ \t]+\"([^\"]+)\"" _ "${_app_h}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "main/app.h 里没解析到 APP_VERSION")
endif()
set(PROJECT_VER ${CMAKE_MATCH_1})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5_friday_voice)
