#include "work.h"
#include "schat.h"
#include "store.h"
#include "util.h"
#include "app.h"
#include "audio_io.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "work";

// ---------------- 状态 ----------------
static char g_request_id[40];
static int64_t g_bot_id;
static bool g_dialing;
static bool g_accepted;
static esp_peer_handle_t g_peer;
static bool g_peer_task_run;
static bool g_dc_open;
static int g_rpc_id;

// 对 peer 的命令（WS 事件任务 → peer 任务，避免跨任务调 esp_peer）
enum { PCMD_NONE = 0, PCMD_SET_SDP, PCMD_SET_CANDIDATE, PCMD_STOP, PCMD_OPEN };
typedef struct {
  uint8_t cmd;
  char payload[4096];
} peer_cmd_t;
static QueueHandle_t g_cmd_q;
static void peer_open_and_dial(void);   // peer_task 里调，定义在后面

// feed 队列（audio 任务 → peer 任务）：预拼好的 JSON 字符串指针
static QueueHandle_t g_feed_q;

// 会话列表快照（activity.subscribe；UI 任务读、peer 任务写）
static work_session_t g_sessions[24];
static int g_sessions_n;
static uint32_t g_sessions_ver;
static SemaphoreHandle_t g_sess_mu;
static bool g_assistant_ok;
static int g_rid_activity = -1;      // activity.subscribe 的回执 id

// 语音会话活着吗（v0.3：服务端答完一轮会自动 stop；再按 A 要先 voice.start）
static bool g_voice_on;
static int g_rid_voice_start = -1;
static int g_rid_hello = -1;   // ★ hello 回执匹配用 id，不能用固定 1：重拨后 id 递增，
                               //   写死 1 会让重拨后永远不再 voice.start（真机踩过：
                               //   通道 160s 空闲被服务端挂断→重拨→喂音全部发进没开的会话）

static void sess_mu_init(void)
{
  if (!g_sess_mu) g_sess_mu = xSemaphoreCreateMutex();
}

// ---------------- JSON 小助手 ----------------

static size_t json_escape(const char *src, size_t len, char *dst, size_t cap)
{
  size_t o = 0;
  for (size_t i = 0; i < len && o + 7 < cap; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
    else if (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
    else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
    else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
    else dst[o++] = c;
  }
  dst[o] = '\0';
  return o;
}

// p2p_* 信令帧（WorkSignaling.ts 的 sendCallFrame 形状：a 是字符串 ackId，chatserver 的 Envelope.A 是 string）
static int g_p2p_ack;
static void send_p2p(const char *event, const char *extra_kv_json)
{
  char *buf = (char *)malloc(strlen(extra_kv_json) + 512);
  if (!buf) return;
  int n = snprintf(buf, strlen(extra_kv_json) + 512,
    "{\"e\":\"%s\",\"d\":{\"toUserId\":%lld,\"targetUserId\":%lld,\"fromUserId\":%lld,"
    "\"requestId\":\"%s\",%s,\"timestamp\":%llu},\"a\":\"%d\",\"t\":\"req\"}",
    event, (long long)g_bot_id, (long long)g_bot_id, (long long)g_store.user_id,
    g_request_id, extra_kv_json, (unsigned long long)now_ms64(), ++g_p2p_ack);
  (void)n;
  // schat 层直接发（复用 request 的信封，但不等 ack：requestId 匹配在事件里做）
  // 这里绕过 schat_ws_request 的自动信封：自己拼好了完整信封
  extern bool schat_ws_send_raw(const char *frame, size_t len);
  schat_ws_send_raw(buf, strlen(buf));
  free(buf);
}

// ---------------- RPC over DataChannel ----------------

static void rpc_send_json(const char *json)
{
  if (!g_peer || !g_dc_open) return;
  esp_peer_data_frame_t f = {
    .type = ESP_PEER_DATA_CHANNEL_STRING,
    .data = (uint8_t *)json,
    .size = (int)strlen(json),
  };
  esp_peer_send_data(g_peer, &f);
}

static int rpc_request(const char *method, const char *params_json)
{
  char *buf = (char *)malloc(strlen(method) + strlen(params_json) + 96);
  if (!buf) return 0;
  int id = ++g_rpc_id;
  snprintf(buf, strlen(method) + strlen(params_json) + 96,
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
           id, method, params_json);
  rpc_send_json(buf);
  free(buf);
  return id;
}

// 调试探针：指定 id 发一个 RPC（ack 在 handle_rpc_message 的 rid>=900 分支原样打日志）
// 保留但默认不用：以后要摸服务端 RPC 字段时叫一行这个就行
static void rpc_debug(int id, const char *method, const char *params_json) __attribute__((unused));
static void rpc_debug(int id, const char *method, const char *params_json)
{
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
           id, method, params_json);
  rpc_send_json(buf);
}

