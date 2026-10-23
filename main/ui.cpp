// ui.cpp — 屏幕与触摸（v0.3）
//
// 机身三键（屏下沿 y≥200 的触摸映射区，M5.setTouchButtonHeight(40) 把这块交给 BtnA/B/C）：
//   A 左：按住说话、松开结束（草稿留在服务端）
//   B 中：把识别结果发给当前目标（默认助手；选中会话则把文字投到该会话）
//   C 右：弃用，不接任何动作
// 主屏：状态栏（图形图标）/ 状态行 / Friday 动态图标（液体彩虹环 + 24 根随真实电平起伏的辐条）
//       / 字幕气泡（含发送目标，点目标行直接开会话页）/ 机身键提示带（不再画三个长条按钮）
// 侧页：右滑 = 音量页；左滑 = 会话列表页。回主屏：反向滑、点左上「主屏」，或放着不管自动回。
// 点中间图标 = 打断播报 / 丢弃草稿（原来右键的活挪到这儿，一眼看得见）。
// 节能：没动静时图标降到 5fps、一分钟不碰降亮度；喇叭功放只在收到语音时才开（见 audio_io）。

#include "ui.h"
#include "audio_io.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#ifdef UI_DUMP
#include "driver/uart.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#define DUMP_WAIT(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#endif

static const char *UI_TAG = "ui";

// 绘制目标：正常固件画到真屏；编译时加 -DUI_DUMP 就画到 PSRAM 画布，
// 把每个页面渲染成 RLE 从串口吐出去，在电脑上还原成 PNG 看版式（正常固件不受影响）。
#ifdef UI_DUMP
static m5gfx::LGFX_Sprite *g_canvas;
static int g_dump_mic, g_dump_play;
#define LCD (*g_canvas)
#define UI_BRIGHT(x) ((void)0)
#else
#define LCD (M5.Lcd)
#define UI_BRIGHT(x) (M5.Lcd.setBrightness(x))
#endif

static int ui_mic_level(void)
{
#ifdef UI_DUMP
  return g_dump_mic;
#else
  return audio_mic_level();
#endif
}
static int ui_play_level(void)
{
#ifdef UI_DUMP
  return g_dump_play;
#else
  return audio_play_level();
#endif
}

#define RGB565(r, g, b) ((uint16_t)(((((r) >> 3) & 0x1F) << 11) | ((((g) >> 2) & 0x3F) << 5) | (((b) >> 3) & 0x1F)))

#define COL_BG      RGB565(8, 10, 18)      // 主底
#define COL_PANEL   RGB565(15, 19, 29)     // 气泡底
#define COL_BAR     RGB565(14, 17, 26)     // 键带底
#define COL_TOP     RGB565(18, 23, 37)     // 状态栏底
#define COL_CHIP    RGB565(24, 30, 46)     // 小按钮底
#define COL_BORDER  RGB565(48, 56, 78)
#define COL_DIM     RGB565(112, 124, 146)
#define COL_TEXT    RGB565(236, 241, 250)
#define COL_ACCENT  RGB565(64, 205, 255)
#define COL_ORANGE  RGB565(255, 152, 44)
#define COL_GREEN   RGB565(62, 226, 132)
#define COL_RED     RGB565(255, 84, 84)
#define COL_YELLOW  RGB565(255, 208, 64)

// ---- 主屏布局 ----
#define TOP_H       20                     // 状态栏 0..19
#define STATUS_Y    22                     // 提示行 22..37（中文，用 efontCN_14）
#define STATUS_H    16
#define ORB_TOP     40                     // 图标区 40..136
#define ORB_CX      160
#define ORB_CY      88
#define ORB_R0      30.0f
#define ORB_HOLE    11.0f
#define ORB_SEGS    40
#define ORB_SPOKES  24
#define BUB_Y       140                    // 字幕气泡 140..196
#define BUB_H       56
#define BUB_X       6
#define BUB_W       308
#define BUB_HEAD_H  17
#define KEY_Y       200                    // 机身键带 200..239（这块触摸 = 物理键）

// ---- 侧页 ----
#define BACK_X0 4
#define BACK_Y0 3
#define BACK_W 58
#define BACK_H 17
#define VOL_SL_X 24
#define VOL_SL_W 272
#define VOL_SL_Y 76
#define VOL_SL_H 24
#define VOL_BTN_CY 140
#define SESS_Y0 26
#define SESS_ROW_H 25
#define SESS_ROW_GAP 3
#define SESS_BOTTOM 196

static ui_callbacks_t g_cb;
static uint32_t g_last_draw;
static bool g_hold_down;
static uint32_t g_b_flash;          // 键 B 点亮的截止时刻
static uint32_t g_last_touch;       // 最近一次触摸/按键（自动返回、降亮度）
static uint8_t g_backlight = 128;
static bool g_qr_page;              // 二维码页占屏
static bool g_portal_page;          // 配网页占屏

typedef enum { PG_MAIN = 0, PG_VOLUME, PG_SESSIONS } page_t;
static page_t g_page = PG_MAIN;

// 发送目标
static struct {
  bool assistant = true;
  char id[40] = "";
  char label[64] = "助手";
} g_target;

void ui_get_target(bool *is_assistant, char *conv_id, size_t idcap)
{
  if (is_assistant) *is_assistant = g_target.assistant;
  if (conv_id) strlcpy(conv_id, g_target.id, idcap);
}
const char *ui_target_label(void) { return g_target.label; }

// 前置声明（页面之间互相跳）
static void page_main_full(void);
static void draw_volume(void);
static void draw_sessions(void);
static void open_page(page_t p);
static void go_main(void);
static void sessions_select(int idx);

// ---------------- 小工具 ----------------

static uint32_t ui_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

