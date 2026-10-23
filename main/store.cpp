#include "store.h"
#include "util.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "store";
// 生产 chatserver（Friday Work 同款默认值，配网页可改）
static const char *DEFAULT_SCHAT_SERVER = "https://47.100.98.3:2012";
store_t g_store;
static nvs_handle_t h;

static void rd_str(const char *key, char *buf, size_t cap)
{
  size_t len = cap;
  if (nvs_get_str(h, key, buf, &len) != ESP_OK) buf[0] = '\0';
}
static void rd_i64(const char *key, int64_t *v)
{
  if (nvs_get_i64(h, key, v) != ESP_OK) *v = 0;
}
static void wr_str(const char *key, const char *v)
{
  if (v && v[0]) nvs_set_str(h, key, v);
  else nvs_set_str(h, key, "");
  nvs_commit(h);
}
static void wr_i64(const char *key, int64_t v)
{
  nvs_set_i64(h, key, v);
  nvs_commit(h);
}

void store_init(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(nvs_open("m5fri", NVS_READWRITE, &h));
  memset(&g_store, 0, sizeof(g_store));
  rd_str("srv", g_store.server, sizeof(g_store.server));
  if (!g_store.server[0])
    strncpy(g_store.server, DEFAULT_SCHAT_SERVER, sizeof(g_store.server) - 1);
  rd_str("dev", g_store.device_id, sizeof(g_store.device_id));
  rd_str("tok", g_store.token, sizeof(g_store.token));
  rd_str("rfr", g_store.refresh, sizeof(g_store.refresh));
  rd_i64("uid", &g_store.user_id);
  rd_i64("bid", &g_store.bot_user_id);
  rd_str("bnm", g_store.bot_name, sizeof(g_store.bot_name));
  // token 死线是「本机开机相对毫秒」（esp_timer），跨重启无意义：不读回，启动/重连前一律先 refresh
  g_store.expires_at_ms = 0;
  rd_str("ssid", g_store.wifi_ssid, sizeof(g_store.wifi_ssid));
  rd_str("pass", g_store.wifi_pass, sizeof(g_store.wifi_pass));
  {
    int64_t v = 200;
    rd_i64("vol", &v);
    g_store.volume = (int)v;
    if (g_store.volume < 0) g_store.volume = 0;
    if (g_store.volume > 255) g_store.volume = 255;
  }
  if (!g_store.device_id[0]) store_gen_device_id();
}

void store_save_volume(void) { wr_i64("vol", g_store.volume); }

void store_gen_device_id(void)
{
  random_hex(g_store.device_id, 32);
  wr_str("dev", g_store.device_id);
}

void store_save_wifi(void)
{
  wr_str("ssid", g_store.wifi_ssid);
  wr_str("pass", g_store.wifi_pass);
}
void store_save_server(void) { wr_str("srv", g_store.server); }
void store_save_auth(void)
{
  wr_str("tok", g_store.token);
  wr_str("rfr", g_store.refresh);
  wr_i64("uid", g_store.user_id);
  wr_i64("exp", g_store.expires_at_ms);
}
void store_save_bot(void)
{
  wr_i64("bid", g_store.bot_user_id);
  wr_str("bnm", g_store.bot_name);
}
void store_clear_bot(void)
{
  g_store.bot_user_id = 0;
  g_store.bot_name[0] = '\0';
  wr_i64("bid", 0);
  wr_str("bnm", "");
  ESP_LOGI(TAG, "bot cleared");
}
void store_clear_auth(void)
{
  g_store.token[0] = g_store.refresh[0] = '\0';
  g_store.user_id = 0;
  g_store.expires_at_ms = 0;
  wr_str("tok", "");
  wr_str("rfr", "");
  wr_i64("uid", 0);
  wr_i64("exp", 0);
  ESP_LOGI(TAG, "auth cleared");
}
