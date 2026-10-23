#include "ui.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// 布局（320x240 竖屏反而不好用，横屏）
// 顶部 24px：状态行（电池/WiFi/阶段）
// 中部：字幕区（efontCN 14）
// 底部 52px：三个触摸区：[按住说话 200px] [停 60px] [音量 60px]

static ui_callbacks_t g_cb;
static bool g_hold_down;
static uint32_t g_last_draw;

// 字幕滚动：caption 512B，一屏显示尾部 ~4 行
static void draw_caption(void)
{
  const int x0 = 0, y0 = 26, w = 320, h = 158;
  M5.Lcd.fillRect(x0, y0, w, h, TFT_BLACK);
  char local[560];
  app_lock();
  strlcpy(local, g_app.caption, sizeof(local));
  app_unlock();
  if (!local[0]) {
    M5.Lcd.setTextDatum(textdatum_t::middle_center);
    M5.Lcd.setFont(&fonts::efontCN_14);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.drawString("按住下面长条说话", x0 + w / 2, y0 + h / 2);
    return;
  }
  // 只画尾部 5 行：按 \n 分段，倒着数
  char *lines[24];
  int n = 0;
  char *p = local;
  while (n < 24 && p) {
    lines[n++] = p;
    char *nl = strchr(p, '\n');
    if (nl) { *nl = '\0'; p = nl + 1; } else break;
  }
  int show = n > 5 ? 5 : n;
  M5.Lcd.setTextDatum(textdatum_t::top_left);
  M5.Lcd.setFont(&fonts::efontCN_14);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i = 0; i < show; i++)
    M5.Lcd.drawString(lines[n - show + i], x0 + 4, y0 + 6 + i * 26);
}

static void draw_top(void)
{
  char line[128];
  const char *stage_txt;
  app_lock();
  switch (g_app.stage) {
    case APP_BOOT: stage_txt = "启动"; break;
    case APP_PORTAL: stage_txt = "配网"; break;
    case APP_WIFI_WAIT: stage_txt = "WiFi"; break;
    case APP_QR_PAIR: stage_txt = "扫码"; break;
    case APP_WS_CONNECT: stage_txt = "连接"; break;
    case APP_DIAL: stage_txt = "呼叫"; break;
    case APP_PEER_CONNECT: stage_txt = "握手"; break;
    case APP_RPC_HELLO: stage_txt = "握手"; break;
    case APP_VOICE_START: stage_txt = "语音"; break;
    case APP_LIVE: {
      switch (g_app.voice_state) {
        case VOICE_LISTENING: stage_txt = "聆听"; break;
        case VOICE_HEARING: stage_txt = "听到"; break;
        case VOICE_THINKING: stage_txt = "想…"; break;
        case VOICE_SPEAKING: stage_txt = "在念"; break;
        default: stage_txt = "就绪"; break;
      }
      break;
    }
    case APP_FATAL: stage_txt = "错误"; break;
    default: stage_txt = ""; break;
  }
  snprintf(line, sizeof(line), "%s  %d%%  %ddBm  %s", stage_txt,
           g_app.battery_pct, g_app.rssi, g_app.status_text);
  app_unlock();
  M5.Lcd.fillRect(0, 0, 320, 24, TFT_NAVY);
  M5.Lcd.setTextDatum(textdatum_t::middle_left);
  M5.Lcd.setFont(&fonts::Font0);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  // 状态文本可能长：尾部能显示 ~52 个半角字符
  size_t len = strlen(line);
  if (len > 52) M5.Lcd.drawString(line + len - 52, 4, 12);
  else M5.Lcd.drawString(line, 4, 12);
}

static void draw_buttons(void)
{
  const int y = 188, h = 52;
  // 按住说话（长条）
  uint32_t c1 = g_hold_down ? TFT_ORANGE : TFT_DARKGREEN;
  M5.Lcd.fillRect(0, y, 200, h, c1);
  M5.Lcd.setTextDatum(textdatum_t::middle_center);
  M5.Lcd.setFont(&fonts::efontCN_14);
  M5.Lcd.setTextColor(TFT_WHITE, c1);
  M5.Lcd.drawString(g_hold_down ? "在听…" : "按住说话", 100, y + h / 2);
  // 停
  M5.Lcd.fillRect(200, y, 60, h, TFT_MAROON);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_MAROON);
  M5.Lcd.drawString("停", 230, y + h / 2);
  // 音量
  M5.Lcd.fillRect(260, y, 60, h, TFT_DARKCYAN);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  extern int audio_volume(void);
  char v[16];
  snprintf(v, sizeof(v), "音%d", audio_volume() * 8 / 255);
  M5.Lcd.drawString(v, 290, y + h / 2);
}

// ---- bot 选择页（扫码后列出机器人让用户点）----
static int g_pick_n = -1;        // -1 = 不在选择页；否则 = 条目数
static int g_pick_sel = -1;      // 已选下标；-1 = 还没选

