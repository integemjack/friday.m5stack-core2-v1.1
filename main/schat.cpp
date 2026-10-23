#include "schat.h"
#include "store.h"
#include "util.h"
#include "app.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "schat";
// WS 状态（真身在后面；find_bot 在声明前就要用）
static volatile bool g_ws_kicked;
#define DEVICE_PLATFORM "desktop"   // chatserver 认的枚举（Friday Work 同款）

// ================= HTTP =================

static char g_http_buf[12288];   // friends 列表可能几 KB
static char g_http_url[256];

static int http_json(const char *method, const char *path, const char *body, const char *bearer)
{
  snprintf(g_http_url, sizeof(g_http_url), "%s%s", g_store.server, path);
  esp_http_client_config_t cfg = {};
  cfg.url = g_http_url;
  cfg.timeout_ms = 15000;
  cfg.skip_cert_common_name_check = true;
  cfg.keep_alive_enable = true;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -1;
  esp_http_client_set_method(c,
    strcmp(method, "GET") == 0 ? HTTP_METHOD_GET : HTTP_METHOD_POST);
  if (bearer) esp_http_client_set_header(c, "Authorization", bearer);
  esp_http_client_set_header(c, "Content-Type", "application/json");
  int post_len = body ? strlen(body) : 0;
  if (post_len) esp_http_client_set_post_field(c, body, post_len);
  esp_err_t err = esp_http_client_open(c, post_len);
  int status = -1;
  if (err == ESP_OK) {
    if (post_len) esp_http_client_write(c, body, post_len);
    int64_t rd = esp_http_client_fetch_headers(c);
    int n = 0;
    if (rd > 0) {
      int remain = (int)rd;
      if (remain > (int)sizeof(g_http_buf) - 1) remain = sizeof(g_http_buf) - 1;
      while (n < remain) {
        int got = esp_http_client_read(c, g_http_buf + n, remain - n);
        if (got <= 0) break;
        n += got;
      }
    } else {
      int got = esp_http_client_read(c, g_http_buf, sizeof(g_http_buf) - 1);
      if (got > 0) n = got;
    }
    g_http_buf[n] = '\0';
    status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
  } else {
    g_http_buf[0] = '\0';   // 失败时清掉旧响应，调用方不会拿到上一次的残留
  }
  esp_http_client_cleanup(c);
  if (err != ESP_OK) ESP_LOGW(TAG, "HTTP %s %s open 失败 err=0x%x", method, path, err);
  ESP_LOGI(TAG, "HTTP %s %s -> %d (%d bytes)", method, path, status, (int)strlen(g_http_buf));
  return status;
}

// 取轮换后的 refreshToken（服务端字段名不统一：refreshToken / refresh / newRefreshToken）
static const char *pick_refresh(cJSON *j)
{
  static const char *keys[] = { "refreshToken", "refresh", "newRefreshToken" };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, keys[i]));
    if (v && v[0]) return v;
  }
  return NULL;
}

static bool status_ok(int s) { return s >= 200 && s < 300; }

// 拿到一份凭据（poll approved / refresh）：存 token+refresh，按 JWT 真实时长算死线
// ★ 以前这里写死「legacy 7 天、6 天后才刷」，实际服务端发的是 15 分钟的 access JWT：
//   15 分钟后连 WS 直接 auth_expired，旧代码还顺手把 refreshToken 一起清掉 → 每次都得重扫码。
static bool auth_apply(const char *tok, const char *access, const char *refresh,
                       cJSON *uid, cJSON *expires_in, const char *from)
{
  const char *use = (tok && tok[0]) ? tok : (access && access[0] ? access : NULL);
  if (!use) return false;
  strlcpy(g_store.token, use, sizeof(g_store.token));
  if (refresh && refresh[0]) strlcpy(g_store.refresh, refresh, sizeof(g_store.refresh));
  if (uid && cJSON_IsNumber(uid)) g_store.user_id = (int64_t)uid->valuedouble;

  // 时长：优先 JWT 的 exp-iat（不依赖本机壁钟）；否则 expiresIn；最后按 15 分钟保守算
  int64_t exp = jwt_exp_ms(use), iat = jwt_iat_ms(use);
  int64_t life_ms = (exp > 0 && iat > 0 && exp > iat) ? (exp - iat) : 0;
  if (life_ms <= 0 && expires_in && cJSON_IsNumber(expires_in))
    life_ms = (int64_t)(expires_in->valuedouble * 1000);
  if (life_ms <= 0) life_ms = 15 * 60 * 1000;
  g_store.expires_at_ms = (int64_t)now_ms64() + life_ms;
  store_save_auth();

  char typ[24];
  jwt_type(use, typ, sizeof(typ));
  ESP_LOGI(TAG, "凭据就绪（%s）：uid=%lld typ=%s 有效期 %lld 分钟 refresh=%s",
           from, (long long)g_store.user_id, typ[0] ? typ : "?",
           (long long)(life_ms / 60000), g_store.refresh[0] ? "有" : "无");
  return true;
}

