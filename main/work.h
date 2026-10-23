// work.h — WebRTC 工作通道：p2p 信令 + esp_peer(DataChannel) + JSON-RPC + voice.*
//
// 主叫流程（REMOTE.md §2，与 Friday Work WorkSignaling.ts / RemoteWorkSession.cpp 对齐）：
//   p2p_request{callType:"work"} → p2p_response{accepted} → esp_peer_new_connection
//   → create_data_channel("friday-work") → 本地 SDP → p2p_signaling{type:"offer"}
//   → answer/ice_candidate 互送 → DataChannel open → JSON-RPC hello → voice.start
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 会话起停
void work_start(int64_t bot_id);   // dial
void work_stop(void);              // hangup（不发 request_cancel：重启即重拨）
void work_reset(void);             // 仅清状态（WS 断时用）

// 外部触发
void work_tap_interrupt(void);     // 「停」按钮 → voice.interrupt
void work_set_held(bool held);     // 按住说话 → voice.held

// 底层喂音（audio_io 调）：pcm16 mono 16k，len 字节数；内部拼 JSON 入 peer 发送队列
void work_feed_audio(const int16_t *pcm, int samples);

// 状态查询
bool work_dc_open(void);
const char *work_request_id(void);

// app_main 调：把 schat WS 事件接进来
void work_bind_ws(void);