void ui_pick_bot(char (*names)[48], int n)
{
  if (n > 6) n = 6;
  g_pick_n = n;
  g_pick_sel = -1;
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextDatum(textdatum_t::top_left);
  M5.Lcd.setFont(&fonts::efontCN_14);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString("选一个机器人连：", 8, 4);
  const int y0 = 24, bh = 34, gap = 4;
  for (int i = 0; i < n; i++) {
    int y = y0 + i * (bh + gap);
    M5.Lcd.fillRoundRect(4, y, 312, bh, 6, i % 2 ? TFT_DARKGREY : TFT_DARKCYAN);
    M5.Lcd.setTextDatum(textdatum_t::middle_left);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawString(names[i], 16, y + bh / 2);
  }
}

int ui_pick_result(void) { return g_pick_n >= 0 ? g_pick_sel : -1; }
void ui_pick_reset(void) { g_pick_n = -1; g_pick_sel = -1; }

static void pick_touch(void)
{
  auto t = M5.Touch.getDetail();
  if (!t.wasClicked() || g_pick_n < 0) return;
  const int y0 = 24, bh = 34, gap = 4;
  if (t.y < y0) return;
  int idx = (t.y - y0) / (bh + gap);
  if (idx >= 0 && idx < g_pick_n && t.y < y0 + idx * (bh + gap) + bh) {
    // 高亮选中项
    M5.Lcd.fillRoundRect(4, y0 + idx * (bh + gap), 312, bh, 6, TFT_ORANGE);
    M5.Lcd.setTextDatum(textdatum_t::middle_left);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_ORANGE);
    // 名字已没了：重画不了，直接交给上层（选完马上跳走，闪一下无所谓）
    g_pick_sel = idx;
  }
}

static void touch_tick(void)
{
  if (g_pick_n >= 0) { pick_touch(); return; }   // 选择页独占触摸
  // Touch 由主循环的 M5.update() 刷新，这里只取状态
  auto t = M5.Touch.getDetail();
  const int y = 188, h = 52;
  bool in_hold = t.state && t.y >= y && t.y < y + h && t.x < 200 && t.x >= 0;
  if (in_hold) {
    if (t.wasPressed() && !g_hold_down) {
      g_hold_down = true;
      if (g_cb.on_hold) g_cb.on_hold(true);
    }
  } else if (g_hold_down && t.wasReleased()) {
    g_hold_down = false;
    if (g_cb.on_hold) g_cb.on_hold(false);
  }
  if (t.wasClicked()) {
    if (t.x >= 200 && t.x < 260 && t.y >= y && t.y < y + h) {
      if (g_cb.on_interrupt) g_cb.on_interrupt();
    } else if (t.x >= 260 && t.x < 320 && t.y >= y && t.y < y + h) {
      if (g_cb.on_volume) g_cb.on_volume(64);
    }
  }
}

void ui_init(void)
{
  M5.Lcd.setRotation(1);   // 320x240 横屏
  M5.Lcd.fillScreen(TFT_BLACK);
  draw_top();
  draw_caption();
  draw_buttons();
}

void ui_draw(const char *qr_payload)
{
  // 全屏白底，二维码最大化（扫得出来优先于好看）
  M5.Lcd.fillScreen(TFT_WHITE);
  M5.Lcd.setTextDatum(textdatum_t::top_center);
  M5.Lcd.setFont(&fonts::Font0);
  M5.Lcd.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Lcd.drawCentreString("Secret-Chat 扫码登录", 160, 2);
  // 屏 320x240：标题 10px，剩 230px 给码；margin=true 自动留 4 模块白边
  if (qr_payload) M5.Lcd.qrcode(qr_payload, (320 - 224) / 2, 12, 224, 6, true);
}

void ui_tick(void)
{
  if (g_pick_n >= 0) { touch_tick(); return; }   // 选择页：只处理触摸，不刷主界面
  static app_stage_t last_stage = APP_BOOT;
  static char last_status[96];
  static char last_caption[128];   // 只比尾部（字幕滚动太频繁不重绘按钮）
  static uint32_t last_caption_draw;
  touch_tick();
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (now - g_last_draw < 200) return;   // 刷新限频 5fps
  g_last_draw = now;
  app_lock();
  bool stage_changed = g_app.stage != last_stage;
  bool status_changed = strcmp(g_app.status_text, last_status) != 0;
  size_t clen = strlen(g_app.caption);
  bool caption_changed = clen < sizeof(last_caption)
    ? strcmp(g_app.caption, last_caption) != 0
    : strcmp(g_app.caption + clen - sizeof(last_caption) + 1, last_caption) != 0;
  app_unlock();
  if (stage_changed || status_changed) draw_top();
  if (caption_changed && now - last_caption_draw > 300) {
    draw_caption();
    last_caption_draw = now;
    app_lock();
    size_t l2 = strlen(g_app.caption);
    if (l2 < sizeof(last_caption)) strlcpy(last_caption, g_app.caption, sizeof(last_caption));
    else strlcpy(last_caption, g_app.caption + l2 - sizeof(last_caption) + 1, sizeof(last_caption));
    app_unlock();
  }
  draw_buttons();   // 5fps 全量重画（屏小，不心疼）
  last_stage = g_app.stage;
  app_lock();
  strlcpy(last_status, g_app.status_text, sizeof(last_status));
  app_unlock();
}

void ui_set_callbacks(const ui_callbacks_t *cb) { g_cb = *cb; }