static void rpc_notify(const char *method, const char *params_json)
{
  char *buf = (char *)malloc(strlen(method) + strlen(params_json) + 80);
  if (!buf) return;
  snprintf(buf, strlen(method) + strlen(params_json) + 80,
           "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params_json);
  rpc_send_json(buf);
  free(buf);
}

// ---------------- voice.* 事件处理 ----------------

static const char *voice_state_text(const char *s)
{
  if (!s) return NULL;
  if (!strcmp(s, "listening")) return "聆听中";
  if (!strcmp(s, "hearing")) return "听到你说话";
  if (!strcmp(s, "thinking")) return "在想";
  if (!strcmp(s, "speaking")) return "在念";
  return s;
}

static void voice_state_apply(cJSON *d)
{
  if (!d) return;
  app_lock();
  cJSON *a = cJSON_GetObjectItem(d, "active");
  if (a) { g_app.voice_active = cJSON_IsTrue(a); g_voice_on = g_app.voice_active; }
  cJSON *av = cJSON_GetObjectItem(d, "available");
  if (av) g_app.voice_available = cJSON_IsTrue(av);
  const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(d, "state"));
  if (st) {
    if (!strcmp(st, "listening")) g_app.voice_state = VOICE_LISTENING;
    else if (!strcmp(st, "hearing")) g_app.voice_state = VOICE_HEARING;
    else if (!strcmp(st, "thinking")) g_app.voice_state = VOICE_THINKING;
    else if (!strcmp(st, "speaking")) g_app.voice_state = VOICE_SPEAKING;
    else g_app.voice_state = VOICE_NONE;
    if (!strcmp(st, "off")) g_voice_on = false;
  }
  const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(d, "error"));
  if (err && err[0]) { strlcpy(g_app.status_text, err, sizeof(g_app.status_text)); }
  app_unlock();
  const char *cap = cJSON_GetStringValue(cJSON_GetObjectItem(d, "caption"));
  if (cap && cap[0]) app_set_caption(cap);
}

