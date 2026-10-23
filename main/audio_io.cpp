// audio_io.cpp — 麦克风（M5Unified Mic，PDM/I2S0）+ 喇叭（自己开 I2S1，不用 M5Unified Speaker）
//
// 为什么喇叭自己推 I2S：
//   1. M5Unified Speaker 每次 playRaw 都是一次“请求”，块边界会重启播放器 → 听感顿挫；
//      它的 isPlaying()/释放回调又报得很早，不能当播放进度用。
//   2. 直接推 I2S 就是一条不断的流，没有块边界，也不丢帧。
// 踩过的坑：
//   - GPIO0 是麦克风 PDM CLK 和喇叭 WS 共用的：开喇叭前必须 M5.Mic.end()，否则一行数据都送不到功放。
//   - 功放在 Core2 v1.1（AXP2101）上是 ALDO3；老 Core2（AXP192）是 GPIO2。
//   - 数据 16k，声卡 48k：自己 3 倍上采样，别交给库重采样（会出呲呲声）。
#include "audio_io.h"
#include "work.h"
#include "app.h"
#include "M5Unified.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

// ---------------- 喇叭硬件 ----------------
#define SPK_PORT I2S_NUM_1
#define SPK_BCK  GPIO_NUM_12
#define SPK_WS   GPIO_NUM_0     // 与麦克风 PDM CLK 共用
#define SPK_DOUT GPIO_NUM_2
#define SPK_RATE 48000
#define SPK_UP   3              // 16k → 48k
#define SPK_CHUNK 320           // 16k 侧一次取 20ms

static i2s_chan_handle_t g_tx;

static void amp_enable(bool on)
{
  switch (M5.Power.getType()) {
    case m5::Power_Class::pmic_axp192:  M5.Power.Axp192.setGPIO2(on); break;
    case m5::Power_Class::pmic_axp2101: M5.Power.Axp2101.setALDO3(on ? 3300 : 0); break;
    default: break;
  }
  ESP_LOGI(TAG, "功放 %s（pmic type=%d）", on ? "开" : "关", (int)M5.Power.getType());
}

static bool spk_hw_start(void)
{
  if (g_tx) return true;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(SPK_PORT, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 512;        // 8×512 帧 ≈ 85ms @48k
  cc.auto_clear = true;
  esp_err_t err = i2s_new_channel(&cc, &g_tx, NULL);
  if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err)); g_tx = NULL; return false; }
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = SPK_BCK;
  sc.gpio_cfg.ws = SPK_WS;
  sc.gpio_cfg.dout = SPK_DOUT;
  sc.gpio_cfg.din = I2S_GPIO_UNUSED;
  err = i2s_channel_init_std_mode(g_tx, &sc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s init std: %s", esp_err_to_name(err));
    i2s_del_channel(g_tx);
    g_tx = NULL;
    return false;
  }
  i2s_channel_enable(g_tx);
  ESP_LOGI(TAG, "喇叭就绪：%d Hz / 16bit / 立体声（bck=%d ws=%d dout=%d）",
           SPK_RATE, SPK_BCK, SPK_WS, SPK_DOUT);
  return true;
}

static void spk_hw_stop(void)
{
  if (!g_tx) return;
  i2s_channel_disable(g_tx);
  i2s_del_channel(g_tx);        // 释放 GPIO0 给 PDM
  g_tx = NULL;
}

// ---------------- 环形缓冲（PSRAM，2 秒） ----------------
#define RING_MS 2000
#define RING_SAMPLES (VOICE_SAMPLE_RATE * RING_MS / 1000)
static int16_t *g_ring;
static size_t g_ring_len, g_ring_head;
static SemaphoreHandle_t g_ring_mu;