static int u8len(const char *p)
{
  unsigned char c = (unsigned char)p[0];
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

// 按像素截断（末尾补 …）。调用前要先设好字体。
static void clip_px(char *s, int maxw)
{
  if ((int)LCD.textWidth(s) <= maxw) return;
  int n = (int)strlen(s);
  while (n > 0) {
    do { n--; } while (n > 0 && (((unsigned char)s[n]) >> 6) == 0x80);
    if (n <= 0) break;
    char save = s[n];
    s[n] = '\0';
    int w = (int)LCD.textWidth(s) + (int)LCD.textWidth("…");
    s[n] = save;
    if (w <= maxw) { s[n] = '\0'; strcat(s, "…"); return; }
  }
  strcpy(s, "…");
}

typedef struct {
  app_stage_t stage;
  voice_state_t vs;
  bool held, chg;
  int batt, rssi;
} st_t;

static st_t snap(void)
{
  st_t s;
  app_lock();
  s.stage = g_app.stage; s.vs = g_app.voice_state; s.held = g_app.held;
  s.batt = g_app.battery_pct; s.rssi = g_app.rssi; s.chg = g_app.charging;
  app_unlock();
  return s;
}

static const char *stage_text(st_t s)
{
  switch (s.stage) {
    case APP_BOOT: return "启动";
    case APP_PORTAL: return "配网";
    case APP_WIFI_WAIT: return "连 WiFi";
    case APP_QR_PAIR: return "扫码";
    case APP_BOT_PICK: return "选机器人";
    case APP_WS_CONNECT: return "连接";
    case APP_DIAL: return "呼叫";
    case APP_PEER_CONNECT:
    case APP_RPC_HELLO: return "握手";
    case APP_VOICE_START: return "语音";
    case APP_LIVE:
      switch (s.vs) {
        case VOICE_LISTENING: return "聆听";
        case VOICE_HEARING: return "听到";
        case VOICE_THINKING: return "在想";
        case VOICE_SPEAKING: return "在念";
        default: return "就绪";
      }
    case APP_FATAL: return "错误";
    default: return "";
  }
}

// 状态色（全局统一：状态点、气泡、图标芯）
static uint16_t state_color(st_t s)
{
  if (s.stage != APP_LIVE) return COL_YELLOW;
  if (s.held) return COL_ORANGE;
  switch (s.vs) {
    case VOICE_HEARING: return COL_ORANGE;
    case VOICE_THINKING: return COL_ACCENT;
    case VOICE_SPEAKING: return COL_GREEN;
    default: return COL_GREEN;
  }
}

// ---------------- 状态栏 ----------------

static void draw_top(void)
{
  st_t s = snap();
  LCD.fillRect(0, 0, 320, TOP_H, COL_TOP);
  LCD.drawFastHLine(0, TOP_H - 1, 320, RGB565(30, 38, 56));
  LCD.fillCircle(10, TOP_H / 2, 4, state_color(s));
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextColor(COL_TEXT, COL_TOP);
  LCD.drawString(stage_text(s), 19, TOP_H / 2 + 1);
  // 机器人名（中间，长了截掉）
  char nm[24];
  app_lock(); strlcpy(nm, g_app.bot_name, sizeof(nm)); app_unlock();
  if (nm[0]) {
    LCD.setTextDatum(textdatum_t::middle_center);
    LCD.setTextColor(RGB565(140, 152, 176), COL_TOP);
    clip_px(nm, 96);
    LCD.drawString(nm, 160, TOP_H / 2 + 1);
  }
  // 右：信号格 + 电量数字 + 电池图形（三者不重叠：244..262 / 270..282 / 286..310）
  int by = 5, bw = 22, bh = 11, bx = 286;
  char pct[12];
  snprintf(pct, sizeof(pct), "%d", s.batt);
  LCD.setFont(&fonts::Font2);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.setTextColor(COL_DIM, COL_TOP);
  clip_px(pct, 24);
  LCD.drawString(pct, bx - 6, TOP_H / 2 + 1);
  LCD.drawRoundRect(bx, by, bw, bh, 3, COL_DIM);
  LCD.fillRect(bx + bw, by + 3, 2, 5, COL_DIM);
  int batt = s.batt < 0 ? 0 : (s.batt > 100 ? 100 : s.batt);
  int fw = (bw - 4) * batt / 100;
  uint16_t bc = s.chg ? COL_GREEN : (batt <= 15 ? COL_RED : COL_ACCENT);
  if (fw > 0) LCD.fillRect(bx + 2, by + 2, fw, bh - 4, bc);
  if (s.chg) {
    LCD.drawLine(bx + 13, by + 1, bx + 9, by + 6, COL_TEXT);
    LCD.drawLine(bx + 9, by + 6, bx + 14, by + 6, COL_TEXT);
    LCD.drawLine(bx + 14, by + 6, bx + 10, by + 10, COL_TEXT);
  }
  int bars = s.rssi == 0 ? 0 : (s.rssi >= -55 ? 4 : s.rssi >= -67 ? 3 : s.rssi >= -77 ? 2 : 1);
  for (int i = 0; i < 4; i++) {
    int hgt = 3 + i * 2;
    uint16_t c = (i < bars) ? (bars >= 3 ? COL_GREEN : bars == 2 ? COL_YELLOW : COL_RED) : RGB565(46, 54, 74);
    LCD.fillRect(244 + i * 5, by + bh - hgt, 3, hgt, c);   // 244..262
  }
}

static void draw_statusline(void)
{
  char local[96];
  app_lock(); strlcpy(local, g_app.status_text, sizeof(local)); app_unlock();
  LCD.fillRect(0, STATUS_Y, 320, STATUS_H, COL_BG);
  if (!local[0]) return;
  // 中文要用 efontCN_14（Font0 里没中文字模，以前这行一直是空的）；长了按像素截，不能按字节截
  clip_px(local, 306);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextColor(RGB565(150, 162, 186), COL_BG);
  LCD.drawString(local, 160, STATUS_Y + STATUS_H / 2);
}

// ---------------- Friday 动态图标 ----------------

static const uint8_t PAL[][3] = {
  {255, 60, 150}, {200, 70, 255}, {90, 120, 255}, {40, 210, 255},
  {70, 255, 190}, {255, 110, 200}, {255, 60, 150},
};
#define PAL_N 6

static uint16_t orb_color(float t, int dim)
{
  if (t < 0) t += 1.0f;
  if (t >= 1.0f) t -= 1.0f;
  float f = t * PAL_N;
  int i = (int)f; if (i >= PAL_N) i = PAL_N - 1;
  int j = (i + 1) % PAL_N;
  float k = f - i;
  int r = PAL[i][0] + (int)((PAL[j][0] - PAL[i][0]) * k);
  int g = PAL[i][1] + (int)((PAL[j][1] - PAL[i][1]) * k);
  int b = PAL[i][2] + (int)((PAL[j][2] - PAL[i][2]) * k);
  return RGB565(r * dim / 100, g * dim / 100, b * dim / 100);
}

static uint8_t g_lv[ORB_SPOKES];   // 电平历史（每帧滚一格：20fps 时约 1.2 秒窗口）

static void draw_orb(uint32_t now, st_t s)
{
  float color_period = 8000;
  float breath = 1.0f + 0.035f * sinf(now * 2.0f * (float)M_PI / 3400.0f);
  float w1 = 3.0f, s1 = 0.9f, w2 = 2.0f;
  int dim = 100;
  bool talking = s.held || s.vs == VOICE_HEARING;
  if (s.stage != APP_LIVE) { dim = 52; color_period = 16000; }
  else if (talking) { w1 = 4.5f; w2 = 3.0f; s1 = 1.7f; color_period = 3200; }
  else if (s.vs == VOICE_THINKING) { color_period = 1100; s1 = 2.2f; }
  else if (s.vs == VOICE_SPEAKING) { color_period = 2600; breath = 1.0f + 0.05f * sinf(now * 2.0f * (float)M_PI / 700.0f); }

  int lv = 0;
  if (talking) lv = ui_mic_level();
  else if (s.vs == VOICE_SPEAKING) lv = ui_play_level();
  if (s.vs == VOICE_SPEAKING) breath += lv / 900.0f;
  for (int i = 0; i < ORB_SPOKES - 1; i++) g_lv[i] = g_lv[i + 1];
  g_lv[ORB_SPOKES - 1] = (uint8_t)(lv > 255 ? 255 : lv);

  float cphase = fmodf((float)now / color_period, 1.0f);
  float p1 = now * 0.001f * s1, p2 = now * 0.0013f * s1 * 0.8f;
  const float TAU = 2.0f * (float)M_PI;

  LCD.fillRect(ORB_CX - 52, ORB_TOP, 104, BUB_Y - 2 - ORB_TOP, COL_BG);

  if (talking) {   // 听到声音：从环上荡开的涟漪
    for (int k = 0; k < 2; k++) {
      float ph = fmodf(now / 1300.0f + k * 0.5f, 1.0f);
      LCD.drawCircle(ORB_CX, ORB_CY, (int)(ORB_R0 + 6 + ph * 14.0f),
                        orb_color(fmodf(cphase + 0.3f + k * 0.5f, 1.0f), 55 - (int)(45 * ph)));
    }
  }

  float xa[ORB_SEGS + 1], ya[ORB_SEGS + 1], xi[ORB_SEGS + 1], yi[ORB_SEGS + 1], ro[ORB_SEGS + 1];
  for (int i = 0; i <= ORB_SEGS; i++) {
    float a = TAU * i / ORB_SEGS;
    ro[i] = (ORB_R0 + w1 * sinf(3.0f * a + p1) + w2 * sinf(5.0f * a - p2 + 1.7f)) * breath;
    float ri = ORB_HOLE + 1.6f * sinf(2.0f * a + p2);
    xa[i] = ORB_CX + ro[i] * cosf(a); ya[i] = ORB_CY + ro[i] * sinf(a);
    xi[i] = ORB_CX + ri * cosf(a); yi[i] = ORB_CY + ri * sinf(a);
  }
  // 中心芯：随电平胀缩
  LCD.fillCircle(ORB_CX, ORB_CY, (int)(ORB_HOLE - 5 + lv / 45.0f), RGB565(20, 25, 40));

  float comet = fmodf((float)now / 700.0f, 1.0f);
  for (int i = 0; i < ORB_SEGS; i++) {
    float t = (float)i / ORB_SEGS;
    int d = dim;
    if (s.vs == VOICE_THINKING) {
      float dd = fmodf(t - comet + 1.5f, 1.0f);
      if (dd < 0.12f) d = 100;
    }
    uint16_t c = orb_color(fmodf(t + cphase, 1.0f), d);
    LCD.fillTriangle(xi[i], yi[i], xi[i + 1], yi[i + 1], xa[i], ya[i], c);
    LCD.fillTriangle(xi[i + 1], yi[i + 1], xa[i], ya[i], xa[i + 1], ya[i + 1], c);
    uint16_t h = orb_color(fmodf(t + cphase, 1.0f), d * 28 / 100);   // 外侧细光晕
    float hx0 = xa[i] + (xa[i] - ORB_CX) * 0.07f, hy0 = ya[i] + (ya[i] - ORB_CY) * 0.07f;
    float hx1 = xa[i + 1] + (xa[i + 1] - ORB_CX) * 0.07f, hy1 = ya[i + 1] + (ya[i + 1] - ORB_CY) * 0.07f;
    LCD.fillTriangle(xa[i], ya[i], xa[i + 1], ya[i + 1], hx0, hy0, h);
    LCD.fillTriangle(xa[i + 1], ya[i + 1], hx0, hy0, hx1, hy1, h);
  }
  // 辐条：说话/播报时就是声纹
  for (int i = 0; i < ORB_SPOKES; i++) {
    float a = TAU * (i + 0.5f) / ORB_SPOKES - (float)M_PI / 2.0f;
    float base = ro[ORB_SEGS * i / ORB_SPOKES] + 3.0f;
    int len = 1 + (g_lv[i] * 9) / 255;
    float cx = ORB_CX + base * cosf(a), cy = ORB_CY + base * sinf(a);
    LCD.drawLine((int)cx, (int)cy, (int)(cx + len * cosf(a)), (int)(cy + len * sinf(a)),
                    orb_color(fmodf((float)i / ORB_SPOKES + cphase, 1.0f), dim));
  }
}

// ---------------- 字幕气泡 ----------------

static void draw_bubble(void)
{
  static char buf[512];
  st_t s = snap();
  app_lock(); strlcpy(buf, g_app.caption, sizeof(buf)); app_unlock();
  uint16_t accent = state_color(s);

  LCD.fillRoundRect(BUB_X, BUB_Y, BUB_W, BUB_H, 8, COL_PANEL);
  LCD.drawRoundRect(BUB_X, BUB_Y, BUB_W, BUB_H, 8, COL_BORDER);
  // 目标行（点它 = 开会话列表）
  LCD.fillCircle(BUB_X + 12, BUB_Y + BUB_HEAD_H / 2 + 1, 3, accent);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextColor(COL_ACCENT, COL_PANEL);
  char tgt[48];
  snprintf(tgt, sizeof(tgt), "发给 %s", ui_target_label());
  clip_px(tgt, 140);
  LCD.drawString(tgt, BUB_X + 20, BUB_Y + BUB_HEAD_H / 2 + 1);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.setTextColor(COL_DIM, COL_PANEL);
  const char *hint = "";
  if (s.stage == APP_LIVE) {
    if (s.vs == VOICE_SPEAKING) hint = "点图标打断";
    else if (buf[0]) hint = "B发送 点图标清";
    else hint = "A按住说话";
  }
  LCD.drawString(hint, BUB_X + BUB_W - 10, BUB_Y + BUB_HEAD_H / 2 + 1);
  LCD.drawFastHLine(BUB_X + 8, BUB_Y + BUB_HEAD_H, BUB_W - 16, RGB565(32, 40, 58));

  int ty = BUB_Y + BUB_HEAD_H;
  LCD.setFont(&fonts::efontCN_14);
  if (!buf[0]) {   // 空：一句话告诉他现在能干什么
    LCD.setTextColor(RGB565(88, 98, 120), COL_PANEL);
    LCD.setTextDatum(textdatum_t::middle_center);
    const char *tip = s.stage == APP_LIVE ? "按住 A 键说话，松开结束"
                      : (s.stage == APP_QR_PAIR ? "扫码登录后就能说话" : "正在连 Friday…");
    LCD.drawString(tip, BUB_X + BUB_W / 2, ty + (BUB_H - BUB_HEAD_H) / 2);
    return;
  }
  // 只显示尾部两行：先按字节取尾巴（对齐 UTF-8 边界），再按像素折行
  size_t len = strlen(buf);
  int off = 0;
  if (len > 150) {
    off = (int)len - 150;
    while (off < (int)len && (((unsigned char)buf[off]) >> 6) == 0x80) off++;
  }
  char *txt = buf + off;
  int starts[20], n = 0, w = 0, cur = 0;
  char tmp[5];
  starts[n++] = 0;
  for (int i = 0; txt[i]; ) {
    if (txt[i] == '\n') { txt[i] = '\0'; if (n < 20) starts[n++] = i + 1; cur = i + 1; w = 0; i++; continue; }
    int cl = u8len(txt + i);
    memcpy(tmp, txt + i, cl); tmp[cl] = 0;
    int cw = (int)LCD.textWidth(tmp);
    if (w + cw > BUB_W - 26 && i > cur) {
      txt[i - 1] = '\0';
      if (n < 20) starts[n++] = i;
      cur = i; w = cw;
    } else w += cw;
    i += cl;
    if (n >= 20) break;
  }
  int show = n > 2 ? 2 : n;
  LCD.setTextDatum(textdatum_t::top_left);
  LCD.setTextColor(s.held ? COL_TEXT : COL_ACCENT, COL_PANEL);
  int lastx = BUB_X + 12;
  for (int i = 0; i < show; i++) {
    int li = n - show + i;
    int y = ty + 2 + i * 17;
    int x = BUB_X + 12;
    if (off && li == 0) { LCD.drawString("…", x, y); x += (int)LCD.textWidth("…"); }
    const char *line = txt + starts[li];
    LCD.drawString(line, x, y);
    lastx = x + (int)LCD.textWidth(line);
  }
  if (s.held && (ui_ms() % 800) < 400)   // 按着说话：光标一闪一闪
    LCD.fillRect(lastx + 2, ty + 2 + (show - 1) * 17 + 3, 2, 11, COL_ORANGE);
}

// ---------------- 机身键提示带 ----------------

static void draw_key_glyph(int cx, const char *letter, const char *label, bool active, uint16_t col)
{
  const int cy = 214;
  LCD.setFont(&fonts::Font0);
  LCD.setTextDatum(textdatum_t::middle_center);
  if (active) {
    LCD.fillCircle(cx, cy, 9, col);
    LCD.setTextColor(TFT_BLACK, col);
  } else {
    LCD.drawCircle(cx, cy, 9, COL_BORDER);
    LCD.setTextColor(COL_DIM, COL_BAR);
  }
  LCD.drawString(letter, cx, cy);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(active ? col : RGB565(152, 162, 182), COL_BAR);
  LCD.drawString(label, cx + 14, cy);
}

static void draw_keyband(page_t pg)
{
  LCD.fillRect(0, KEY_Y, 320, 240 - KEY_Y, COL_BAR);
  LCD.drawFastHLine(0, KEY_Y, 320, RGB565(34, 42, 62));
  bool bflash = g_b_flash && (int32_t)(ui_ms() - g_b_flash) < 0;
  draw_key_glyph(24, "A", g_hold_down ? "松开结束" : "按住说话", g_hold_down, COL_ORANGE);
  draw_key_glyph(150, "B", "发送", bflash, COL_ACCENT);
  // 第三个键按用户要求弃用：不画按钮、不写字，只留一个暗点表示这里没事
  LCD.fillCircle(288, 214, 2, RGB565(34, 42, 62));
  LCD.setFont(&fonts::Font0);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(RGB565(92, 102, 124), COL_BAR);
  const char *hint = pg == PG_MAIN ? "右滑 音量 · 左滑 会话"
                   : (pg == PG_VOLUME ? "左滑 或 点左上 回主屏" : "点一行选目标 · 右滑回主屏");
  LCD.drawString(hint, 160, 232);
}

static void page_main_full(void)
{
  uint32_t now = ui_ms();
  st_t s = snap();
  LCD.fillScreen(COL_BG);
  draw_top();
  draw_statusline();
  draw_orb(now, s);
  draw_bubble();
  draw_keyband(PG_MAIN);
}

// ---------------- 音量页 ----------------

static void draw_back_chip(const char *title)
{
  LCD.fillRoundRect(BACK_X0, BACK_Y0, BACK_W, BACK_H, 5, COL_CHIP);
  LCD.drawRoundRect(BACK_X0, BACK_Y0, BACK_W, BACK_H, 5, COL_BORDER);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(COL_TEXT, COL_CHIP);
  LCD.drawString(title, BACK_X0 + BACK_W / 2 + 5, BACK_Y0 + BACK_H / 2);
  int cy = BACK_Y0 + BACK_H / 2;
  LCD.fillTriangle(BACK_X0 + 12, cy - 4, BACK_X0 + 12, cy + 4, BACK_X0 + 7, cy, COL_DIM);
}

static void draw_volume(void)
{
  int v = audio_volume();
  LCD.fillScreen(COL_BG);
  draw_back_chip("主屏");
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(COL_TEXT, COL_BG);
  LCD.drawString("音量", 74, 12);

  // 喇叭图标 + 声波（按音量画 0~3 道）
  int sx = 44, sy = 46;
  LCD.fillRect(sx, sy - 4, 6, 8, COL_TEXT);
  LCD.fillTriangle(sx + 6, sy - 4, sx + 6, sy + 4, sx + 14, sy + 9, COL_TEXT);
  LCD.fillTriangle(sx + 6, sy - 4, sx + 6, sy + 4, sx + 14, sy - 9, COL_TEXT);
  int waves = v == 0 ? 0 : 1 + v * 3 / 256;
  for (int i = 0; i < waves; i++) {
    int r = 5 + i * 5;
    LCD.drawCircle(sx + 16, sy, r, i + 1 == waves ? COL_ACCENT : RGB565(70, 90, 120));
  }
  // 大数字（居中；以前靠右，和左边的喇叭图标之间一大块空白）
  char num[12];
  snprintf(num, sizeof(num), "%d%%", v * 100 / 255);
  LCD.setFont(&fonts::Font4);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(v == 0 ? COL_DIM : COL_TEXT, COL_BG);
  LCD.drawString(num, 160, 46);
  if (v == 0) {
    LCD.setFont(&fonts::efontCN_14);
    LCD.setTextDatum(textdatum_t::middle_right);
    LCD.setTextColor(COL_RED, COL_BG);
    LCD.drawString("静音", 300, 46);
  }

  // 滑条
  int fw = VOL_SL_W * v / 255;
  LCD.fillRoundRect(VOL_SL_X, VOL_SL_Y, VOL_SL_W, VOL_SL_H, 12, RGB565(28, 34, 50));
  if (fw >= 14) LCD.fillRoundRect(VOL_SL_X, VOL_SL_Y, fw, VOL_SL_H, 12, COL_ACCENT);
  else if (fw > 0) LCD.fillRoundRect(VOL_SL_X, VOL_SL_Y + VOL_SL_H / 2 - 3, fw, 6, 3, COL_ACCENT);
  LCD.drawRoundRect(VOL_SL_X, VOL_SL_Y, VOL_SL_W, VOL_SL_H, 12, COL_BORDER);
  for (int i = 1; i < 4; i++)
    LCD.drawFastVLine(VOL_SL_X + VOL_SL_W * i / 4, VOL_SL_Y + VOL_SL_H + 3, 3, RGB565(60, 70, 92));
  int kx = VOL_SL_X + fw;
  if (kx < VOL_SL_X + 10) kx = VOL_SL_X + 10;
  if (kx > VOL_SL_X + VOL_SL_W - 10) kx = VOL_SL_X + VOL_SL_W - 10;
  LCD.fillCircle(kx, VOL_SL_Y + VOL_SL_H / 2, 13, COL_TEXT);
  LCD.fillCircle(kx, VOL_SL_Y + VOL_SL_H / 2, 10, v == 0 ? COL_DIM : COL_ACCENT);

  // － / ＋ 圆按钮 + 静音 / 最大 快捷
  int cy = VOL_BTN_CY;
  LCD.drawCircle(46, cy, 24, COL_BORDER);
  LCD.drawCircle(274, cy, 24, COL_BORDER);
  LCD.setFont(&fonts::Font4);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(COL_TEXT, COL_BG);
  LCD.drawString("-", 46, cy);
  LCD.drawString("+", 274, cy);
  LCD.setFont(&fonts::efontCN_14);
  LCD.fillRoundRect(92, cy - 14, 56, 28, 6, v == 0 ? COL_RED : COL_CHIP);
  LCD.drawRoundRect(92, cy - 14, 56, 28, 6, v == 0 ? COL_RED : COL_BORDER);
  LCD.setTextColor(v == 0 ? TFT_BLACK : COL_TEXT, v == 0 ? COL_RED : COL_CHIP);
  LCD.drawString("静音", 120, cy);
  LCD.fillRoundRect(172, cy - 14, 56, 28, 6, v == 255 ? COL_GREEN : COL_CHIP);
  LCD.setTextColor(v == 255 ? TFT_BLACK : COL_TEXT, v == 255 ? COL_GREEN : COL_CHIP);
  LCD.drawString("最大", 200, cy);

  // 实时播放电平（收到语音时它会跳，顺便证明喇叭真的开了）
  LCD.fillRect(58, 176, VOL_SL_X + VOL_SL_W - 58 + 24, 8, RGB565(28, 34, 50));
  int lv = ui_play_level();
  if (lv > 0) LCD.fillRect(58, 176, (VOL_SL_X + VOL_SL_W - 34) * lv / 255, 8, COL_GREEN);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(COL_DIM, COL_BG);
  LCD.drawString("播放", VOL_SL_X, 180);

  draw_keyband(PG_VOLUME);
}

static void draw_volume_level_only(void)   // 100ms 一次，只刷电平条那一小条
{
  int lv = ui_play_level();
  LCD.fillRect(VOL_SL_X + 26, 176, VOL_SL_W - 26, 8, RGB565(28, 34, 50));
  if (lv > 0) LCD.fillRect(VOL_SL_X + 26, 176, (VOL_SL_W - 26) * lv / 255, 8, COL_GREEN);
}

// ---------------- 会话列表页 ----------------

static int g_sess_scroll;
static char g_sel_id[40] = "";

static uint16_t sess_status_color(const char *s)
{
  if (!strcmp(s, "running")) return COL_GREEN;
  if (!strcmp(s, "approval")) return COL_ORANGE;
  if (!strcmp(s, "question")) return COL_ACCENT;
  if (!strcmp(s, "quota")) return COL_YELLOW;
  if (!strcmp(s, "error")) return COL_RED;
  return COL_DIM;
}
static const char *sess_status_text(const char *s)
{
  if (!strcmp(s, "running")) return "运行中";
  if (!strcmp(s, "approval")) return "等确认";
  if (!strcmp(s, "question")) return "在提问";
  if (!strcmp(s, "quota")) return "额度";
  if (!strcmp(s, "error")) return "出错";
  return "";
}

static void draw_sessions(void)
{
  static work_session_t ss[24];
  int n = work_sessions_copy(ss, 24);
  int total = n + 1;
  int view_h = SESS_BOTTOM - SESS_Y0;
  int content_h = total * (SESS_ROW_H + SESS_ROW_GAP);
  if (content_h <= view_h) g_sess_scroll = 0;
  else {
    if (g_sess_scroll < 0) g_sess_scroll = 0;
    if (g_sess_scroll > content_h - view_h) g_sess_scroll = content_h - view_h;
  }

  LCD.fillScreen(COL_BG);
  draw_back_chip("主屏");
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(COL_TEXT, COL_BG);
  LCD.drawString("发送到", 74, 12);
  char sub[16];
  snprintf(sub, sizeof(sub), "%d", n);
  LCD.setFont(&fonts::Font0);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.setTextColor(COL_DIM, COL_BG);
  LCD.drawString(sub, 308, 12);

  if (n == 0) {
    LCD.setFont(&fonts::efontCN_14);
    LCD.setTextDatum(textdatum_t::middle_center);
    LCD.setTextColor(COL_DIM, COL_BG);
    LCD.drawString("还没有别的会话", 160, 84);
    LCD.drawString("在 Friday Work 里发起后会出现在这", 160, 104);
  }

  LCD.setClipRect(0, SESS_Y0, 312, SESS_BOTTOM - SESS_Y0);
  for (int i = 0; i < total; i++) {
    int y = SESS_Y0 + i * (SESS_ROW_H + SESS_ROW_GAP) - g_sess_scroll;
    if (y > SESS_BOTTOM || y + SESS_ROW_H < SESS_Y0) continue;
    const char *id = (i == 0) ? "" : ss[i - 1].id;
    bool sel = (i == 0) ? !g_sel_id[0] : !strcmp(g_sel_id, id);
    uint16_t bg = sel ? RGB565(22, 46, 66) : RGB565(16, 20, 32);
    uint16_t bc = sel ? COL_ACCENT : RGB565(40, 48, 68);
    LCD.fillRoundRect(6, y, 302, SESS_ROW_H, 6, bg);
    LCD.drawRoundRect(6, y, 302, SESS_ROW_H, 6, bc);
    if (sel) LCD.fillRect(7, y + 5, 2, SESS_ROW_H - 10, COL_ACCENT);
    if (i == 0) {
      bool ok = work_assistant_enabled();
      LCD.fillCircle(20, y + SESS_ROW_H / 2, 5, ok ? COL_GREEN : COL_DIM);
      LCD.setFont(&fonts::efontCN_14);
      LCD.setTextDatum(textdatum_t::middle_left);
      LCD.setTextColor(COL_TEXT, bg);
      LCD.drawString("助手", 30, y + SESS_ROW_H / 2);
      LCD.setFont(&fonts::Font0);
      LCD.setTextDatum(textdatum_t::middle_right);
      LCD.setTextColor(sel ? COL_ACCENT : RGB565(96, 106, 128), bg);
      LCD.drawString(ok ? "VOICE" : "OFF", 300, y + SESS_ROW_H / 2);
    } else {
      LCD.fillCircle(20, y + SESS_ROW_H / 2, 5, sess_status_color(ss[i - 1].status));
      char title[48];
      strlcpy(title, ss[i - 1].title, sizeof(title));
      const char *xt = sess_status_text(ss[i - 1].status);
      LCD.setFont(&fonts::efontCN_14);
      LCD.setTextDatum(textdatum_t::middle_left);
      LCD.setTextColor(COL_TEXT, bg);
      clip_px(title, xt[0] ? 170 : 240);
      LCD.drawString(title, 30, y + SESS_ROW_H / 2);
      if (xt[0]) {
        LCD.setTextColor(sess_status_color(ss[i - 1].status), bg);
        LCD.setTextDatum(textdatum_t::middle_right);
        LCD.drawString(xt, 300, y + SESS_ROW_H / 2);
      }
    }
  }
  LCD.clearClipRect();

  if (content_h > view_h) {
    int track_h = SESS_BOTTOM - SESS_Y0 - 4;
    int thumb = track_h * view_h / content_h;
    if (thumb < 12) thumb = 12;
    int ty = SESS_Y0 + 2 + (track_h - thumb) * g_sess_scroll / (content_h - view_h);
    LCD.fillRoundRect(314, ty, 4, thumb, 2, RGB565(70, 82, 108));
  }
  draw_keyband(PG_SESSIONS);
}

// ---------------- 页面切换 ----------------

static void open_page(page_t p)
{
  if (g_page == PG_SESSIONS && p != PG_SESSIONS && g_cb.on_sessions_close) g_cb.on_sessions_close();
  g_page = p;
  g_last_touch = ui_ms();
  ESP_LOGI(UI_TAG, "page=%s", p == PG_MAIN ? "main" : (p == PG_VOLUME ? "volume" : "sessions"));
  if (p == PG_MAIN) page_main_full();
  else if (p == PG_VOLUME) draw_volume();
  else {
    g_sess_scroll = 0;
    if (g_cb.on_sessions_open) g_cb.on_sessions_open();
    draw_sessions();
  }
}

static void go_main(void)
{
  if (g_page == PG_SESSIONS && g_cb.on_sessions_close) g_cb.on_sessions_close();
  g_page = PG_MAIN;
  g_last_touch = ui_ms();
  page_main_full();
}

static void sessions_select(int idx)
{
  if (idx == 0) {
    g_target.assistant = true;
    strlcpy(g_target.id, "", sizeof(g_target.id));
    strlcpy(g_target.label, "助手", sizeof(g_target.label));
    g_sel_id[0] = '\0';
  } else {
    static work_session_t ss[24];
    int n = work_sessions_copy(ss, 24);
    if (idx - 1 >= n) return;
    g_target.assistant = false;
    strlcpy(g_target.id, ss[idx - 1].id, sizeof(g_target.id));
    strlcpy(g_target.label, ss[idx - 1].title, sizeof(g_target.label));
    strlcpy(g_sel_id, ss[idx - 1].id, sizeof(g_sel_id));
  }
  ESP_LOGI(UI_TAG, "target=%s assistant=%d", g_target.label, (int)g_target.assistant);
  go_main();
}

// ---------------- 二维码页 ----------------

void ui_draw(const char *qr_payload)
{
  g_qr_page = true;
  g_portal_page = false;
  LCD.fillScreen(TFT_WHITE);
  LCD.fillRect(0, 0, 320, 26, RGB565(24, 28, 40));
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(TFT_WHITE, RGB565(24, 28, 40));
  LCD.drawString("扫码登录", 8, 13);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.drawString(DEVICE_NAME, 312, 13);
  if (qr_payload) {
    int qs = 176;
    LCD.drawRect((320 - qs) / 2 - 5, 33, qs + 10, qs + 10, RGB565(210, 210, 210));
    LCD.qrcode(qr_payload, (320 - qs) / 2, 38, qs, 4, true);
  }
  ui_qr_hint(false, 300000);
}

void ui_qr_hint(bool scanned, int remain_ms)
{
  if (!g_qr_page) return;
  LCD.fillRect(0, 210, 320, 30, TFT_WHITE);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(RGB565(16, 16, 16), TFT_WHITE);
  if (scanned) { LCD.drawString("已扫码，等手机上批准…", 160, 224); return; }
  char line[64];
  static const char dots[] = { '.', '.', '.', ' ' };
  static int di;
  di = (di + 1) & 3;
  char dd[5] = { dots[di], dots[(di + 1) & 3], dots[(di + 2) & 3], 0 };
  snprintf(line, sizeof(line), "用 Secret-Chat 扫码 %s 剩 %d 秒", dd, (remain_ms > 0 ? remain_ms : 0) / 1000);
  LCD.drawString(line, 160, 224);
}

// ---------------- 配网页（首次开机 SoftAP）----------------

void ui_portal(int clients, uint32_t remain_ms)
{
  g_portal_page = true;
  g_qr_page = false;
  static int last_clients = -1;
  if (clients == last_clients) {   // 整页只画一次，倒计时单独刷那一行（免得每秒闪一下）
    char line[48];
    snprintf(line, sizeof(line), clients > 0 ? "已连上 %d 台，页里填好保存就自动重启" : "等手机连上来…（剩 %d 秒）",
             clients > 0 ? clients : (int)(remain_ms / 1000));
    LCD.fillRect(0, 158, 320, 20, COL_BG);
    LCD.setFont(&fonts::efontCN_14);
    LCD.setTextDatum(textdatum_t::middle_center);
    LCD.setTextColor(clients > 0 ? COL_GREEN : COL_DIM, COL_BG);
    LCD.drawString(line, 160, 168);
    return;
  }
  last_clients = clients;

  LCD.fillScreen(COL_BG);
  LCD.fillRect(0, 0, 320, 24, COL_TOP);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(COL_TEXT, COL_TOP);
  LCD.drawString("给设备配网", 10, 12);
  LCD.setFont(&fonts::Font2);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.setTextColor(COL_YELLOW, COL_TOP);
  LCD.drawString("SOFTAP", 310, 12);

  // 三步：左边做什么，右边具体连什么（值用 Font2，比原来的 6px 清楚很多）
  struct { const char *k, *v; } rows[] = {
    { "1. 手机连这个 WiFi", "Friday-Voice" },
    { "2. 密码", "12345678" },
    { "3. 打开这个地址", "192.168.4.1" },
  };
  for (int i = 0; i < 3; i++) {
    int y = 32 + i * 38;
    LCD.fillRoundRect(8, y, 304, 34, 7, COL_PANEL);
    LCD.drawRoundRect(8, y, 304, 34, 7, COL_BORDER);
    LCD.setFont(&fonts::efontCN_14);
    LCD.setTextDatum(textdatum_t::middle_left);
    LCD.setTextColor(COL_DIM, COL_PANEL);
    LCD.drawString(rows[i].k, 18, y + 17);
    LCD.setFont(&fonts::Font4);
    LCD.setTextDatum(textdatum_t::middle_right);
    LCD.setTextColor(COL_ACCENT, COL_PANEL);
    char v[32];
    strlcpy(v, rows[i].v, sizeof(v));
    clip_px(v, 170);
    LCD.drawString(v, 302, y + 17);
  }

  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(clients > 0 ? COL_GREEN : COL_DIM, COL_BG);
  char line[48];
  snprintf(line, sizeof(line), clients > 0 ? "已连上 %d 台，页里填好保存就自动重启" : "等手机连上来…（剩 %d 秒）",
           clients > 0 ? clients : (int)(remain_ms / 1000));
  LCD.drawString(line, 160, 168);
  LCD.setTextColor(RGB565(96, 108, 132), COL_BG);
  LCD.drawString("配网完会自动去连 chatserver，然后扫码登录", 160, 196);
  LCD.setFont(&fonts::Font2);
  LCD.setTextColor(RGB565(70, 80, 100), COL_BG);
  LCD.drawString(APP_VERSION, 160, 222);
}

// ---------------- bot 选择页 ----------------

static int g_pick_n = -1;
static int g_pick_sel = -1;
static char g_pick_names[5][48];
static int g_pick_y0 = 38, g_pick_bh = 44, g_pick_gap = 6;   // 行几何（点击判定用同一套）

// 行高自适应：2~5 个都把可用高度填满（以前固定 30px，3 个时下面空一大块）
static void pick_layout(int n)
{
  if (n < 1) n = 1;
  const int avail = KEY_Y - 12 - 32;          // 32..188
  g_pick_gap = 6;
  int bh = (avail - (n - 1) * g_pick_gap) / n;
  if (bh > 44) bh = 44;
  if (bh < 26) bh = 26;
  g_pick_bh = bh;
  g_pick_y0 = 32 + (avail - (bh * n + g_pick_gap * (n - 1))) / 2;
}

void ui_pick_bot(char (*names)[48], int n)
{
  if (n > 5) n = 5;
  g_pick_n = n;
  g_pick_sel = -1;
  g_qr_page = false;
  g_portal_page = false;
  g_page = PG_MAIN;
  for (int i = 0; i < n; i++) strlcpy(g_pick_names[i], names[i], sizeof(g_pick_names[i]));
  pick_layout(n);
  LCD.fillScreen(COL_BG);
  LCD.fillRect(0, 0, 320, 24, COL_TOP);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(COL_TEXT, COL_TOP);
  LCD.drawString("选一个 Friday 连", 10, 12);
  for (int i = 0; i < n; i++) {
    int y = g_pick_y0 + i * (g_pick_bh + g_pick_gap);
    LCD.fillRoundRect(6, y, 308, g_pick_bh, 8, COL_PANEL);
    LCD.drawRoundRect(6, y, 308, g_pick_bh, 8, COL_BORDER);
    LCD.fillCircle(26, y + g_pick_bh / 2, 11, RGB565(34, 44, 66));
    char idx[3] = { (char)('0' + i + 1), 0, 0 };   // 最多 5 个，单字符就够
    LCD.setFont(&fonts::Font2);
    LCD.setTextDatum(textdatum_t::middle_center);
    LCD.setTextColor(COL_ACCENT, RGB565(34, 44, 66));
    LCD.drawString(idx, 26, y + g_pick_bh / 2);
    char nm[48];
    strlcpy(nm, g_pick_names[i], sizeof(nm));
    LCD.setFont(&fonts::efontCN_14);
    LCD.setTextDatum(textdatum_t::middle_left);
    LCD.setTextColor(COL_TEXT, COL_PANEL);
    clip_px(nm, 232);
    LCD.drawString(nm, 46, y + g_pick_bh / 2);
  }
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_center);
  LCD.setTextColor(RGB565(110, 122, 146), COL_BG);
  LCD.drawString("点一行连它（机身键在这页不管用）", 160, 218);
}