static void handle_voice_event(const char *method, cJSON *params)
{
  // 延迟测量点（自动测试用）
  if (!strcmp(method, "voice.state")) {
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(params, "state"));
    if (st) ESP_LOGI(TAG, "[LAT] state=%s t=%u", st, (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    const char *cap = cJSON_GetStringValue(cJSON_GetObjectItem(params, "caption"));
    if (cap && cap[0]) ESP_LOGI(TAG, "[LAT] caption=\"%s\" t=%u", cap, (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    voice_state_apply(params);
  } else if (!strcmp(method, "voice.audio")) {
    static uint32_t first_audio_t;
    if (!first_audio_t) {
      first_audio_t = xTaskGetTickCount() * portTICK_PERIOD_MS;
      ESP_LOGI(TAG, "[LAT] first-tts-frame t=%u", first_audio_t);
    }
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(params, "pcm"));
    if (b64) {
      static int16_t pcm[VOICE_FEED_SAMPLES * 3];   // 最大 60ms 帧
      int n = b64_decode(b64, strlen(b64), (uint8_t *)pcm, sizeof(pcm));
      if (n > 0) {
        audio_play(pcm, n / 2);
        app_lock(); g_app.audio_frames++; app_unlock();
      }
    }
  } else if (!strcmp(method, "voice.clearAudio")) {
    audio_clear();
  } else if (!strcmp(method, "voice.duck")) {
    cJSON *on = cJSON_GetObjectItem(params, "on");
    audio_duck(cJSON_IsTrue(on));
  } else if (!strcmp(method, "voice.pause")) {
    cJSON *p = cJSON_GetObjectItem(params, "paused");
    audio_pause(cJSON_IsTrue(p));
  }
}

static void handle_rpc_message(const char *json, size_t len)
{
  cJSON *msg = cJSON_ParseWithLength(json, len);
  if (!msg) return;
  const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "method"));
  cJSON *params = cJSON_GetObjectItem(msg, "params");
  if (method) {
    // 事件
    if (!strcmp(method, "activity.assistant")) {
      bool en = cJSON_IsTrue(cJSON_GetObjectItem(params, "enabled"));
      if (g_sess_mu) { xSemaphoreTake(g_sess_mu, portMAX_DELAY); g_assistant_ok = en; g_sessions_ver++; xSemaphoreGive(g_sess_mu); }
      ESP_LOGI(TAG, "assistant %s", en ? "on" : "off");
    } else if (!strcmp(method, "activity")) {
      // 增量动静：不解析（列表页只做快照），刷一下版本号让 UI 重拉也不至于
    } else {
      handle_voice_event(method, params);
    }
    cJSON_Delete(msg);
    return;
  }
  // 回执
  cJSON *id = cJSON_GetObjectItem(msg, "id");
  cJSON *result = cJSON_GetObjectItem(msg, "result");
  cJSON *error = cJSON_GetObjectItem(msg, "error");
  if (!id || !cJSON_IsNumber(id)) { cJSON_Delete(msg); return; }
  int rid = (int)id->valuedouble;
  if (error) {
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
    cJSON *ec = cJSON_GetObjectItem(error, "code");
    app_set_status("Friday：%s", m ? m : "出错了");
    ESP_LOGW(TAG, "RPC id=%d 出错：code=%d msg=%s", rid,
             (ec && cJSON_IsNumber(ec)) ? (int)ec->valuedouble : 0, m ? m : "");
    if ((rid == g_rid_hello && rid > 0) || (ec && cJSON_IsNumber(ec) && (int)ec->valuedouble == 4)) {
      // hello / voice.start 失败：显示原因（助手没开 / 没装本地语音）
      app_set_status("voice 不可用：%s", m ? m : "");
    }
    cJSON_Delete(msg);
    return;
  }
  if (rid >= 900) {   // 调试探针（settings.* 探索用）：原样打回日志
    char *s = cJSON_PrintUnformatted(msg);
    ESP_LOGI(TAG, "RPC#%d -> %s", rid, s ? s : "?");
    if (s) cJSON_free(s);
    cJSON_Delete(msg);
    return;
  }
  if (rid == g_rid_activity && rid > 0 && result) {
    // activity.subscribe 回执：{sessions:[SessionActivity], assistant}
    if (g_sess_mu) {
      xSemaphoreTake(g_sess_mu, portMAX_DELAY);
      g_sessions_n = 0;
      cJSON *arr = cJSON_GetObjectItem(result, "sessions");
      cJSON *s;
      cJSON_ArrayForEach(s, arr) {
        if (g_sessions_n >= (int)(sizeof(g_sessions) / sizeof(g_sessions[0]))) break;
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(s, "id"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(s, "title"));
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(s, "status"));
        if (!id || !id[0]) continue;
        strlcpy(g_sessions[g_sessions_n].id, id, sizeof(g_sessions[0].id));
        strlcpy(g_sessions[g_sessions_n].title, title ? title : "(无名会话)", sizeof(g_sessions[0].title));
        strlcpy(g_sessions[g_sessions_n].status, status ? status : "idle", sizeof(g_sessions[0].status));
        g_sessions_n++;
      }
      cJSON *as = cJSON_GetObjectItem(result, "assistant");
      if (as) g_assistant_ok = cJSON_IsTrue(as);
      g_sessions_ver++;
      xSemaphoreGive(g_sess_mu);
      ESP_LOGI(TAG, "activity 快照：%d 个会话，assistant=%d", g_sessions_n, (int)g_assistant_ok);
    }
    cJSON_Delete(msg);
    return;
  }
  if (rid == g_rid_voice_start && rid > 0 && error) {
    g_voice_on = false;   // start 被拒：下次按 A 重试
  }
  if (rid == g_rid_hello && rid > 0 && result) {
    // hello 回执：看 server.features 有没有 voice
    cJSON *server = cJSON_GetObjectItem(result, "server");
    const char *host = NULL, *ver = NULL;
    if (server) {
      host = cJSON_GetStringValue(cJSON_GetObjectItem(server, "hostname"));
      ver = cJSON_GetStringValue(cJSON_GetObjectItem(server, "version"));
    }
    bool has_voice = false;
    cJSON *feats = server ? cJSON_GetObjectItem(server, "features") : NULL;
    if (feats && cJSON_IsArray(feats)) {
      cJSON *f;
      cJSON_ArrayForEach(f, feats) {
        const char *s = cJSON_GetStringValue(f);
        if (s && !strcmp(s, "voice")) { has_voice = true; break; }
      }
    }
    app_set_status("已连上 Friday %s（%s）", ver ? ver : "", host ? host : "");
    if (has_voice) {
      // ★ 声纹：远端设备过不了这台 Friday 的声纹校验（不是登记人声就不交给助手，真机验证过：
      //   说话无反应就是这个）。推 settings.update 关掉声纹过滤（voiceOnlyEnrolled=false、严格度 0）。
      //   这改的是那台机器的全局语音设置，断开不自动恢复；要恢复去桌面端设置 › 能力 › 声纹。
      rpc_request("settings.update", "{\"voiceOnlyEnrolled\":false,\"voiceprintStrictness\":0}");
      char p[96];
      snprintf(p, sizeof(p), "{\"inputSampleRate\":%d,\"outputSampleRate\":%d}",
               VOICE_SAMPLE_RATE, VOICE_SAMPLE_RATE);
      g_rid_voice_start = rpc_request("voice.start", p);
      g_voice_on = true;
      ESP_LOGI(TAG, "voice.start sent（声纹过滤已关）id=%d", g_rid_voice_start);
    } else {
      app_set_status("这台 Friday 不支持语音（2.35+ 且本地语音可用才有）");
    }
  } else if (result && params == NULL) {
    // voice.start 的回执就是 voice.state
    cJSON *vs = cJSON_GetObjectItem(result, "state");
    if (vs || result) voice_state_apply(result);
  }
  cJSON_Delete(msg);
}