static size_t ring_push(const int16_t *pcm, size_t n)
{
  if (!g_ring || !g_ring_mu || !n) return 0;
  xSemaphoreTake(g_ring_mu, portMAX_DELAY);
  size_t room = RING_SAMPLES - g_ring_len;
  size_t put = n < room ? n : room;
  for (size_t i = 0; i < put; i++) g_ring[(g_ring_head + i) % RING_SAMPLES] = pcm[i];
  g_ring_head = (g_ring_head + put) % RING_SAMPLES;
  g_ring_len += put;
  xSemaphoreGive(g_ring_mu);
  return put;
}

static size_t ring_peek(int16_t *out, size_t n)
{
  if (!g_ring_mu) return 0;
  xSemaphoreTake(g_ring_mu, portMAX_DELAY);
  size_t got = n < g_ring_len ? n : g_ring_len;
  size_t start = (g_ring_head + RING_SAMPLES - g_ring_len) % RING_SAMPLES;
  for (size_t i = 0; i < got; i++) out[i] = g_ring[(start + i) % RING_SAMPLES];
  xSemaphoreGive(g_ring_mu);
  return got;
}

static void ring_drop_front(size_t n)
{
  if (!g_ring_mu) return;
  xSemaphoreTake(g_ring_mu, portMAX_DELAY);
  if (n > g_ring_len) n = g_ring_len;
  g_ring_len -= n;
  xSemaphoreGive(g_ring_mu);
}

static size_t ring_len(void)
{
  if (!g_ring_mu) return 0;
  xSemaphoreTake(g_ring_mu, portMAX_DELAY);
  size_t n = g_ring_len;
  xSemaphoreGive(g_ring_mu);
  return n;
}

static void ring_clear(void)
{
  if (!g_ring_mu) return;
  xSemaphoreTake(g_ring_mu, portMAX_DELAY);
  g_ring_len = 0;
  g_ring_head = 0;
  xSemaphoreGive(g_ring_mu);
}

// ---------------- 半双工 / 状态 ----------------
static bool g_running, g_spk_on, g_mic_on, g_paused, g_duck;
static volatile bool g_hold;      // 按住说话（mic_task 看这个）
static int16_t g_mic_buf[2][VOICE_FEED_SAMPLES];
static volatile bool g_spk_writing;
static int g_volume = 200, g_duck_volume = 80;
static uint32_t g_last_play_ms;
static SemaphoreHandle_t g_switch_mu = NULL;

static uint32_t now_ms_(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

static bool g_mic_cfg_done;

static void mic_off_locked(void)
{
  if (!g_mic_on) return;
  M5.Mic.end();
  g_mic_on = false;
  ESP_LOGI(TAG, "mic off");
}

static void spk_on_locked(void)
{
  if (g_spk_on) return;
  // GPIO0 与 PDM CLK 共用：M5.begin(internal_mic=true) 已经把麦开起来了，必须先收走
  if (g_mic_on || M5.Mic.isEnabled()) {
    M5.Mic.end();
    g_mic_on = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "mic off (half-duplex, 喇叭要 GPIO0)");
  }
  amp_enable(true);
  if (!spk_hw_start()) { amp_enable(false); return; }
  g_spk_on = true;
}

static void spk_off_locked(void)
{
  if (!g_spk_on) return;
  vTaskDelay(pdMS_TO_TICKS(20));
  spk_hw_stop();
  amp_enable(false);
  g_spk_on = false;
  ESP_LOGI(TAG, "spk off (half-duplex)");
}

static void mic_on_locked(void)
{
  if (g_mic_on) return;
  if (g_spk_on) {
    spk_off_locked();
    vTaskDelay(pdMS_TO_TICKS(30));   // 让 I2S1 引脚彻底放开（GPIO0 给 PDM）
  }
  if (!g_mic_cfg_done) {
    // M5Unified 的 mic_task 默认不限核（重负载下落 CPU0 和 WiFi/esp_peer 抢，
    // i2s_channel_read 阻塞会饿死中断看门狗）。钉 CPU1 + 优先级 6。
    // 增益/噪声抑制保持默认：实测抬到 24/32 并没变好（32 还会削顶）。
    m5::mic_config_t cfg = M5.Mic.config();
    cfg.task_pinned_core = 1;
    cfg.task_priority = 6;
    M5.Mic.config(cfg);
    g_mic_cfg_done = true;
  }
  M5.Mic.begin();
  g_mic_on = M5.Mic.isEnabled();
  ESP_LOGI(TAG, "mic on = %d", g_mic_on);
}