int ui_pick_result(void) { return g_pick_n >= 0 ? g_pick_sel : -1; }

void ui_pick_reset(void)
{
  g_pick_n = -1;
  g_pick_sel = -1;
  g_qr_page = false;
  g_portal_page = false;
  page_main_full();
}

static void pick_touch(int x, int y)
{
  if (y < g_pick_y0 || y >= KEY_Y) return;
  int idx = (y - g_pick_y0) / (g_pick_bh + g_pick_gap);
  if (idx < 0 || idx >= g_pick_n) return;
  int ry = g_pick_y0 + idx * (g_pick_bh + g_pick_gap);
  if (y >= ry + g_pick_bh) return;
  g_pick_sel = idx;
  LCD.fillRoundRect(6, ry, 308, g_pick_bh, 8, COL_ACCENT);
  LCD.setFont(&fonts::efontCN_14);
  LCD.setTextDatum(textdatum_t::middle_left);
  LCD.setTextColor(TFT_BLACK, COL_ACCENT);
  char nm[48];
  strlcpy(nm, g_pick_names[idx], sizeof(nm));
  clip_px(nm, 232);
  LCD.drawString(nm, 46, ry + g_pick_bh / 2);
  LCD.setTextDatum(textdatum_t::middle_right);
  LCD.drawString("已选", 306, ry + g_pick_bh / 2);
  (void)x;
}

