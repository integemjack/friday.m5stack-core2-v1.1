// app.h — 全局状态与配置
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define APP_VERSION "0.2.0"
#define PROTOCOL_VERSION 1  // REMOTE.md §4

// ---- 默认值 ----
#define DEFAULT_SCHAT_SERVER "https://47.100.98.3:2012"
#define DEVICE_NAME "M5Stack Core2"

// voice.* 音频参数（REMOTE.md §6「语音」）
#define VOICE_SAMPLE_RATE 16000
#define VOICE_FEED_SAMPLES (VOICE_SAMPLE_RATE / 10)   // 100ms 一帧 feed
#define VOICE_FEED_BYTES (VOICE_FEED_SAMPLES * 2)

// 主界面阶段
typedef enum {
  APP_BOOT = 0,        // 启动
  APP_PORTAL,          // SoftAP 配网（首次）
  APP_WIFI_WAIT,       // 连 WiFi
  APP_QR_PAIR,         // 屏显二维码等扫码
  APP_BOT_PICK,        // 扫码后选哪个 bot（多个机器人时）
  APP_WS_CONNECT,      // 连 chatserver WS
  APP_DIAL,            // p2p_request(work) 等接听
  APP_PEER_CONNECT,    // WebRTC 握手
  APP_RPC_HELLO,       // DataChannel 打开，发 hello
  APP_VOICE_START,     // voice.start
  APP_LIVE,            // 正常：语音来回
  APP_FATAL,           // 不可恢复错误（显示后停）
} app_stage_t;

// 语音链路状态（voice.state 的 state 字段）
typedef enum {
  VOICE_NONE = 0,
  VOICE_LISTENING,
  VOICE_HEARING,     // 听到声音
  VOICE_THINKING,    // 助手在想
  VOICE_SPEAKING,    // 助手在念
  VOICE_ERROR,
} voice_state_t;

// 全局共享状态（跨任务读，UI 主任务写部分字段；锁内改）
typedef struct {
  app_stage_t stage;
  char status_text[96];      // 屏幕顶部的状态行
  char caption[512];         // 实时字幕（听写/回复）
  char reply_tail[256];      // 最近一句回复的尾巴（显示用）
  voice_state_t voice_state;
  bool voice_available;
  bool voice_active;
  bool held;                 // 按住说话中
  bool ducked;
  int volume;                // 0-255
  int battery_pct;
  bool charging;
  int rssi;
  char bot_name[64];         // Friday 机器人名
  int64_t bot_user_id;
  char peer_ip[48];          // WebRTC 对端地址（展示用）
  uint32_t feeds_sent, audio_frames, drops;
} app_state_t;

extern app_state_t g_app;
extern portMUX_TYPE g_app_mux;

static inline void app_lock(void) { portENTER_CRITICAL(&g_app_mux); }
static inline void app_unlock(void) { portEXIT_CRITICAL(&g_app_mux); }

void app_set_stage(app_stage_t s);
void app_set_status(const char *fmt, ...);
void app_set_caption(const char *utf8);  // utf8 或 NULL 清空

// 供其他模块拿当前 server / 凭据（store.h 里的持久化数据）
struct schat_session;