// ---------------- 播放：环形缓冲 → 48k 上采样 → I2S 阻塞写 ----------------
static void feed_task(void *arg)
{
  (void)arg;
  static int16_t src[SPK_CHUNK];
  static int16_t out[SPK_CHUNK * SPK_UP * 2];   // 立体声：每帧两份
  static int16_t hold = 0;                      // 上一块最后一个采样，保证拼接连续
  TickType_t wake = xTaskGetTickCount();
  uint32_t last_warn = 0;

  while (g_running) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    if (g_paused || ring_len() == 0) continue;
    if (!g_spk_on) {
      xSemaphoreTake(g_switch_mu, portMAX_DELAY);
      if (!g_mic_on) spk_on_locked();
      xSemaphoreGive(g_switch_mu);
      continue;
    }
    size_t n = ring_peek(src, SPK_CHUNK);
    if (!n) continue;

    // 16k → 48k 线性插值 + 音量，再复制到左右声道
    int vol = g_duck ? g_duck_volume : g_volume;
    for (size_t i = 0; i < n; i++) {
      int16_t a = (i == 0) ? hold : src[i - 1];
      int16_t b = src[i];
      for (int k = 0; k < SPK_UP; k++) {
        int32_t s = a + (int32_t)(b - a) * k / SPK_UP;
        if (vol < 255) s = s * vol / 255;
        int16_t v = (int16_t)s;
        out[(i * SPK_UP + k) * 2] = v;
        out[(i * SPK_UP + k) * 2 + 1] = v;
      }
    }
    hold = src[n - 1];
    size_t bytes = n * SPK_UP * 2 * sizeof(int16_t);

    g_spk_writing = true;
    size_t wrote = 0;
    esp_err_t err = i2s_channel_write(g_tx, out, bytes, &wrote, pdMS_TO_TICKS(500));
    g_spk_writing = false;
    if (err == ESP_OK && wrote == bytes) {
      ring_drop_front(n);
      g_last_play_ms = now_ms_();
    } else if (now_ms_() - last_warn > 1000) {
      last_warn = now_ms_();
      ESP_LOGW(TAG, "i2s write %s（%u/%u 字节）", esp_err_to_name(err),
               (unsigned)wrote, (unsigned)bytes);
    }
  }
  vTaskDelete(NULL);
}

// ---------------- 对外接口 ----------------
void audio_play(const int16_t *pcm, int samples)
{
  if (g_paused || !pcm || samples <= 0) return;
  if (!g_ring_mu || !g_switch_mu) return;
  if (!g_spk_on && !g_mic_on) {
    xSemaphoreTake(g_switch_mu, portMAX_DELAY);
    spk_on_locked();
    xSemaphoreGive(g_switch_mu);
  }
  size_t put = ring_push(pcm, (size_t)samples);
  if (put < (size_t)samples) {
    static uint32_t drop_cnt;
    if (++drop_cnt % 20 == 1)
      ESP_LOGW(TAG, "待播缓冲满（%d 秒），丢 %u 采样", RING_MS / 1000, (unsigned)((size_t)samples - put));
  }
  g_last_play_ms = now_ms_();
}

void audio_clear(void) { ring_clear(); }

bool audio_playing(void)
{
  return ring_len() > 0 || g_spk_writing;
}

uint32_t audio_last_play_ms(void) { return g_last_play_ms; }

void audio_set_volume(int v01)
{
  if (v01 < 0) v01 = 0;
  if (v01 > 255) v01 = 255;
  g_volume = v01;
  g_duck_volume = v01 * 2 / 5;
}

