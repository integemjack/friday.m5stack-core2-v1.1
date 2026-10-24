// ota.cpp — 固件自更新：GitHub Releases
//
// 手动：主界面点顶部状态栏 → 检查 → 有新版则下载写入备用 OTA 分区 → 重启
// 自动：开机 30s 后静默查一次，之后每 6h 一次；发现新版只在状态栏提示，绝不自动装
// 下载源：latest release 的 m5_friday_voice.bin（app 镜像；CI 打 tag 时发布）
// 证书：GitHub 必须真校验（esp_crt_bundle），与 chatserver 的自签跳过（全局
//       SKIP_SERVER_CERT_VERIFY）不同——显式给了 bundle 就按 bundle 校验。
// 网络：设备需直连 api.github.com / release-assets.githubusercontent.com。
#include "ota.h"
#include "app.h"
#include "ui.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota";

#define OTA_API_URL "https://api.github.com/repos/integemjack/friday.m5stack-core2-v1.1/releases/latest"
#define OTA_ASSET_NAME "m5_friday_voice.bin"
// ota 任务 0.3.1 栈是 12288：TLS1.3 握手+证书链 ECDSA 验签偶发 xQueueGiveFromISR assert 崩整机重启
// （开机 30s 自动检查正好撞上用户发消息等回复的窗口，回复被重启吞掉）。20KB 栈+错峰双保险。
#define OTA_CHECK_PERIOD_MS (6 * 3600 * 1000)
#define EV_MANUAL BIT0

char ota_latest_ver[24];
static volatile bool s_busy;
static char s_http_buf[32768];    // latest release JSON（可能带长 changelog，给足）
static EventGroupHandle_t s_ev;

bool ota_busy(void) { return s_busy; }

bool ota_newer_available(void)
{
  return ota_latest_ver[0] != '\0' && ota_ver_newer(ota_latest_ver, APP_VERSION);
}

bool ota_ver_newer(const char *latest, const char *cur)
{
  while (*latest == 'v' || *latest == 'V') latest++;
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  sscanf(latest, "%d.%d.%d", &a[0], &a[1], &a[2]);
  sscanf(cur, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; i++)
    if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

// GET（手动跟 30x，最多 5 跳）。200 时把 body 写进 out；返回状态码，-1 = 网络失败
static int http_get(const char *url, char *out, size_t cap)
{
  char next[512];
  strlcpy(next, url, sizeof(next));
  for (int hop = 0; hop < 5; hop++) {
    esp_http_client_config_t cfg = {};
    cfg.url = next;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // GitHub：真证书链校验
    cfg.disable_auto_redirect = true;                // esp_http_client 不自动跟重定向
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "User-Agent", "m5-friday-voice-ota");   // GitHub API 必须带 UA
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
      esp_http_client_fetch_headers(c);
      status = esp_http_client_get_status_code(c);
      if (status >= 300 && status < 400) {
        char *loc = NULL;
        if (esp_http_client_get_header(c, "Location", &loc) == ESP_OK &&
            loc && strstr(loc, "http") == loc) {
          strlcpy(next, loc, sizeof(next));
          esp_http_client_cleanup(c);
          continue;
        }
      } else if (status == 200 && out) {
        size_t n = 0;
        while (n < cap - 1) {
          int want = cap - 1 - n > 4096 ? 4096 : (int)(cap - 1 - n);
          int got = esp_http_client_read(c, out + n, want);
          if (got <= 0) break;
          n += got;
        }
        out[n] = '\0';
      }
    }
    esp_http_client_cleanup(c);
    return status;
  }
  return -1;
}

// 最新版本号 + 固件下载地址
static bool fetch_latest(char *ver, size_t vcap, char *url, size_t ucap)
{
  int st = http_get(OTA_API_URL, s_http_buf, sizeof(s_http_buf));
  if (st != 200) {
    ESP_LOGW(TAG, "releases/latest HTTP %d", st);
    return false;
  }
  cJSON *j = cJSON_Parse(s_http_buf);
  if (!j) {
    ESP_LOGW(TAG, "release JSON 解析失败");
    return false;
  }
  bool ok = false;
  const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tag_name"));
  if (tag) {
    strlcpy(ver, tag, vcap);
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "assets")) {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
      const char *dl = cJSON_GetStringValue(cJSON_GetObjectItem(it, "browser_download_url"));
      if (name && dl && strcmp(name, OTA_ASSET_NAME) == 0) {
        strlcpy(url, dl, ucap);
        ok = true;
        break;
      }
    }
  }
  cJSON_Delete(j);
  return ok;
}

