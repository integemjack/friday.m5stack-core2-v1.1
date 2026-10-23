// app_main.cpp — M5Stack Core2 v1.1 · Friday 语音终端
//
// 状态机：
//   BOOT → (无WiFi) PORTAL → WIFI_WAIT → (无token) QR_PAIR → WS_CONNECT
//        → DIAL → PEER_CONNECT → RPC_HELLO → VOICE_START → LIVE
//   断线：DC 断 → DIAL 重拨（退避）；WS 断 → WS_CONNECT；kicked → QR_PAIR
#include "app.h"
#include "store.h"
#include "schat.h"
#include "work.h"
#include "audio_io.h"
#include "ui.h"
#include "portal.h"
#include "ota.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "util.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "main";

app_state_t g_app;
portMUX_TYPE g_app_mux = portMUX_INITIALIZER_UNLOCKED;

// 重连退避（连上就归零）
static uint32_t g_dial_backoff_ms = 2000;

void app_set_stage(app_stage_t s)
{
  app_lock();
  g_app.stage = s;
  app_unlock();
}

void app_set_status(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  app_lock();
  vsnprintf(g_app.status_text, sizeof(g_app.status_text), fmt, ap);
  app_unlock();
  va_end(ap);
  ESP_LOGI(TAG, "status: %s", g_app.status_text);
}

void app_set_caption(const char *utf8)
{
  app_lock();
  strlcpy(g_app.caption, utf8 ? utf8 : "", sizeof(g_app.caption));
  app_unlock();
}

// ---------------- WiFi ----------------

static EventGroupHandle_t g_wifi_ev;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (id == WIFI_EVENT_STA_DISCONNECTED) {
      static uint8_t retry;
      if (retry < 20) {
        retry++;
        esp_wifi_connect();
      }
      xEventGroupClearBits(g_wifi_ev, WIFI_CONNECTED_BIT);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(g_wifi_ev, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "WiFi got ip");
  }
}