// ---------------- esp_peer ----------------

static int peer_on_state(esp_peer_state_t state, void *ctx)
{
  ESP_LOGI(TAG, "peer state %d", (int)state);
  switch (state) {
    case ESP_PEER_STATE_NEW_CONNECTION:
      break;
    case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED:
      // SCTP 关联建立后才能建通道（官方 local_jpeg_stream 同款时序；连接前建会 INVALID_STATE + 堆损坏）
      {
        static char label[] = "friday-work";   // esp_peer 要 char*，不收 const
        esp_peer_data_channel_cfg_t ch = {
          .type = ESP_PEER_DATA_CHANNEL_RELIABLE,
          .ordered = true,
          .label = label,
        };
        int rc = esp_peer_create_data_channel(g_peer, &ch);
        ESP_LOGI(TAG, "create_data_channel rc=%d", rc);
      }
      break;
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
      g_dc_open = true;
      app_set_stage(APP_RPC_HELLO);
      app_set_status("通道打开，hello…");
      {
        char hello[256];
        snprintf(hello, sizeof(hello),
          "{\"protocol\":%d,\"client\":{\"name\":\"Friday Device\",\"version\":\"%s\","
          "\"platform\":\"esp32\",\"kind\":\"device\"},\"features\":[]}",
          PROTOCOL_VERSION, APP_VERSION);
        g_rid_hello = rpc_request("hello", hello);
      }
      break;
    case ESP_PEER_STATE_CONNECT_FAILED:
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_DATA_CHANNEL_CLOSED:
    case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED:
      if (g_dc_open || state == ESP_PEER_STATE_CONNECT_FAILED) {
        g_dc_open = false;
        app_set_stage(APP_DIAL);   // 上层看到会退避重拨
        app_set_status("连接断了，准备重拨");
      }
      break;
    default:
      break;
  }
  return 0;
}

static int peer_on_msg(esp_peer_msg_t *msg, void *ctx)
{
  if (!g_accepted) return 0;
  if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
    // 本地 offer SDP → 转给对端
    char esc[8192];
    json_escape((const char *)msg->data, msg->size, esc, sizeof(esc));
    char *frame = (char *)malloc(strlen(esc) + 256);
    if (!frame) return 0;
    snprintf(frame, strlen(esc) + 256, "\"type\":\"offer\",\"payload\":\"%s\"", esc);
    send_p2p("p2p_signaling", frame);
    free(frame);
    ESP_LOGI(TAG, "offer sent (%d bytes)", msg->size);
  } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
    // 本地 candidate → 转给对端（包成 {"candidate":"..."}：Friday Work 的形状）
    char esc[512];
    json_escape((const char *)msg->data, msg->size, esc, sizeof(esc));
    char *frame = (char *)malloc(strlen(esc) + 256);
    if (!frame) return 0;
    snprintf(frame, strlen(esc) + 256,
             "\"type\":\"ice_candidate\",\"payload\":\"{\\\"candidate\\\":\\\"%s\\\"}\"", esc);
    send_p2p("p2p_signaling", frame);
    free(frame);
    ESP_LOGI(TAG, "candidate sent (%d bytes)", (int)strlen(esc));
  }
  return 0;
}

static int peer_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
  ESP_LOGI(TAG, "channel open '%s' (sid %u)", ch->label ? ch->label : "?", ch->stream_id);
  return 0;
}

static int peer_on_data(esp_peer_data_frame_t *frame, void *ctx)
{
  if (frame->type != ESP_PEER_DATA_CHANNEL_STRING) return 0;
  // NUL 终止后交给 RPC
  char *buf = (char *)malloc(frame->size + 1);
  if (!buf) return 0;
  memcpy(buf, frame->data, frame->size);
  buf[frame->size] = '\0';
  handle_rpc_message(buf, frame->size);
  free(buf);
  return 0;
}

