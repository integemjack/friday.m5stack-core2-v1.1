idf_component_register(
  SRCS
    "app_main.cpp"
    "util.cpp"
    "store.cpp"
    "schat.cpp"
    "work.cpp"
    "audio_io.cpp"
    "ui.cpp"
    "portal.cpp"
    "ota.cpp"
  INCLUDE_DIRS "."
  REQUIRES
    nvs_flash
    esp_wifi
    esp_http_client
    esp_websocket_client
    esp_http_server
    esp_timer
    json
    mbedtls
    esp_peer
    esp_libsrtp
    app_update
    M5Unified
    M5GFX
    esp_driver_i2s
)

# 调试构建：设了环境变量 UI_DUMP 就把 ui.cpp 的绘制目标换成内存画布，
# 开机把各页面 RLE 吐到串口（见 tools/ui-dump-decode.mjs）；正常构建不受影响。
if(DEFINED ENV{UI_DUMP})
  target_compile_options(${COMPONENT_LIB} PRIVATE -DUI_DUMP)
endif()