bool schat_create_qr_session(char *out_token, size_t cap, int *out_interval_ms)
{
  char body[256];
  snprintf(body, sizeof(body),
           "{\"deviceId\":\"%s\",\"devicePlatform\":\"%s\",\"deviceName\":\"%s\"}",
           g_store.device_id, DEVICE_PLATFORM, DEVICE_NAME);
  int s = http_json("POST", "/login-sessions", body, NULL);
  if (!status_ok(s)) return false;
  cJSON *j = cJSON_Parse(g_http_buf);
  if (!j) return false;
  const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "token"));
  bool ok = tok && tok[0];
  if (ok) {
    strlcpy(out_token, tok, cap);
    cJSON *iv = cJSON_GetObjectItem(j, "intervalMs");
    *out_interval_ms = (iv && cJSON_IsNumber(iv)) ? (int)iv->valuedouble : 2000;
  }
  cJSON_Delete(j);
  return ok;
}

qr_status_t schat_poll_qr(const char *token)
{
  char body[320];
  snprintf(body, sizeof(body), "{\"token\":\"%s\",\"deviceId\":\"%s\"}", token, g_store.device_id);
  int s = http_json("POST", "/login-sessions/poll", body, NULL);
  if (s == 404) return QR_EXPIRED;
  if (!status_ok(s)) return QR_ERROR;
  cJSON *j = cJSON_Parse(g_http_buf);
  if (!j) return QR_ERROR;
  const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(j, "status"));
  qr_status_t r = QR_PENDING;
  if (st && !strcmp(st, "scanned")) r = QR_SCANNED;
  else if (st && !strcmp(st, "approved")) {
    const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "token"));
    const char *acc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "accessToken"));
    const char *rfr = pick_refresh(j);
    cJSON *uid = cJSON_GetObjectItem(j, "userId");
    cJSON *exp = cJSON_GetObjectItem(j, "expiresIn");
    r = auth_apply(tok, acc, rfr, uid, exp, "扫码登录") ? QR_APPROVED : QR_ERROR;
  }
  cJSON_Delete(j);
  return r;
}

schat_refresh_t schat_refresh(void)
{
  if (!g_store.refresh[0]) return SCHAT_REFRESH_NO_TOKEN;
  char body[1200];
  snprintf(body, sizeof(body), "{\"refreshToken\":\"%s\"}", g_store.refresh);
  int s = http_json("POST", "/auth/refresh", body, NULL);
  if (s == 401 || s == 403 || s == 404) {
    ESP_LOGW(TAG, "refresh 被拒（HTTP %d）：凭据已作废", s);
    return SCHAT_REFRESH_AUTH;
  }
  if (!status_ok(s)) return SCHAT_REFRESH_NET;
  cJSON *j = cJSON_Parse(g_http_buf);
  if (!j) return SCHAT_REFRESH_NET;
  const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "token"));
  const char *acc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "accessToken"));
  const char *rfr = pick_refresh(j);
  cJSON *uid = cJSON_GetObjectItem(j, "userId");
  cJSON *exp = cJSON_GetObjectItem(j, "expiresIn");
  bool ok = auth_apply(tok, acc, rfr, uid, exp, "refresh");
  cJSON_Delete(j);
  return ok ? SCHAT_REFRESH_OK : SCHAT_REFRESH_AUTH;
}

int64_t schat_token_deadline_ms(void) { return g_store.expires_at_ms; }