// ---------------- 触摸 / 手势 / 按键 ----------------

static struct { int16_t x, y; uint32_t t; bool down; } g_sw;

static bool in_back(int x, int y)
{
  return x >= BACK_X0 && x <= BACK_X0 + BACK_W + 6 && y >= BACK_Y0 && y <= BACK_Y0 + BACK_H + 4;
}

static void wake(void)
{
  if (g_backlight != 128) ESP_LOGI(UI_TAG, "wake");
  g_last_touch = ui_ms();
  if (g_backlight != 128) { UI_BRIGHT(128); g_backlight = 128; }
}

static void handle_gesture_end(int x, int y)
{
  if (!g_sw.down) return;
  int dx = x - g_sw.x, dy = y - g_sw.y;
  uint32_t dt = ui_ms() - g_sw.t;
  if (!(abs(dx) > 55 && abs(dx) > 2 * abs(dy) && dt < 600)) return;
  if (g_page == PG_MAIN) open_page(dx > 0 ? PG_VOLUME : PG_SESSIONS);
  else if (g_page == PG_VOLUME && dx < 0) go_main();
  else if (g_page == PG_SESSIONS && dx > 0) go_main();
}

static void vol_set(int v)
{
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  if (v == audio_volume()) return;   // 没变就不重画（拖动时每个采样都进来）
  if (g_cb.on_volume_set) g_cb.on_volume_set(v);
  draw_volume();
}