int audio_volume(void) { return g_volume; }

void audio_duck(bool on) { g_duck = on; }

void audio_pause(bool paused)
{
  g_paused = paused;
  if (paused) audio_clear();
}

void audio_hold(bool down)
{
  if (!g_switch_mu) return;
  g_hold = down;
  if (down) {
    xSemaphoreTake(g_switch_mu, portMAX_DELAY);
    audio_clear();            // 按下说话：把还没播完的 TTS 丢掉（打断）
    mic_on_locked();
    xSemaphoreGive(g_switch_mu);
  } else {
    xSemaphoreTake(g_switch_mu, portMAX_DELAY);
    mic_off_locked();
    xSemaphoreGive(g_switch_mu);
  }
  ESP_LOGI(TAG, "hold %s", down ? "DOWN" : "UP");
}

// ---------------- 麦克风读取任务 ----------------
static void mic_task(void *arg)
{
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  int slot = 0, skip = 4;   // 麦刚上电第一帧会削顶（爆音），跳过前几拍
  while (g_running) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
    if (!g_hold || g_paused || !g_mic_on) { skip = 4; continue; }
    if (!M5.Mic.isEnabled()) continue;
    if (skip > 0) { skip--; continue; }
    if (!M5.Mic.record(g_mic_buf[slot], VOICE_FEED_SAMPLES, VOICE_SAMPLE_RATE)) continue;
    work_feed_audio(g_mic_buf[slot], VOICE_FEED_SAMPLES);
    slot ^= 1;
  }
  vTaskDelete(NULL);
}

void audio_start(void)
{
  if (g_running) return;
  if (!g_switch_mu) g_switch_mu = xSemaphoreCreateMutex();
  if (!g_ring_mu) g_ring_mu = xSemaphoreCreateMutex();
  if (!g_ring) {
    g_ring = (int16_t *)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!g_ring) g_ring = (int16_t *)malloc(RING_SAMPLES * sizeof(int16_t));
    if (g_ring) ESP_LOGI(TAG, "待播缓冲 %d KB（%d ms）", (int)(RING_SAMPLES * 2 / 1024), RING_MS);
    else ESP_LOGE(TAG, "待播缓冲分配失败");
  }
  g_running = true;
  ring_clear();
  M5.Mic.end();
  g_mic_on = false;
  xSemaphoreTake(g_switch_mu, portMAX_DELAY);
  spk_on_locked();      // 初始只要喇叭（等 TTS）；麦克风等按住再开
  xSemaphoreGive(g_switch_mu);
  xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(feed_task, "ttsfeed", 4096, NULL, 6, NULL, 1);
  ESP_LOGI(TAG, "audio started");
}

void audio_stop(void)
{
  g_running = false;
  vTaskDelay(pdMS_TO_TICKS(150));
  if (g_switch_mu) xSemaphoreTake(g_switch_mu, portMAX_DELAY);
  mic_off_locked();
  ring_clear();
  spk_off_locked();
  if (g_switch_mu) xSemaphoreGive(g_switch_mu);
}

// ---------------- 调试用 ----------------
// 灌一段 440Hz 验喇叭（一次性灌满环：真实 TTS 也是一整段推过来）
void audio_beep(int ms) __attribute__((unused));
void audio_beep(int ms)
{
  int total = VOICE_SAMPLE_RATE * ms / 1000;
  int16_t *buf = (int16_t *)heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!buf) return;
  for (int i = 0; i < total; i++)
    buf[i] = (int16_t)(20000.0f * sinf(2.0f * (float)M_PI * 440.0f * i / VOICE_SAMPLE_RATE));
  audio_play(buf, total);
  vTaskDelay(pdMS_TO_TICKS(ms + 100));
  free(buf);
  ESP_LOGI(TAG, "[BEEP] 播了 %d ms（一次灌满）", ms);
}