static void peer_task(void *arg)
{
  ESP_LOGI(TAG, "peer task up");
  while (g_peer_task_run) {
    // 1. 处理跨任务命令
    peer_cmd_t cmd;
    while (xQueueReceive(g_cmd_q, &cmd, 0) == pdTRUE) {
      if (cmd.cmd == PCMD_OPEN) {
        ESP_LOGI(TAG, "PCMD_OPEN → peer_open_and_dial");
        peer_open_and_dial();   // 在这里做（本任务栈 32KB）；WS 任务只有 10KB，干不了这个
        continue;
      }
      if (!g_peer) continue;
      if (cmd.cmd == PCMD_SET_SDP) {
        esp_peer_msg_t m = {
          .type = ESP_PEER_MSG_TYPE_SDP,
          .data = (uint8_t *)cmd.payload,
          .size = (int)strlen(cmd.payload),
        };
        esp_peer_send_msg(g_peer, &m);
        ESP_LOGI(TAG, "remote SDP set (%d bytes)", m.size);
      } else if (cmd.cmd == PCMD_SET_CANDIDATE) {
        esp_peer_msg_t m = {
          .type = ESP_PEER_MSG_TYPE_CANDIDATE,
          .data = (uint8_t *)cmd.payload,
          .size = (int)strlen(cmd.payload),
        };
        esp_peer_send_msg(g_peer, &m);
      } else if (cmd.cmd == PCMD_STOP) {
        g_dc_open = false;   // 先降旗：其他任务（mic/UI）不再往正在关的通道写
        if (g_peer) {
          esp_peer_close(g_peer);
          g_peer = NULL;
        }
      }
    }
    // 2. feed 队列（每次循环最多发 2 帧：SCTP 发送路径拿锁重，一次发多了会押着 esp_peer 内部线程，
    //    中断看门狗饿死（真机：Interrupt wdt timeout，栈在 sctp_outgoing_data 等锁））
    char *feed;
    int sent = 0;
    while (sent < 2 && xQueueReceive(g_feed_q, &feed, 0) == pdTRUE) {
      if (g_peer && g_dc_open) {
        esp_peer_data_frame_t f = {
          .type = ESP_PEER_DATA_CHANNEL_STRING,
          .data = (uint8_t *)feed,
          .size = (int)strlen(feed),
        };
        int rc = esp_peer_send_data(g_peer, &f);
        if (rc == ESP_PEER_ERR_WOULD_BLOCK) { free(feed); break; }   // 缓冲满：这帧丢了，下轮再发新帧
        sent++;
      }
      free(feed);
    }
    // 3. 主循环（每次也让 esp_peer 内部线程跑：它要发 SCTP 重传/心跳）
    if (g_peer) esp_peer_main_loop(g_peer);
    vTaskDelay(pdMS_TO_TICKS(10));   // ★ 必须让出：同优先级/更高优先级的 WiFi/定时器线程要跑
  }
  if (g_peer) { esp_peer_close(g_peer); g_peer = NULL; }
  vTaskDelete(NULL);
}

static void peer_open_and_dial(void)
{
  ESP_LOGI(TAG, "peer_open_and_dial …");
  // 关旧连接
  g_dc_open = false;   // 先降旗，避免 mic/UI 任务往马上要关的通道写
  if (g_peer) {
    esp_peer_close(g_peer);
    g_peer = NULL;
  }

  esp_peer_default_cfg_t extra = {
    .agent_recv_timeout = 500,
    .data_ch_cfg = {
      .cache_timeout = 3000,
      // 缓存去 PSRAM（malloc 阈值改小后）：recv 要装得下 Friday 的单条大消息
      // （hello 全量 state 一条直达 200KB 内，WorkFramer 的 WHOLE_LIMIT）
      .send_cache_size = 64 * 1024,
      .recv_cache_size = 160 * 1024,
    },
    .rtp_cfg = {
      // 数据通道用不上 RTP：压掉默认的 100K+400K+400K 占用（peer_demo 同款）
      .audio_recv_jitter = { .cache_size = 1024 },
      .send_pool_size = 1024,
      .send_queue_num = 10,
    },
    .max_candidates = 8,
  };
  esp_peer_cfg_t cfg = {};
  cfg.role = ESP_PEER_ROLE_CONTROLLING;
  cfg.enable_data_channel = true;
  cfg.manual_ch_create = true;
  cfg.audio_dir = ESP_PEER_MEDIA_DIR_NONE;
  cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
  cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  // ★ 没有 ICE 服务器时 candidate 收集不启动，永远不出 offer（真机踩过：state 停在 2，30s 被对方挂断）
  //   与 Friday Work 的内置 STUN 同源；国内可达的优先（Google STUN 在国内不通）
  static esp_peer_ice_server_cfg_t ice_servers[] = {
    { .stun_url = (char *)"stun:stun.miwifi.com:3478" },
    { .stun_url = (char *)"stun:stun.qq.com:3478" },
  };
  cfg.server_lists = ice_servers;
  cfg.server_num = sizeof(ice_servers) / sizeof(ice_servers[0]);
  cfg.on_state = peer_on_state;
  cfg.on_msg = peer_on_msg;
  cfg.on_channel_open = peer_on_channel_open;
  cfg.on_data = peer_on_data;
  cfg.extra_cfg = &extra;
  cfg.extra_size = sizeof(extra);

  int rc = esp_peer_open(&cfg, esp_peer_get_default_impl(), &g_peer);
  if (rc != 0) {
    ESP_LOGE(TAG, "esp_peer_open rc=%d", rc);
    app_set_status("WebRTC 内存不够（rc=%d）", rc);
    return;
  }
  if (!g_peer_task_run) {
    g_peer_task_run = true;
    xTaskCreate(peer_task, "peer", 32 * 1024, NULL, 5, NULL);
  }
  // 主叫：new_connection 生成 offer（带 SCTP application）；DataChannel 在 SCTP 关联建立后
  // （peer_on_state 的 DATA_CHANNEL_CONNECTED）再建，连接前建会 INVALID_STATE + 堆损坏
  rc = esp_peer_new_connection(g_peer);
  ESP_LOGI(TAG, "new_connection rc=%d", rc);
}

