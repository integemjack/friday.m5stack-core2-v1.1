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
void work_tap_interrupt(void);     // 「停」→ voice.interrupt（打断播报/生成）
void work_voice_held(bool held);   // 键 A：true=按住（必要时自动 voice.start），false=松开（草稿留在服务端）
void work_voice_release(void);     // 键 B（发给助手）：voice.held false → 服务端定稿草稿交给助手
void work_voice_discard(void);     // 键 C（取消）/ 发给会话后清草稿：voice.stop

// 发文字到某个会话（runner.submit；conv_id 来自会话列表）
void work_send_text(const char *conv_id, const char *text);

// 会话列表（activity.subscribe 的快照；UI 拉）
typedef struct {
  char id[40];
  char title[64];
  char status[12];     // idle / running / approval / question / quota / error
} work_session_t;
void work_activity_sub(void);                       // 会话页打开：订阅并拉全量
void work_activity_unsub(void);                     // 会话页关闭：退订
int  work_sessions_copy(work_session_t *out, int cap);   // 拷出快照，返回条数
uint32_t work_sessions_version(void);               // 快照变了没有（UI 刷新用）
bool work_assistant_enabled(void);                  // 这台 Friday 的助手开着吗

// 底层喂音（audio_io 调）：pcm16 mono 16k，len 字节数；内部拼 JSON 入 peer 发送队列
void work_feed_audio(const int16_t *pcm, int samples);

// 状态查询
bool work_dc_open(void);
const char *work_request_id(void);

// app_main 调：把 schat WS 事件接进来
void work_bind_ws(void);
