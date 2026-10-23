// ota.h — 固件自更新（GitHub Releases，tag vX.Y.Z ↔ main/app.h 的 APP_VERSION）
#pragma once
#include <stdbool.h>

void ota_init(void);          // WiFi 连上后调：起后台任务（开机 30s 后首查，之后每 6h 静默检查）
bool ota_check_now(void);     // 手动检查（有新版则下载安装并重启）；false = 任务忙
bool ota_busy(void);
bool ota_newer_available(void);   // 静默检查的结果：有新版（只提示，不自动装）

extern char ota_latest_ver[24];   // 最新版本号（去 v 前缀）；空 = 没查过/没查到

// "0.10.0" > "0.9.1" 的数字比较（前导 v/V 跳过，缺段按 0）
bool ota_ver_newer(const char *latest, const char *cur);