static void handle_tap(int x, int y)
{
  if (y >= KEY_Y) return;   // 键带区触摸 = 物理键，不当触摸用
  if (g_page != PG_MAIN && in_back(x, y)) { go_main(); return; }
  if (g_page == PG_MAIN) {
    if (y < TOP_H) { if (g_cb.on_update) g_cb.on_update(); return; }
    if (x >= ORB_CX - 52 && x <= ORB_CX + 52 && y >= ORB_TOP && y <= BUB_Y - 2) {
      ESP_LOGI(UI_TAG, "tap icon -> stop/clear");
      if (g_cb.on_stop) g_cb.on_stop();          // 点图标：打断播报 / 丢草稿
      return;
    }
    if (y >= BUB_Y && y <= BUB_Y + BUB_HEAD_H) { open_page(PG_SESSIONS); return; }   // 点目标行
    return;
  }
  if (g_page == PG_VOLUME) {
    if (y >= VOL_SL_Y - 18 && y <= VOL_SL_Y + VOL_SL_H + 18) { vol_set((x - VOL_SL_X) * 255 / VOL_SL_W); return; }
    if (x < 80 && y > 110 && y < 172) { vol_set(audio_volume() - 24); return; }
    if (x > 240 && y > 110 && y < 172) { vol_set(audio_volume() + 24); return; }
    if (x >= 92 && x <= 148 && y >= VOL_BTN_CY - 14 && y <= VOL_BTN_CY + 14) { vol_set(0); return; }
    if (x >= 172 && x <= 228 && y >= VOL_BTN_CY - 14 && y <= VOL_BTN_CY + 14) { vol_set(255); return; }
    return;
  }
  if (y < SESS_Y0) return;
  int yy = y + g_sess_scroll - SESS_Y0;
  int idx = yy / (SESS_ROW_H + SESS_ROW_GAP);
  if (yy - idx * (SESS_ROW_H + SESS_ROW_GAP) < SESS_ROW_H) sessions_select(idx);
}