// 下载 app 镜像写入备用 OTA 分区（不切启动分区、不重启，由调用方收尾）
static bool fetch_and_flash(const char *url)
{
  char cur[512];
  strlcpy(cur, url, sizeof(cur));
  esp_http_client_handle_t c = NULL;
  for (int hop = 0; hop < 5 && !c; hop++) {
    esp_http_client_config_t cfg = {};
    cfg.url = cur;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "m5-friday-voice-ota");
    if (esp_http_client_open(c, 0) != ESP_OK) {
      esp_http_client_cleanup(c);
      return false;
    }
    esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st >= 300 && st < 400) {
      char *loc = NULL;
      if (esp_http_client_get_header(c, "Location", &loc) == ESP_OK &&
          loc && strstr(loc, "http") == loc) {
        strlcpy(cur, loc, sizeof(cur));
        esp_http_client_cleanup(c);
        c = NULL;
        continue;
      }
    }
    if (st != 200) {
      ESP_LOGW(TAG, "固件 HTTP %d", st);
      esp_http_client_cleanup(c);
      return false;
    }
  }
  if (!c) return false;

  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (!part) {
    esp_http_client_cleanup(c);
    ESP_LOGE(TAG, "没有 OTA 分区（分区表对吗？）");
    return false;
  }
  esp_ota_handle_t oh;
  esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &oh);
  if (err != ESP_OK) {
    esp_http_client_cleanup(c);
    ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
    return false;
  }
  int64_t total = esp_http_client_get_content_length(c);
  int64_t done = 0;
  uint32_t last_pct = UINT32_MAX;
  bool fail = false;
  char buf[4096];
  int n;
  while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
    if (esp_ota_write(oh, buf, n) != ESP_OK) {
      fail = true;
      break;
    }
    done += n;
    if (total > 0) {
      uint32_t pct = (uint32_t)(done * 100 / total);
      if (pct != last_pct) {
        app_set_status("OTA 下载 %u%%", pct);
        last_pct = pct;
      }
    }
  }
  esp_http_client_cleanup(c);
  if (fail || (total > 0 && done < total)) {
    esp_ota_abort(oh);
    ESP_LOGE(TAG, "下载不完整（%lld/%lld 字节）", (long long)done, (long long)total);
    return false;
  }
  err = esp_ota_end(oh);   // 含镜像头/校验和验证
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ota_end: %s（镜像损坏？）", esp_err_to_name(err));
    return false;
  }
  err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "新固件已写入 %s（%lld 字节）", part->label, (long long)done);
  return true;
}

static void ota_task(void *arg)
{
  bool first = true;
  while (true) {
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_MANUAL, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(first ? 30000 : OTA_CHECK_PERIOD_MS));
    first = false;
    bool manual = (b & EV_MANUAL) != 0;
    // 错峰：自动检查等屏幕全黑（用户不在）再跑，避免 TLS 握手撞上语音会话；手动检查不等
    if (!manual) {
      for (int i = 0; i < 120 && ui_backlight() != 0; i++) {
        b = xEventGroupWaitBits(s_ev, EV_MANUAL, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (b & EV_MANUAL) { manual = true; break; }   // 等的过程中用户点了手动：立即跑
      }
      if (!manual) ESP_LOGI(TAG, "屏幕全黑，错峰跑自动检查");
    }
    s_busy = true;
    char ver[24], url[512];
    if (!fetch_latest(ver, sizeof(ver), url, sizeof(url))) {
      if (manual) app_set_status("检查更新失败（GitHub 连不上？）");
    } else {
      ESP_LOGI(TAG, "ota 栈余 %u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));   // IDF 单位就是字节
      strlcpy(ota_latest_ver, (*ver == 'v' || *ver == 'V') ? ver + 1 : ver, sizeof(ota_latest_ver));
      if (!ota_ver_newer(ota_latest_ver, APP_VERSION)) {
        if (manual) app_set_status("固件 %s 已是最新", APP_VERSION);
      } else if (manual) {
        app_set_status("下载 %s…", ota_latest_ver);
        app_set_caption("");
        if (!fetch_and_flash(url)) {
          app_set_status("OTA 失败，稍后再试");
        } else {
          app_set_status("v%s 已装好，重启", ota_latest_ver);
          vTaskDelay(pdMS_TO_TICKS(2500));
          esp_restart();
        }
      } else {
        app_set_status("有新版 v%s（点顶部状态栏安装）", ota_latest_ver);
      }
    }
    s_busy = false;
  }
}

void ota_init(void)
{
  s_ev = xEventGroupCreate();
  xTaskCreate(ota_task, "ota", 20480, NULL, 3, NULL);   // TLS 验签吃栈，12KB 偏紧
}

bool ota_check_now(void)
{
  if (!s_ev || s_busy) return false;
  xEventGroupSetBits(s_ev, EV_MANUAL);
  return true;
}