// ---------------- 喂音 ----------------

void work_feed_audio(const int16_t *pcm, int samples)
{
  if (!g_dc_open) return;
  static uint32_t fed;
  if (++fed % 50 == 1) ESP_LOGI(TAG, "feed %u 帧…", fed);   // 5 秒一条，别刷屏
  // 电平（每秒一条）：判断麦克风是不是根本没收到声/太小（声纹不过、ASR 只听成“你好”时先看这个）
  {
    static uint32_t lv;
    int16_t peak = 0;
    for (int i = 0; i < samples; i++) {
      int16_t v = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
      if (v > peak) peak = v;
    }
    if (++lv % 10 == 1) ESP_LOGI(TAG, "[MIC] peak=%d（第 %u 帧）", peak, lv);
  }
  // 静音检测：全零帧不喂（省流量也省服务端听写额度）
  int64_t acc = 0;
  for (int i = 0; i < samples; i += 8) acc += pcm[i] < 0 ? -(int64_t)pcm[i] : pcm[i];
  if (acc < 32) {
    app_lock(); g_app.drops++; app_unlock();
    return;
  }
  char *json = (char *)malloc(VOICE_FEED_BYTES * 4 / 3 + 128);
  if (!json) return;
  int n = snprintf(json, 64, "{\"jsonrpc\":\"2.0\",\"method\":\"voice.feed\",\"params\":{\"pcm\":\"");
  n += b64_encode((const uint8_t *)pcm, samples * 2, json + n, VOICE_FEED_BYTES * 4 / 3 + 8);
  n += snprintf(json + n, 16, "\"}}");
  if (uxQueueMessagesWaiting(g_feed_q) >= 4) {
    // 积压：丢最新这帧（voice.feed 本来就允许丢）
    free(json);
    app_lock(); g_app.drops++; app_unlock();
    return;
  }
  char *q = json;
  xQueueSend(g_feed_q, &q, 0);
  app_lock(); g_app.feeds_sent++; app_unlock();
}

// ---------------- 信令事件（WS 任务上下文）----------------

static void on_ws_event(const char *event, const char *data, size_t len)
{
  if (strncmp(event, "p2p_", 4)) return;
  cJSON *env = cJSON_ParseWithLength(data, len);
  if (!env) return;
  cJSON *d = cJSON_GetObjectItem(env, "d");
  if (!d) { cJSON_Delete(env); return; }
  const char *req = cJSON_GetStringValue(cJSON_GetObjectItem(d, "requestId"));
  if (!req || strcmp(req, g_request_id)) { cJSON_Delete(env); return; }   // 不是这条会话

  if (!strcmp(event, "p2p_response")) {
    cJSON *acc = cJSON_GetObjectItem(d, "accepted");
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(d, "reason"));
    bool ok = acc && cJSON_IsTrue(acc);
    // ★ 先拷到栈里再释放 JSON：reason 指向 cJSON 内部，Delete 后用是 use-after-free（真机崩过）
    char reason_buf[64];
    strlcpy(reason_buf, reason ? reason : "", sizeof(reason_buf));
    cJSON_Delete(env);
    ESP_LOGI(TAG, "p2p_response accepted=%d reason=%s", ok, reason_buf);
    if (ok) {
      g_accepted = true;
      app_set_stage(APP_PEER_CONNECT);
      app_set_status("已接听，WebRTC 握手…");
      // ★ 不能在这里直接 peer_open_and_dial：这是 WS 任务（栈 4KB），
      //   esp_peer_open 的 DTLS/SRTP 初始化会撑爆它（真机卡死/崩溃在这）——交 peer 任务做
      peer_cmd_t c = { .cmd = PCMD_OPEN };
      xQueueSend(g_cmd_q, &c, 0);
    } else {
      g_dialing = false;
      g_accepted = false;
      // 换个 bot 能解的：清掉存的选择，回列表页重选
      if (!strcmp(reason_buf, "bot_no_p2p") || !strcmp(reason_buf, "not_friends") ||
          !strcmp(reason_buf, "offline") || !strcmp(reason_buf, "blocked")) {
        ESP_LOGW(TAG, "机器人拒绝（%s）：清选择回列表", reason_buf);
        store_clear_bot();
        // 注意：这里在 WS 事件任务里，不能 esp_websocket_client_stop（会死锁）；
        // 主循环的 APP_BOT_PICK 分支会走 ensure_voice_online，它开头会重建 WS
        app_set_status("这个机器人接不了（%s），换一个", reason_buf);
        app_set_stage(APP_BOT_PICK);
      } else {
        app_set_status("机器人拒绝（%s）", reason_buf);
        app_set_stage(APP_DIAL);
      }
    }
    return;
  }

  if (!strcmp(event, "p2p_signaling")) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
    const char *payload = cJSON_GetStringValue(cJSON_GetObjectItem(d, "payload"));
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(d, "reason"));
    char *type_c = type ? strdup(type) : nullptr;
    char *payload_c = payload ? strdup(payload) : nullptr;
    char *reason_c = reason ? strdup(reason) : nullptr;
    cJSON_Delete(env);   // 先释放，后面只用拷贝
    if (type_c && !strcmp(type_c, "answer") && payload_c) {
      peer_cmd_t c = { .cmd = PCMD_SET_SDP };
      strlcpy(c.payload, payload_c, sizeof(c.payload));
      xQueueSend(g_cmd_q, &c, 0);
    } else if (type_c && !strcmp(type_c, "ice_candidate") && payload_c) {
      peer_cmd_t c = { .cmd = PCMD_SET_CANDIDATE };
      c.payload[0] = '\0';
      cJSON *j = cJSON_Parse(payload_c);
      if (j) {
        const char *cand = cJSON_GetStringValue(cJSON_GetObjectItem(j, "candidate"));
        if (cand) strlcpy(c.payload, cand, sizeof(c.payload));
        cJSON_Delete(j);
      } else {
        strlcpy(c.payload, payload_c, sizeof(c.payload));
      }
      if (c.payload[0]) xQueueSend(g_cmd_q, &c, 0);
    } else if (type_c && (!strcmp(type_c, "hangup") || !strcmp(type_c, "request_cancel"))) {
      ESP_LOGW(TAG, "对方挂断：%s", reason_c ? reason_c : "");
      work_reset();
      app_set_stage(APP_DIAL);
      app_set_status("对方挂断，准备重拨");
    }
    free(type_c);
    free(payload_c);
    free(reason_c);
  }
}