static void handle_drag(int x, int y)
{
  if (g_page == PG_VOLUME && y >= VOL_SL_Y - 22 && y <= VOL_SL_Y + VOL_SL_H + 22) {
    vol_set((x - VOL_SL_X) * 255 / VOL_SL_W);
  } else if (g_page == PG_SESSIONS && abs(y - g_sw.y) > 4) {
    g_sess_scroll -= (y - g_sw.y);
    if (g_sess_scroll < 0) g_sess_scroll = 0;
    static uint32_t last_scroll_draw;
    if (ui_ms() - last_scroll_draw > 60) { last_scroll_draw = ui_ms(); draw_sessions(); }   // 拖动时限流重画
  }
  g_sw.x = x;
  g_sw.y = y;
}

static void touch_tick(void)
{
  if (g_pick_n >= 0 || g_qr_page || g_portal_page) {   // 这些页只吃点击（或根本不吃）
    auto t = M5.Touch.getDetail();
    if (t.state) wake();
    if (g_pick_n >= 0 && t.wasClicked()) pick_touch(t.x, t.y);
    return;
  }
  auto t = M5.Touch.getDetail();
  bool in_keys = t.state && t.y >= KEY_Y;
  if (t.state) wake();
  if (t.wasPressed() && !in_keys) { g_sw.x = t.x; g_sw.y = t.y; g_sw.t = ui_ms(); g_sw.down = true; }
  if (t.isPressed() && !in_keys && g_sw.down) handle_drag(t.x, t.y);
  if (t.wasReleased() && !in_keys) {
    g_sw.down = false;
    int dx = t.x - g_sw.x, dy = t.y - g_sw.y;
    if (abs(dx) < 15 && abs(dy) < 15) handle_tap(t.x, t.y);
    else handle_gesture_end(t.x, t.y);
  }
}