// 好友里的 bot 列表（最多 8 个）。返回个数；-1 = 拉取失败/凭据失效（置 kicked）
int schat_list_bots(int64_t *ids, char (*names)[48], int max)
{
  char auth[600];
  snprintf(auth, sizeof(auth), "Bearer %s", g_store.token);
  int s = http_json("GET", "/friends?include=all", NULL, auth);
  if (s == 401) {
    ESP_LOGW(TAG, "friends 401：凭据失效，需重新扫码");
    g_ws_kicked = true;
    return -1;
  }
  if (!status_ok(s)) return -1;
  cJSON *arr = cJSON_Parse(g_http_buf);
  if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return -1; }
  int n = 0;
  char in_names[8][48];   // 内部名（name 字段）另存一份，重名时用于区分
  cJSON *it;
  cJSON_ArrayForEach(it, arr) {
    cJSON *isBot = cJSON_GetObjectItem(it, "isBot");
    if (!isBot || !cJSON_IsTrue(isBot)) continue;
    cJSON *id = cJSON_GetObjectItem(it, "id");
    if (!id || !cJSON_IsNumber(id)) continue;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "nickname"));
    // 备注名在 relation.ext.remark（include=all 才带；Friday Work 同款优先级：备注 > 昵称 > 内部名）
    cJSON *rel = cJSON_GetObjectItem(it, "relation");
    const char *remark = NULL;
    if (rel) {
      cJSON *ext = cJSON_GetObjectItem(rel, "ext");
      if (ext) remark = cJSON_GetStringValue(cJSON_GetObjectItem(ext, "remark"));
    }
    if (remark && remark[0]) name = remark;
    if (!name || !name[0]) name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
    if (!name || !name[0]) name = "?";
    const char *iname = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
    if (n < max) {
      ids[n] = (int64_t)id->valuedouble;
      strlcpy(names[n], name, 48);
      strlcpy(in_names[n], iname ? iname : "", 48);
      n++;
    }
  }
  cJSON_Delete(arr);
  // 同名机器人（比如昵称都叫「Friday 助手」）用内部名区分，否则用户分不出谁是谁
  // （friends API 只有 name/nickname，手机里设置的备注名不上报）
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      if (strcmp(names[i], names[j]) == 0 && in_names[j][0]) {
        char tmp[96];
        snprintf(tmp, sizeof(tmp), "%s · %s", names[j], in_names[j]);
        strlcpy(names[j], tmp, 48);
      }
    }
  }
  return n;
}

// ================= WebSocket =================

static esp_websocket_client_handle_t g_ws;
static volatile bool g_ws_online;
static schat_ev_fn g_event_fn;
static schat_ws_cb g_reconn_cb;
static void (*g_pump)(void);
static int g_ack_id;
static char g_ws_rx[16384];   // 分片攒帧
static size_t g_ws_rx_len;

static void ws_send_envelope(const char *event, const char *data_json, bool with_ack)
{
  if (!g_ws) return;   // 不查 online：hello 本身就是在 connected 后、ack 前发的
  size_t cap = strlen(event) + strlen(data_json) + 128;
  char *buf = (char *)malloc(cap);
  if (!buf) return;
  int len;
  // ★ a（ackId）必须是字符串：chatserver 的 Envelope.A 是 string，数字会让整帧解析失败（真机踩过："first frame must be hello" + close 1008）
  if (with_ack)
    len = snprintf(buf, cap, "{\"e\":\"%s\",\"d\":%s,\"a\":\"%d\",\"t\":\"req\"}", event, data_json, ++g_ack_id);
  else
    len = snprintf(buf, cap, "{\"e\":\"%s\",\"d\":%s,\"t\":\"ev\"}", event, data_json);
  if (len > 0) esp_websocket_client_send_text(g_ws, buf, len, pdMS_TO_TICKS(3000));
  free(buf);
}

bool schat_ws_request(const char *event, const char *data_json)
{
  if (!g_ws || !g_ws_online) return false;
  ws_send_envelope(event, data_json, true);
  return true;
}

bool schat_ws_send_raw(const char *frame, size_t len)
{
  if (!g_ws || !g_ws_online) return false;
  return esp_websocket_client_send_text(g_ws, frame, (int)len, pdMS_TO_TICKS(5000)) >= 0;
}

static void ws_send_hello(void)
{
  char d[1400];
  snprintf(d, sizeof(d),
           "{\"v\":1,\"token\":\"%s\",\"deviceId\":\"%s\",\"devicePlatform\":\"%s\",\"deviceName\":\"%s\"}",
           g_store.token, g_store.device_id, DEVICE_PLATFORM, DEVICE_NAME);
  ws_send_envelope("hello", d, true);
}