// ---------------- 对外 ----------------

// peer 任务要 24KB 内部栈。等到拨号时再建会撞上 WSS+TLS 把 DRAM 吃得只剩十几 KB 的时刻
// （真机：24K/16K 都 create 失败 → WebRTC 永远起不来 → 30 秒被挂断）。
// 所以开机早期（work_bind_ws，WiFi 还没 start）就把它建好，占住那 24KB。
static void peer_task_ensure(void)
{
  if (g_peer_task_run) return;
  if (!g_cmd_q) {
    g_cmd_q = xQueueCreate(6, sizeof(peer_cmd_t));
    g_feed_q = xQueueCreate(6, sizeof(char *));
  }
  g_peer_task_run = true;
  BaseType_t ok = xTaskCreate(peer_task, "peer", 24 * 1024, NULL, 5, NULL);
  if (ok != pdPASS) ok = xTaskCreate(peer_task, "peer", 16 * 1024, NULL, 5, NULL);
  ESP_LOGW(TAG, "peer task create %s（内部堆余 %uB）", ok == pdPASS ? "ok" : "FAIL",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (ok != pdPASS) g_peer_task_run = false;
}

void work_start(int64_t bot_id)
{
  if (!g_cmd_q) {
    g_cmd_q = xQueueCreate(6, sizeof(peer_cmd_t));
    g_feed_q = xQueueCreate(6, sizeof(char *));
    sess_mu_init();
  }
  // peer 任务已在 work_bind_ws（开机早期）建好；万一那次失败（RAM 紧）这里再兜底试一次
  peer_task_ensure();
  work_reset();
  ESP_LOGI(TAG, "dial：内部堆余 %uB", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  g_bot_id = bot_id;
  random_hex(g_request_id, 32);
  g_dialing = true;
  char extra[256];
  snprintf(extra, sizeof(extra),
           "\"callType\":\"work\",\"expiresAt\":%llu",
           (unsigned long long)(now_ms64() + 30000));
  send_p2p("p2p_request", extra);
  app_set_stage(APP_DIAL);
  app_set_status("呼叫机器人（work）…");
  ESP_LOGI(TAG, "dial bot=%lld req=%s", (long long)bot_id, g_request_id);
}

void work_stop(void)
{
  if (g_dialing || g_accepted) {
    send_p2p("p2p_signaling", "\"type\":\"hangup\",\"payload\":\"\"");
  }
  work_reset();
}

void work_reset(void)
{
  g_dialing = false;
  g_accepted = false;
  g_request_id[0] = '\0';
  g_dc_open = false;
  g_voice_on = false;        // 语音会话随通道一起死：别让旧标志骗过 work_voice_held
  g_rid_voice_start = -1;    // （按 A 时会自己补 voice.start）
  g_rid_hello = -1;
  if (g_cmd_q) {
    peer_cmd_t c = { .cmd = PCMD_STOP };
    xQueueSend(g_cmd_q, &c, 0);
  }
}

void work_tap_interrupt(void)
{
  if (g_dc_open) {
    rpc_notify("voice.interrupt", "{}");
    audio_clear();
  }
}

// ---- v0.3 按键的语音语义（对齐桌面端 AssistantVoice：held 阻止自动交付，松开=草稿停在那）----

static void voice_start_now(void)
{
  char p[96];
  snprintf(p, sizeof(p), "{\"inputSampleRate\":%d,\"outputSampleRate\":%d}",
           VOICE_SAMPLE_RATE, VOICE_SAMPLE_RATE);
  g_rid_voice_start = rpc_request("voice.start", p);
  g_voice_on = true;
  ESP_LOGI(TAG, "voice.start（按 A 重开）id=%d", g_rid_voice_start);
}

void work_voice_held(bool held)
{
  if (!g_dc_open) return;
  if (held) {
    // 服务端答完一轮会自动 stop：没会话就先开一个再按住（不然按了也白按）
    if (!g_voice_on) voice_start_now();
    rpc_notify("voice.held", "{\"held\":true}");
  }
  // 松开不发 held=false：草稿留在服务端，等 B 发送 / C 丢弃
}

void work_voice_release(void)
{
  if (!g_dc_open) return;
  rpc_notify("voice.held", "{\"held\":false}");   // 定稿草稿 → 交给助手
}

void work_voice_discard(void)
{
  if (!g_dc_open) return;
  rpc_notify("voice.stop", "{}");
  g_voice_on = false;
}

void work_send_text(const char *conv_id, const char *text)
{
  if (!g_dc_open || !conv_id || !conv_id[0] || !text || !text[0]) return;
  char esc[1280];
  json_escape(text, strlen(text), esc, sizeof(esc));
  size_t cap = strlen(esc) + strlen(conv_id) + 128;
  char *buf = (char *)malloc(cap);
  if (!buf) return;
  snprintf(buf, cap, "{\"conversationID\":\"%s\",\"text\":\"%s\"}", conv_id, esc);
  int rid = rpc_request("runner.submit", buf);
  free(buf);
  ESP_LOGI(TAG, "runner.submit → 会话 %s（%d 字，id=%d）", conv_id, (int)strlen(esc), rid);
}

void work_activity_sub(void)
{
  if (!g_dc_open) return;
  sess_mu_init();
  g_rid_activity = rpc_request("activity.subscribe", "{}");
  ESP_LOGI(TAG, "activity.subscribe id=%d", g_rid_activity);
}

void work_activity_unsub(void)
{
  if (!g_dc_open) return;
  rpc_notify("activity.unsubscribe", "{}");
}

int work_sessions_copy(work_session_t *out, int cap)
{
  if (!g_sess_mu || !out || cap <= 0) return 0;
  xSemaphoreTake(g_sess_mu, portMAX_DELAY);
  int n = g_sessions_n < cap ? g_sessions_n : cap;
  memcpy(out, g_sessions, (size_t)n * sizeof(work_session_t));
  xSemaphoreGive(g_sess_mu);
  return n;
}

uint32_t work_sessions_version(void)
{
  if (!g_sess_mu) return 0;
  xSemaphoreTake(g_sess_mu, portMAX_DELAY);
  uint32_t v = g_sessions_ver;
  xSemaphoreGive(g_sess_mu);
  return v;
}

bool work_assistant_enabled(void)
{
  if (!g_sess_mu) return false;
  xSemaphoreTake(g_sess_mu, portMAX_DELAY);
  bool ok = g_assistant_ok;
  xSemaphoreGive(g_sess_mu);
  return ok;
}

bool work_dc_open(void) { return g_dc_open; }
const char *work_request_id(void) { return g_request_id; }

void work_bind_ws(void)
{
  sess_mu_init();
  peer_task_ensure();   // 开机就把 peer 任务建好（见上面注释：等拨号时建会因 DRAM 紧而失败）
  schat_ws_on_event(on_ws_event);
}

#ifdef UI_DUMP
// 调试构建专用：填几个假会话，好把会话列表页的版式渲染出来看
extern "C" void work_dump_seed_sessions(void)
{
  static const char *titles[] = {
    "重构 Friday Work 的登录流程",
    "帮我看看这个 ESP32 的 I2S 时钟为什么和麦克风冲突",
    "写一份 v0.3 的发布说明",
    "翻译这篇论文",
    "这个标题特别特别长用来验证截断到底截在哪里看看能不能看清",
  };
  static const char *sts[] = { "running", "approval", "question", "idle", "error" };
  sess_mu_init();
  xSemaphoreTake(g_sess_mu, portMAX_DELAY);
  g_sessions_n = 0;
  for (int i = 0; i < 5; i++) {
    snprintf(g_sessions[i].id, sizeof(g_sessions[i].id), "conv-%d", 1000 + i);
    strlcpy(g_sessions[i].title, titles[i], sizeof(g_sessions[i].title));
    strlcpy(g_sessions[i].status, sts[i], sizeof(g_sessions[i].status));
    g_sessions_n++;
  }
  g_assistant_ok = true;
  g_sessions_ver++;
  xSemaphoreGive(g_sess_mu);
}
#endif
