// portal.h — SoftAP 配网（首次 / WiFi 连不上时）
#pragma once
#include <cstdint>

// 开 SoftAP + HTTP 配网页；阻塞直到拿到新配置（返回 true）或超时（false）
// 页面可填：WiFi SSID/密码、chatserver 地址
bool portal_run(uint32_t timeout_ms);
