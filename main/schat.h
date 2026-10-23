// schat.h — chatserver 的 HTTP API + schat-ws/v1 WebSocket 客户端
//
// 协议出处（与 Friday Work 一字不差）：
//   扫码登录   friday-desktop-work src/protocol/schat/Login.ts（chatserver internal/server/login_http.go）
//   WS 帧格式  src/protocol/schat/SchatClient.ts（Secret-Chat docs/raw_ws_protocol.md）
//   WS 入口    https://host:port → wss://host:port/ws
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- HTTP ----
// POST /login-sessions → {token, expiresAt, intervalMs}；失败 false
bool schat_create_qr_session(char *out_token, size_t cap, int *out_interval_ms);

typedef enum { QR_PENDING = 0, QR_SCANNED, QR_APPROVED, QR_EXPIRED, QR_ERROR } qr_status_t;
// POST /login-sessions/poll；approved 时已写进 g_store（store_save_auth）
qr_status_t schat_poll_qr(const char *token);

// POST /auth/refresh；成功写 g_store（refresh 轮换：旧的作废）
typedef enum {
  SCHAT_REFRESH_OK = 0,   // 拿到新凭据（已存 NVS）
  SCHAT_REFRESH_NO_TOKEN, // 没有 refreshToken，刷不了
  SCHAT_REFRESH_AUTH,     // 服务端拒了（401/403/404）：必须重新扫码
  SCHAT_REFRESH_NET,      // 网络/服务端异常：旧凭据可能还能用，别清
} schat_refresh_t;
schat_refresh_t schat_refresh(void);

// 当前 token 的死线（本机开机相对的毫秒，与 now_ms64 同源）；0 = 未知
int64_t schat_token_deadline_ms(void);
// WS 等 hello 回执时的“泵”（主循环刷新触摸/界面用，不设也能跑）
void schat_set_pump(void (*fn)(void));

// GET /friends?include=all → 第一个可用机器人（isBot 且 botCalls!=false 且 botDisabled!=true）
// 登录后拉一次好友里的 bot 列表（选择页用）；-1 = 失败
int schat_list_bots(int64_t *ids, char (*names)[48], int max);

// ---- WebSocket ----
typedef void (*schat_ev_fn)(const char *event, const char *data_json, size_t len);

bool schat_ws_start(void);        // 连 + hello；成功 true
void schat_ws_stop(void);
bool schat_ws_online(void);
bool schat_ws_kicked(void);       // auth_expired：需重新扫码

// 发一个 req 并异步等 ack（不等待：fire and forget，ack 在事件回调里分发）
bool schat_ws_request(const char *event, const char *data_json);

// WS 生命周期事件（app_main 注册：断线重连后重拨）
typedef void (*schat_ws_cb)(void);
void schat_ws_on_reconnected(schat_ws_cb cb);

// 各事件（p2p_* 等）的统一分发
void schat_ws_on_event(schat_ev_fn fn);

// 发一段拼好的完整信封帧（work.cpp 自己拼 p2p_* 帧用）
bool schat_ws_send_raw(const char *frame, size_t len);