static void buttons_tick(void)
{
  if (g_pick_n >= 0) return;
  // 键 A：按住说话 / 松开结束
  bool a = M5.BtnA.isPressed();
  if (M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) wake();
  if (a != g_hold_down) {
    g_hold_down = a;
    wake();
    if (g_page == PG_MAIN) { draw_keyband(PG_MAIN); draw_bubble(); }
    if (g_cb.on_hold) g_cb.on_hold(a);
  }
  // 键 B：发送
  if (M5.BtnB.wasClicked()) {
    g_b_flash = ui_ms() + 320;
    wake();
    ESP_LOGI(UI_TAG, "key B -> send (page=%d)", (int)g_page);
    if (g_page == PG_MAIN) draw_keyband(PG_MAIN);
    if (g_cb.on_send) g_cb.on_send();
  }
  // 键 C：弃用（回主屏靠反滑 / 点左上「主屏」/ 自动返回；打断和清草稿靠点中间图标）
  if (g_b_flash && (int32_t)(ui_ms() - g_b_flash) >= 0) {
    g_b_flash = 0;
    if (g_page == PG_MAIN) draw_keyband(PG_MAIN);
  }
}

// ---------------- 对外 ----------------

void ui_init(void)
{
#ifdef UI_DUMP
  return;   // 调试构建：画布在 ui_dump_all() 里才建，这里不能碰 LCD
#endif
  LCD.setRotation(1);
  LCD.setTextWrap(false);
  g_page = PG_MAIN;
  g_qr_page = false;
  g_portal_page = false;
  g_last_touch = ui_ms();
  page_main_full();
}

void ui_tick(void)
{
  if (g_qr_page || g_portal_page) { touch_tick(); return; }   // 全屏页自己管画
  if (g_pick_n >= 0) { touch_tick(); return; }
  touch_tick();
  buttons_tick();
  uint32_t now = ui_ms();
  st_t s = snap();

  // 侧页放着不管 → 自动回主屏
  if (g_page == PG_VOLUME && now - g_last_touch > 12000) { go_main(); return; }
  if (g_page == PG_SESSIONS && now - g_last_touch > 20000) { go_main(); return; }

  bool busy = s.held || s.vs == VOICE_SPEAKING || s.vs == VOICE_THINKING || s.vs == VOICE_HEARING;
  if (g_page == PG_MAIN && s.stage == APP_LIVE && !busy && now - g_last_touch > 60000 && g_backlight != 24) {
    UI_BRIGHT(24);   // 节能：一分钟不碰就暗下来
    g_backlight = 24;
    ESP_LOGI(UI_TAG, "dim（省电）");
  }

  if (g_page == PG_VOLUME) {
    if (now - g_last_draw >= 100) { g_last_draw = now; draw_volume_level_only(); }
    return;
  }
  if (g_page == PG_SESSIONS) {
    static uint32_t last_ver = 0xFFFFFFFF;
    uint32_t v = work_sessions_version();
    if (v != last_ver) { last_ver = v; draw_sessions(); }
    return;
  }

  // 主屏：忙 20fps、闲 5fps、暗屏 1fps
  uint32_t period = g_backlight == 24 ? 1000 : (busy ? 50 : 200);
  if (now - g_last_draw >= period) { g_last_draw = now; draw_orb(now, s); }

  static app_stage_t last_stage = APP_BOOT;
  static voice_state_t last_vs = VOICE_NONE;
  static bool last_held;
  static char last_status[96], last_caption[512], last_target[64];
  static uint32_t last_bubble;
  bool stage_changed = s.stage != last_stage || s.vs != last_vs;
  bool held_changed = s.held != last_held;
  bool target_changed = strcmp(last_target, ui_target_label()) != 0;
  app_lock();
  bool status_changed = strcmp(g_app.status_text, last_status) != 0;
  bool caption_changed = strcmp(g_app.caption, last_caption) != 0;
  app_unlock();
  if (stage_changed || status_changed) draw_top();
  if (status_changed) draw_statusline();
  if ((caption_changed || target_changed || held_changed || stage_changed) && now - last_bubble > 200) {
    draw_bubble();
    last_bubble = now;
    app_lock();
    strlcpy(last_caption, g_app.caption, sizeof(last_caption));
    strlcpy(last_status, g_app.status_text, sizeof(last_status));
    app_unlock();
    strlcpy(last_target, ui_target_label(), sizeof(last_target));
    if (s.held) draw_keyband(g_page);
  }
  last_stage = s.stage;
  last_vs = s.vs;
  last_held = s.held;
}

void ui_set_callbacks(const ui_callbacks_t *cb) { g_cb = *cb; }

#ifdef UI_DUMP
// ---------------- 调试：把各页面渲染成位图，POST 回电脑看版式 ----------------
// 编译：idf.py -B build-dump -DCMAKE_CXX_FLAGS="-DUI_DUMP" build（或设环境变量 UI_DUMP=1）
// 电脑端：node tools/ui-dump-server.mjs → node tools/ui-dump-decode.mjs → node tools/ui-dump-inspect.mjs
extern "C" void work_dump_seed_sessions(void);

static char *g_dump_buf;          // 256KB，从 PSRAM 分（DRAM 不够）
#define DUMP_BUF_SIZE 262144

static void dump_hex4(char *out, int *pos, uint16_t v)
{
  static const char H[] = "0123456789ABCDEF";
  for (int k = 3; k >= 0; k--) out[(*pos)++] = H[(v >> (4 * k)) & 0xF];
}

// 把一页存进 PSRAM 待发列表（调试构建里设备自己起个 HTTP 服务，电脑来拉；
// 反过来设备 POST 到电脑会被 Windows 防火墙挡住）
typedef struct { const char *name; char *data; int len; } dump_page_t;
#define DUMP_MAX_PAGES 24
static dump_page_t g_dump_pages[DUMP_MAX_PAGES];
static int g_dump_n;