static void ws_handle_frame(const char *data, size_t len)
{
  cJSON *env = cJSON_ParseWithLength(data, len);
  if (!env) return;
  const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(env, "e"));
  const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(env, "t"));
  if (e && !strcmp(e, "auth_expired")) {
    g_ws_kicked = true;
    g_ws_online = false;
    ESP_LOGW(TAG, "auth_expired：被踢，需重新扫码");
    cJSON_Delete(env);
    return;
  }
  if (t && !strcmp(t, "err")) {
    // hello 被拒（凭据失效）等服务端错误：置 kicked 让上层清凭据重新扫码
    const char *code = NULL;
    cJSON *d = cJSON_GetObjectItem(env, "d");
    if (d) code = cJSON_GetStringValue(cJSON_GetObjectItem(d, "code"));
    ESP_LOGW(TAG, "服务端 err：%s", code ? code : "?");
    if (code && (!strcmp(code, "auth_expired") || !strcmp(code, "unauthorized"))) {
      g_ws_kicked = true;
      g_ws_online = false;
    }
    cJSON_Delete(env);
    return;
  }
  if (t && !strcmp(t, "ack")) {
    // hello 的 ack（e=self）才算鉴权通过
    const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(env, "e"));
    if (ae && !strcmp(ae, "self")) {
      g_ws_online = true;
      if (g_reconn_cb) g_reconn_cb();
    }
    cJSON_Delete(env);
    return;   // 其它 ack 不用
  }
  if (e && g_event_fn) g_event_fn(e, data, len);
  cJSON_Delete(env);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
  esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WS connected");
      g_ws_rx_len = 0;
      ws_send_hello();   // online 要等 hello 的 ack（下面 t==ack 分支置位）；被拒会 t==err
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      g_ws_online = false;
      ESP_LOGW(TAG, "WS disconnected（close code=%d）", ev->close_status_code);
      break;
    case WEBSOCKET_EVENT_DATA:
      if (ev->op_code == 0x08) break;   // close
      if (ev->data_len <= 0) break;
      // 分片攒帧（payload_len == 总长）
      if (g_ws_rx_len + ev->data_len < sizeof(g_ws_rx)) {
        memcpy(g_ws_rx + g_ws_rx_len, ev->data_ptr, ev->data_len);
        g_ws_rx_len += ev->data_len;
        if (g_ws_rx_len >= (size_t)ev->payload_len) {
          ws_handle_frame(g_ws_rx, g_ws_rx_len);
          g_ws_rx_len = 0;
        }
      } else {
        ESP_LOGE(TAG, "WS 帧太大，丢弃");
        g_ws_rx_len = 0;
      }
      break;
    default:
      break;
  }
  // 重连成功且 hello 过 → 通知上层重拨（在 WS 事件任务里直接回调，上层只置状态）
  if (id == WEBSOCKET_EVENT_CONNECTED && g_reconn_cb) g_reconn_cb();
}

bool schat_ws_start(void)
{
  if (g_ws) schat_ws_stop();
  g_ws_kicked = false;
  g_ws_online = false;
  // https://host:port → wss://host:port/ws
  const char *srv = g_store.server;
  char *url = (char *)malloc(strlen(srv) + 16);
  if (!url) return false;
  const char *rest = strstr(srv, "://");
  rest = rest ? rest + 3 : srv;
  bool tls = strncmp(srv, "http://", 7) != 0;
  snprintf(url, strlen(srv) + 16, "%s%s/ws", tls ? "wss://" : "ws://", rest);
  ESP_LOGI(TAG, "WS connect %s", url);
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url;
  cfg.subprotocol = "schat-ws.v1";
  cfg.buffer_size = 16384;
  cfg.task_stack = 12 * 1024;   // 挂断处理路径（JSON+拼帧+stop）眈过栈；malloc 改 PSRAM 后内部 RAM 宽裕了
  cfg.reconnect_timeout_ms = 3000;
  cfg.network_timeout_ms = 12000;
  cfg.skip_cert_common_name_check = true;
  // 保活：NAT/负载均衡会悄无声息地回收空闲 TCP（之前掉线就是这么来的）
  cfg.ping_interval_sec = 20;
  cfg.pingpong_timeout_sec = 60;
  cfg.disable_pingpong_discon = false;
  g_ws = esp_websocket_client_init(&cfg);
  free(url);
  if (!g_ws) return false;
  esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
  esp_websocket_client_start(g_ws);
  // 等 hello ack（上限 12 秒；等待期间泵 UI，否则屏幕和触摸冻 12 秒）
  for (int i = 0; i < 120 && !g_ws_online && !g_ws_kicked; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (g_pump) g_pump();
  }
  return g_ws_online;
}

void schat_ws_stop(void)
{
  if (g_ws) {
    esp_websocket_client_stop(g_ws);
    esp_websocket_client_destroy(g_ws);
    g_ws = NULL;
  }
  g_ws_online = false;
}

bool schat_ws_online(void) { return g_ws_online; }
bool schat_ws_kicked(void) { return g_ws_kicked; }
void schat_ws_on_event(schat_ev_fn fn) { g_event_fn = fn; }
void schat_ws_on_reconnected(schat_ws_cb cb) { g_reconn_cb = cb; }
void schat_set_pump(void (*fn)(void)) { g_pump = fn; }