static bool wifi_connect(uint32_t timeout_ms)
{
  wifi_config_t wc = {};
  strlcpy((char *)wc.sta.ssid, g_store.wifi_ssid, sizeof(wc.sta.ssid));
  strlcpy((char *)wc.sta.password, g_store.wifi_pass, sizeof(wc.sta.password));
  wc.sta.pmf_cfg.capable = true;
  wc.sta.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  // 语音流不平省电：modem sleep 会在大流量时把中断看门狗饿死（Interrupt wdt timeout on CPU0，真机跴过）
  esp_wifi_set_ps(WIFI_PS_NONE);
  EventBits_t bits = xEventGroupWaitBits(g_wifi_ev, WIFI_CONNECTED_BIT,
                                         pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void housekeeping_update(void)
{
  // 电池 / RSSI（1s 一次）
  static uint32_t last;
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (now - last < 1000) return;
  last = now;
  int pct = M5.Power.getBatteryLevel();
  wifi_ap_record_t ap;
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
  app_lock();
  g_app.battery_pct = pct;
  g_app.rssi = rssi;
  g_app.charging = M5.Power.isCharging();
  app_unlock();
  // token 快过期：提前刷（登录发的凭据 7 天，refresh 换来的可能只有 15 分钟）
  // 通话中不刷：轮换可能在服务端把当前会话作废，等空闲/重连时再刷。
  // 刷新失败不清凭据（Friday Work 同理）：旧 token 还能用就继续用，真被服务端拒了再重扫。
  static uint32_t next_try;
  int64_t deadline = schat_token_deadline_ms();
  app_stage_t cur; app_lock(); cur = g_app.stage; app_unlock();
  if (g_store.refresh[0] && deadline && cur != APP_LIVE &&
      (int64_t)now_ms64() + 120000 >= deadline &&
      (int32_t)(now - next_try) >= 0) {
    ESP_LOGW(TAG, "token 还剩 %lld 秒，提前刷新", (long long)((deadline - (int64_t)now_ms64()) / 1000));
    schat_refresh_t r = schat_refresh();
    if (r != SCHAT_REFRESH_OK) {
      next_try = now + 60000;   // 失败别一秒一刷；真过期后连 WS 会再试一次，还不行才重扫
      if ((int64_t)now_ms64() > deadline) ESP_LOGW(TAG, "凭据已过期且刷新不成，等 WS 判定");
    }
  }
}

// ---------------- 主流程 ----------------

// 只在快过期时 refresh（登录发的凭据是 7 天；refresh 换来的可能只有 15 分钟，
// 无脑每次刷会把凭据降级 —— 旧固件就是这么 15 分钟后就挂的）。
// 失败不致命：旧 token 大概率还能用（Friday Work 同款：刷新失败就用旧 token 试）
static bool auth_refresh_if_needed(bool force)
{
  if (!g_store.refresh[0]) return false;
  int64_t dl = schat_token_deadline_ms();
  if (!force && (!dl || (int64_t)now_ms64() + 120000 < dl)) return false;
  schat_refresh_t r = schat_refresh();
  if (r == SCHAT_REFRESH_OK) return true;
  ESP_LOGW(TAG, "refresh 失败（%d），继续用现有 token", (int)r);
  return false;
}

static bool ensure_voice_online(void)
{
  // WS + WebRTC + voice 全链路；返回 false = 要从 WS 层重来
  app_set_stage(APP_WS_CONNECT);
  if (!g_store.token[0]) {
    store_clear_auth();
    app_set_status("需要扫码登录");
    app_set_stage(APP_QR_PAIR);
    return false;
  }
  auth_refresh_if_needed(false);   // 只在临期时刷
  app_set_status("连 chatserver…");
  ESP_LOGI(TAG, "用凭据：uid=%lld dev=%s", (long long)g_store.user_id, g_store.device_id);
  bool online = schat_ws_start();
  if (!online && schat_ws_kicked() && auth_refresh_if_needed(true)) {
    // hello 被拒：服务端可能轮换过凭据，换一份再试一次（以前这步直接清凭据重扫码）
    ESP_LOGW(TAG, "换过凭据后重试 WS");
    online = schat_ws_start();
  }
  if (!online) {
    if (schat_ws_kicked()) {
      // token 被拒且换凭据也救不回来：现在才真的需要重扫
      store_clear_auth();
      app_set_status("登录已失效，请重新扫码");
      app_set_stage(APP_QR_PAIR);
    } else {
      app_set_status("连不上 chatserver，稍后重试");
      app_set_stage(APP_WS_CONNECT);
    }
    return false;
  }
  // bot 选择：没存过 → 列出来让用户点；存过 → 直接用
  int64_t bot = g_store.bot_user_id;
  char name[64];
  strlcpy(name, g_store.bot_name, sizeof(name));
  if (!bot) {
    static int64_t ids[8];
    static char names[8][48];
    schat_ws_stop();   // 选择期间不占连接
    app_set_stage(APP_BOT_PICK);
    int n = -1;
    while (n < 0) {
      n = schat_list_bots(ids, names, 8);
      if (n < 0) {
        if (schat_ws_kicked()) {
          store_clear_auth();
          app_set_status("凭据失效，请重新扫码");
          app_set_stage(APP_QR_PAIR);
          return false;
        }
        app_set_status("拉好友列表失败，重试…");
        vTaskDelay(pdMS_TO_TICKS(3000));
      }
    }
    if (n == 0) {
      app_set_status("好友里没有机器人，5 秒后重试");
      vTaskDelay(pdMS_TO_TICKS(5000));
      return false;
    }
    // 重新连 WS（拉列表时停了），再选
    app_set_status("连 chatserver…");
    if (!schat_ws_start()) {
      if (schat_ws_kicked()) { store_clear_auth(); app_set_stage(APP_QR_PAIR); }
      else app_set_stage(APP_WS_CONNECT);
      return false;
    }
    ui_pick_bot(names, n);   // 屏幕列出，等用户点
    int pick = -1;
    while (pick < 0) {
      M5.update();       // 触摸采样要主循环驱动；这里阻塞在选人，得自己刷
      ui_tick();
      vTaskDelay(pdMS_TO_TICKS(30));
      pick = ui_pick_result();
    }
    bot = ids[pick];
    strlcpy(name, names[pick], sizeof(name));
    ui_pick_reset();
    g_store.bot_user_id = bot;
    strlcpy(g_store.bot_name, name, sizeof(g_store.bot_name));
    store_save_bot();
    ESP_LOGI(TAG, "用户选了 bot=%lld %s", (long long)bot, name);
  }
  app_lock();
  g_app.bot_user_id = bot;
  strlcpy(g_app.bot_name, name, sizeof(g_app.bot_name));
  app_unlock();
  if (bot != g_store.bot_user_id) {
    g_store.bot_user_id = bot;
    strlcpy(g_store.bot_name, name, sizeof(g_store.bot_name));
    store_save_bot();
  }
  // p2p 拨号（重试在 work/peer 状态回调里驱动）
  work_start(bot);
  // 等 DC 打开（上限 45s；answer/ICE 都在里面）
  uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
  while (xTaskGetTickCount() * portTICK_PERIOD_MS - start < 45000) {
    if (work_dc_open()) break;
    if (!schat_ws_online()) { work_stop(); schat_ws_stop(); app_set_stage(APP_WS_CONNECT); return false; }
    app_stage_t cur; app_lock(); cur = g_app.stage; app_unlock();
    if (cur == APP_BOT_PICK || cur == APP_QR_PAIR) return false;   // 被拒改道：去列表页/重扫
    M5.update();          // 触摸不能停（否则「停」键死掉）
    ui_tick();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!work_dc_open()) {
    app_set_status("WebRTC 连不上，重试");
    work_stop();
    schat_ws_stop();
    app_set_stage(APP_WS_CONNECT);
    return false;
  }
  app_set_stage(APP_VOICE_START);
  audio_start();
  // voice.start 的回执会带 state；等 LIVE（顺带等 voice.start 的回执，别在语音还没就续时就宣布就续）
  for (int i = 0; i < 80; i++) {
    app_stage_t st;
    bool va;
    app_lock(); st = g_app.stage; va = g_app.voice_available; app_unlock();
    if (st == APP_LIVE || va) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  app_set_stage(APP_LIVE);
  g_dial_backoff_ms = 2000;   // 连上了：退避归零（否则断几次之后每次要等 30 秒）
  bool voice_ok;
  app_lock(); voice_ok = g_app.voice_available; app_unlock();
  if (voice_ok) app_set_status("按住 A 键说话，松开结束；B 键发送");
  else app_set_status("已连上，但这台 Friday 的语音不可用");
  ESP_LOGI(TAG, "LIVE（voice=%d）", voice_ok);
  return true;
}

// WS 断线重连回调（WS 任务上下文）：置标志，主循环处理
static volatile bool g_ws_reconnected;
static void on_ws_reconnected(void) { g_ws_reconnected = true; }

// schat 等 hello 回执时的泵（否则屏幕/触摸冻十几秒）
static void ui_pump(void)
{
  M5.update();
  ui_tick();
}

static void ui_hold(bool down)
{
  ESP_LOGI(TAG, "hold %s", down ? "DOWN" : "UP");
  audio_hold(down);      // 半双工：按下开麦（扬声器让出 GPIO0），松开关
  work_voice_held(down); // 按下：held=true；松开不发 held=false——草稿留在服务端，等 B/C 处置
  app_lock(); g_app.held = down; app_unlock();
}

// 键 B：发送草稿（发给助手=交给它语音回答；发给会话=runner.submit）
static void ui_send(void)
{
  app_stage_t st; bool held;
  char draft[512];
  app_lock(); st = g_app.stage; held = g_app.held; strlcpy(draft, g_app.caption, sizeof(draft)); app_unlock();
  if (st != APP_LIVE) { app_set_status("还没连上 Friday"); return; }
  if (held) { app_set_status("还在按着，松开再发"); return; }
  if (!draft[0]) { app_set_status("先按住 A 键说点什么"); return; }
  bool assist; char id[40];
  ui_get_target(&assist, id, sizeof(id));
  if (assist) {
    work_voice_release();   // voice.held false：服务端定稿草稿 → 交给助手
    app_set_status("已交给助手，等它回答");
  } else {
    work_send_text(id, draft);
    work_voice_discard();   // 草稿发走了，服务端那份清掉
    app_set_caption(NULL);
    app_set_status("已发到会话：%s", ui_target_label());
  }
}

// 点屏幕中间图标（原来的右键动作挪到这里）：有草稿=丢弃，没草稿=打断播报/思考
static void ui_stop(void)
{
  char draft[512];
  app_lock(); strlcpy(draft, g_app.caption, sizeof(draft)); app_unlock();
  if (draft[0]) {
    work_voice_discard();
    app_set_caption(NULL);
    app_set_status("已丢弃");
  } else {
    work_tap_interrupt();
    app_set_status("已打断");
  }
}

// 音量页：设置音量（0..255，M5 口径）
static void ui_volume_set(int v)
{
  extern void audio_set_volume(int);
  audio_set_volume(v);
  g_store.volume = v;
  store_save_volume();
}

static void ui_sessions_open(void) { work_activity_sub(); }
static void ui_sessions_close(void) { work_activity_unsub(); }

// 点顶部状态栏：检查更新（有新版就下载安装重启）
static void ui_update(void)
{
  if (!ota_check_now()) app_set_status("更新任务忙，稍等");
}

extern "C" void app_main(void)
{
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_spk = false;   // 喇叭由 audio_io 自己开 I2S1（M5Unified Speaker 块边界会顿挫）
  cfg.internal_mic = true;
  M5.begin(cfg);
  M5.setTouchButtonHeight(40);   // ★ 启用 Core2 屏下沿三键映射（默认 0=不映射）：y≥200 原始区 = BtnA/B/C
  M5.Lcd.setBrightness(128);
  M5.Power.setExtOutput(false);   // 不对外供电，省电

  g_wifi_ev = xEventGroupCreate();
  store_init();
  {
    extern void audio_set_volume(int);
    audio_set_volume(g_store.volume);   // 上次的音量（默认 200）
  }
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

  static ui_callbacks_t uicb = { .on_hold = ui_hold, .on_send = ui_send,
                                 .on_stop = ui_stop, .on_update = ui_update,
                                 .on_volume_set = ui_volume_set,
                                 .on_sessions_open = ui_sessions_open,
                                 .on_sessions_close = ui_sessions_close };
  ui_set_callbacks(&uicb);
  ui_init();
  work_bind_ws();
  schat_ws_on_reconnected(on_ws_reconnected);
  schat_set_pump(ui_pump);

  app_set_stage(APP_BOOT);
  app_set_status("启动");

  // WiFi
  bool wifi_ok = false;
  if (g_store.wifi_ssid[0]) {
    app_set_stage(APP_WIFI_WAIT);
    wifi_ok = wifi_connect(15000);
  }

#ifdef UI_DUMP
  // 调试构建：WiFi 连上后把各页面渲染好，设备自己起个 HTTP 服务（:8098）让电脑来拉。
  // 电脑端：node tools/ui-dump-fetch.mjs → node tools/ui-dump-decode.mjs → node tools/ui-dump-inspect.mjs
  if (wifi_ok) {
    ui_dump_serve();
    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
  }
  ESP_LOGE(TAG, "UI_DUMP 构建需要 NVS 里已有的 WiFi 凭据（这次没连上 WiFi）");
  for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
#endif

  if (!wifi_ok) {
    app_set_stage(APP_PORTAL);
    portal_run(10 * 60 * 1000);   // 10 分钟没人配就重启（重试）
    esp_restart();
  }
  ui_init();
  app_set_status("WiFi 已连");
  ota_init();   // 后台静默检查更新（开机 30s 后首查，每 6h 一次；手动入口在顶部状态栏）
  // 进入主阶段：没凭据去扫码，有凭据直接连
  if (g_store.token[0]) app_set_stage(APP_WS_CONNECT);
  else app_set_stage(APP_QR_PAIR);

  // 主循环
  while (true) {
    M5.update();
    housekeeping_update();

    app_stage_t st;
    app_lock(); st = g_app.stage; app_unlock();

    switch (st) {
      case APP_QR_PAIR: {
        // 扫码登录：建会话 → 画码 → 轮询
        char token[256];
        int interval = 2000;
        if (!schat_create_qr_session(token, sizeof(token), &interval)) {
          app_set_status("建登录会话失败，5 秒后重试");
          vTaskDelay(pdMS_TO_TICKS(5000));
          break;
        }
        char payload[300];
        snprintf(payload, sizeof(payload), "SCSH:1:login:%s", token);
        ESP_LOGI(TAG, "QR payload(%d): %s", (int)strlen(payload), payload);
        ui_draw(payload);
        app_set_status("等扫码");
        qr_status_t r;
        uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
        while ((r = schat_poll_qr(token)) == QR_PENDING || r == QR_SCANNED || r == QR_ERROR) {
          if (r == QR_ERROR) vTaskDelay(pdMS_TO_TICKS(3000));
          else vTaskDelay(pdMS_TO_TICKS(interval));
          uint32_t used = xTaskGetTickCount() * portTICK_PERIOD_MS - start;
          ui_qr_hint(r == QR_SCANNED, 300000 - used);
          if (used > 300000) r = QR_EXPIRED;
          if (r == QR_EXPIRED) break;
        }
        if (r == QR_APPROVED) {
          ui_init();
          app_set_status("登录成功");
          // 不在这里刷：刚领的凭据就是新的；下次连 WS 前（连接口）会统一 refresh
          store_clear_bot();   // 新登录：清掉上次选的 bot，进列表页重选（用户要求：扫码后列出来选）
          app_set_stage(APP_BOT_PICK);
        } else {
          app_set_status("二维码过期，重新生成");
          ui_init();
          vTaskDelay(pdMS_TO_TICKS(800));
        }
        break;
      }
      case APP_BOT_PICK: {
        // 机器人列表页（扫码后多个 bot，或被拒后重选）：重走链路，进选择流程
        ensure_voice_online();
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      }
      case APP_WS_CONNECT: {
        // 建链入口（阻塞到 LIVE 或失败；失败后它自己会把阶段定好）
        if (!ensure_voice_online()) {
          app_set_status("连接失败，%u 秒后重试", g_dial_backoff_ms / 1000);
          for (uint32_t i = 0; i < g_dial_backoff_ms / 100; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            M5.update();
            ui_tick();
          }
          if (g_dial_backoff_ms < 30000) g_dial_backoff_ms *= 2;
        }
        break;
      }
      case APP_LIVE: {
        if (work_dc_open() && schat_ws_online()) {
          g_ws_reconnected = false;
          g_dial_backoff_ms = 2000;
          vTaskDelay(pdMS_TO_TICKS(50));
          break;
        }
        // 断了（WS 或数据通道）：整条链路拆了重来（带退避）
        ESP_LOGW(TAG, "LIVE 链路断了（ws=%d dc=%d），重连",
                 (int)schat_ws_online(), (int)work_dc_open());
        work_stop();
        schat_ws_stop();
        app_set_status("连接断了，%u 秒后重连", g_dial_backoff_ms / 1000);
        for (uint32_t i = 0; i < g_dial_backoff_ms / 100; i++) {
          vTaskDelay(pdMS_TO_TICKS(100));
          M5.update();
          ui_tick();
        }
        if (g_dial_backoff_ms < 30000) g_dial_backoff_ms *= 2;
        app_set_stage(APP_WS_CONNECT);
        break;
      }
      case APP_DIAL:
      case APP_PEER_CONNECT:
      case APP_RPC_HELLO:
      case APP_VOICE_START: {
        // 这些阶段本该在 ensure_voice_online 内部跑完。能停在这里说明建链被打断
        // （对方挂断 / 通道在握手时断了）。看门狗：没通道超 2 秒、或同一阶段停 20 秒就重建
        // ——以前 peer 被动断开后要干等 90 秒，期间怎么按都没反应。
        static uint32_t stage_since;
        static app_stage_t last_seen = APP_BOOT;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (st != last_seen) { last_seen = st; stage_since = now; }
        uint32_t in_stage = now - stage_since;
        bool no_channel = !work_dc_open() || !schat_ws_online();
        if (in_stage > 20000 || (no_channel && in_stage > 2000)) {
          ESP_LOGW(TAG, "阶段 %d 停 %u ms（dc=%d ws=%d），重建链路", (int)st, (unsigned)in_stage,
                   (int)work_dc_open(), (int)schat_ws_online());
          work_stop();
          schat_ws_stop();
          app_set_stage(APP_WS_CONNECT);
        }
        M5.update();
        ui_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      }
      default:
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
    }

    ui_tick();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}