static void dump_emit(const char *name)
{
  const uint16_t *px = (const uint16_t *)g_canvas->getBuffer();
  const int total = 320 * 240;
  int pos = 0;
  if (g_dump_n >= DUMP_MAX_PAGES) return;
  if (!g_dump_buf) return;
  g_dump_buf[0] = 0;
  for (int i = 0; i < total && pos + 8 < DUMP_BUF_SIZE; ) {
    uint16_t c = px[i];
    int n = 1;
    while (i + n < total && px[i + n] == c && n < 65535) n++;
    i += n;
    dump_hex4(g_dump_buf, &pos, (uint16_t)n);
    dump_hex4(g_dump_buf, &pos, c);
  }
  g_dump_buf[pos] = 0;
  // 搬到一个固定名字的小缓冲里（每个页面一份）
  char *copy = (char *)heap_caps_malloc(pos + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) return;
  memcpy(copy, g_dump_buf, pos + 1);
  g_dump_pages[g_dump_n++] = (dump_page_t){ name, copy, pos };
  ESP_LOGI(UI_TAG, "dump %s：%d 字节，共 %d 页", name, pos, g_dump_n);
}

static void dump_main(const char *name, app_stage_t stage, voice_state_t vs,
                      const char *status, const char *caption, bool held, int lv)
{
  g_app.stage = stage; g_app.voice_state = vs; g_app.held = held;
  g_app.voice_available = true; g_app.battery_pct = 76; g_app.charging = false; g_app.rssi = -52;
  strlcpy(g_app.bot_name, "Friday 2.79.0", sizeof(g_app.bot_name));
  app_set_status("%s", status);
  app_set_caption(caption);
  g_hold_down = held;
  g_dump_mic = held ? lv : 0;
  g_dump_play = (vs == VOICE_SPEAKING) ? lv : 0;
  page_main_full();
  for (int i = 0; i < ORB_SPOKES; i++)
    g_lv[i] = (uint8_t)(lv * (0.30f + 0.70f * fabsf(sinf(i * 0.7f + 0.4f))));
  draw_orb(ui_ms(), snap());
  dump_emit(name);
}

extern "C" void ui_dump_all(void)   // app_main 里按 C 名字调
{
  g_canvas = new m5gfx::LGFX_Sprite();
  g_dump_buf = (char *)heap_caps_malloc(DUMP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_dump_buf) { ESP_LOGE(UI_TAG, "dump 缓冲没分到（PSRAM？）"); return; }
  g_canvas->setColorDepth(m5gfx::color_depth_t::rgb565_2Byte);
  g_canvas->setPsram(true);
  if (!g_canvas->createSprite(320, 240)) { ESP_LOGE(UI_TAG, "画布没建起来（PSRAM？）"); return; }
  g_canvas->setTextWrap(false);

  dump_main("01-main-idle", APP_LIVE, VOICE_LISTENING, "按住 A 键说话，松开结束；B 键发送", "", false, 0);
  dump_main("02-main-held", APP_LIVE, VOICE_HEARING, "听到你在说…", "今天北京多云，气温 18 到 26 度，", true, 190);
  dump_main("03-main-think", APP_LIVE, VOICE_THINKING, "助手在想…", "今天北京多云，气温 18 到 26 度，下午西风 3 级。", false, 0);
  dump_main("04-main-speak", APP_LIVE, VOICE_SPEAKING, "助手在念",
            "好的，我把今天的安排念给你听：上午十点站会，下午两点评审，晚上记得把固件烧回去。", false, 150);
  // 目标 = 某个会话 + 超长字幕（看尾部截断和折行）
  g_target.assistant = false;
  strlcpy(g_target.id, "conv-1002", sizeof(g_target.id));
  strlcpy(g_target.label, "重构 Friday Work 的登录流程", sizeof(g_target.label));
  strlcpy(g_sel_id, "conv-1002", sizeof(g_sel_id));
  dump_main("05-main-session-target", APP_LIVE, VOICE_SPEAKING, "已发到会话：重构 Friday Work 的登录流程",
            "第一段：这个改动涉及登录、token 刷新、设备批准三个地方；第二段：所以我把会话列表顶部固定了一个助手，默认选中它；"
            "第三段：很长很长很长很长很长很长很长很长很长很长很长很长的一行用来验证折行和截断位置对不对看不看得清", false, 210);
  g_target.assistant = true;
  strlcpy(g_target.label, "助手", sizeof(g_target.label));
  strlcpy(g_target.id, "", sizeof(g_target.id));
  g_sel_id[0] = '\0';
  dump_main("06-connecting", APP_DIAL, VOICE_NONE, "呼叫机器人（work）…", "", false, 0);

  // 音量页
  g_app.stage = APP_LIVE;
  g_page = PG_VOLUME;
  audio_set_volume(200); g_dump_play = 130; draw_volume(); dump_emit("07-volume");
  audio_set_volume(0); g_dump_play = 0; draw_volume(); dump_emit("08-volume-mute");

  // 会话页
  work_dump_seed_sessions();
  g_page = PG_SESSIONS; g_sess_scroll = 0;
  draw_sessions(); dump_emit("09-sessions");
  g_sess_scroll = 60; draw_sessions(); dump_emit("10-sessions-scroll");
  g_sess_scroll = 0; strlcpy(g_sel_id, "conv-1001", sizeof(g_sel_id));
  draw_sessions(); dump_emit("11-sessions-selected");
  strlcpy(g_sel_id, "", sizeof(g_sel_id));

  // 扫码页
  ui_draw("https://chat.ireoo.com/#/login?code=3f9c2a7e1b8d4c6a92b1&device=76d7b3f998205a4bc832bbb36b6f8b74"
          "&name=M5Stack%20Core2&server=https://47.100.98.3:2012");
  dump_emit("12-qr");
  ui_qr_hint(true, 0);
  dump_emit("13-qr-scanned");

  // 配网页
  ui_portal(1, 240000);
  dump_emit("14-portal");

  // 选机器人页
  static char names[3][48];
  strlcpy(names[0], "Friday 2.79.0（DESKTOP-QJ8JHB5）", 48);
  strlcpy(names[1], "Friday 工作台", 48);
  strlcpy(names[2], "Friday 测试机", 48);
  ui_pick_bot(names, 3);
  dump_emit("15-pick");
  ESP_LOGW(UI_TAG, "@@DUMP 一轮跑完（共 %d 页）", g_dump_n);
}

// 调试构建的设备端 HTTP 服务：GET / 给页名清单，GET /<名> 给那页的 RLE 文本。
// 电脑主动来拉（避开 Windows 防火墙拦被连），tools/ui-dump-fetch.mjs 负责拉+存。
#include "esp_http_server.h"

static esp_err_t dump_handler(httpd_req_t *req)
{
  const char *path = req->uri;   // 结构体里直接带（IDF 5.5 没有 get_url_path_str）
  if (!strcmp(path, "/") || !strcmp(path, "")) {
    char idx[1024];
    int n = 0;
    idx[0] = 0;
    for (int i = 0; i < g_dump_n; i++) n += snprintf(idx + n, sizeof(idx) - n, "%s%s", i ? "\n" : "", g_dump_pages[i].name);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, idx, n);
    return ESP_OK;
  }
  const char *want = path[0] == '/' ? path + 1 : path;
  for (int i = 0; i < g_dump_n; i++) {
    if (strcmp(g_dump_pages[i].name, want)) continue;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, g_dump_pages[i].data, g_dump_pages[i].len);
    return ESP_OK;
  }
  httpd_resp_send_404(req);
  return ESP_OK;
}

void ui_dump_serve(void)
{
  ui_dump_all();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 8098;
  cfg.stack_size = 8192;
  cfg.uri_match_fn = httpd_uri_match_wildcard;   // ★ 不设的话 "/*" 会被当字面量（默认精确匹配）
  httpd_handle_t h = NULL;
  if (httpd_start(&h, &cfg) != ESP_OK || !h) { ESP_LOGE(UI_TAG, "dump 服务起不来"); return; }
  httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = dump_handler, .user_ctx = NULL };
  httpd_uri_t uri_all = { .uri = "/*", .method = HTTP_GET, .handler = dump_handler, .user_ctx = NULL };
  httpd_register_uri_handler(h, &uri_root);    // 根路径单独注册：只注 "/*" 时 "/" 会 404
  httpd_register_uri_handler(h, &uri_all);
  ESP_LOGW(UI_TAG, "@@DUMP 服务已起：http://<设备IP>:8098/");
}
#endif   // UI_DUMP
