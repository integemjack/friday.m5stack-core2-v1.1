// store.h — NVS 持久化（WiFi、chatserver 地址、登录凭据、bot 选择）
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  char server[128];        // https://host:port
  char device_id[40];      // 稳定设备 id（随机 hex 32）
  // 扫码登录领到的凭据（Friday Work Login.ts 的口径）
  char token[512];         // 当前 token（WS hello / REST Bearer，服务端发的是短命 access JWT）
  char refresh[512];       // refreshToken（轮换）
  int64_t user_id;         // 自己的账号 id
  int64_t bot_user_id;     // 选中的 Friday 机器人 id（0 = 自动取第一个）
  char bot_name[64];
  int64_t expires_at_ms;   // 当前 token 的死线（本机开机相对毫秒；跨重启无效，不读回）
  char wifi_ssid[33];
  char wifi_pass[65];
  int volume;              // 喇叭音量 0..255（音量页改）
} store_t;

extern store_t g_store;

void store_init(void);            // NVS 打开 + 读全部
void store_save_wifi(void);
void store_save_server(void);
void store_save_auth(void);       // token/refresh/userId/expires/bot
void store_save_bot(void);
void store_save_volume(void);       // 音量
void store_clear_bot(void);         // 忘掉选中的机器人（换机器人连）
void store_clear_auth(void);      // 登出：清 token 相关
void store_gen_device_id(void);   // 没有时生成
