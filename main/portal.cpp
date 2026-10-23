#include "portal.h"
#include "store.h"
#include "app.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "portal";
static bool g_got;

static const char PAGE[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Friday 语音终端</title><style>"
"body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 12px;background:#111;color:#eee}"
"input{width:100%%;box-sizing:border-box;margin:6px 0 14px;padding:10px;font-size:16px;border-radius:8px;border:1px solid #555;background:#222;color:#eee}"
"button{width:100%%;padding:14px;font-size:17px;border-radius:10px;border:0;background:#3a7d3c;color:#fff}"
"h2{font-size:19px}"
"</style></head><body><h2>Friday 语音终端 · 配置</h2>"
"<form method=post action=/save>"
"<label>WiFi 名称</label><input name=ssid required>"
"<label>WiFi 密码</label><input name=pass>"
"<label>chatserver 地址（默认已填，一般不用改）</label>"
"<input name=server value='%s'>"
"<button type=submit>保存并重启</button></form></body></html>";

static const char SAVED[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"</head><body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding-top:60px'>"
"<h2>已保存，设备正在重启</h2></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
  char buf[1400];
  int n = snprintf(buf, sizeof(buf), PAGE, g_store.server);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, buf, n > 0 ? n : 0);
  return ESP_OK;
}

// URL 解码（%xx、+）
static void urldecode(char *s)
{
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '+') *o++ = ' ';
    else if (*p == '%' && p[1] && p[2]) {
      int hi = p[1], lo = p[2];
      hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
      lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
      *o++ = (char)((hi << 4) | lo);
      p += 2;
    } else *o++ = *p;
  }
  *o = '\0';
}

static bool field(const char *body, const char *name, char *out, size_t cap)
{
  char pat[24];
  snprintf(pat, sizeof(pat), "%s=", name);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat);
  const char *e = strchr(p, '&');
  size_t len = e ? (size_t)(e - p) : strlen(p);
  if (len >= cap) len = cap - 1;
  memcpy(out, p, len);
  out[len] = '\0';
  urldecode(out);
  return out[0] != '\0';
}

static esp_err_t page_post(httpd_req_t *req)
{
  char body[512];
  size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
  int got = httpd_req_recv(req, body, total);
  if (got <= 0) return ESP_FAIL;
  body[got] = '\0';
  char ssid[33] = "", pass[65] = "", server[128] = "";
  bool ok = false;
  if (field(body, "ssid", ssid, sizeof(ssid))) {
    strlcpy(g_store.wifi_ssid, ssid, sizeof(g_store.wifi_ssid));
    field(body, "pass", pass, sizeof(pass));
    strlcpy(g_store.wifi_pass, pass, sizeof(g_store.wifi_pass));
    if (field(body, "server", server, sizeof(server)))
      strlcpy(g_store.server, server, sizeof(g_store.server));
    store_save_wifi();
    store_save_server();
    ok = true;
    g_got = true;
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, SAVED, strlen(SAVED));
  if (ok) {
    ESP_LOGI(TAG, "配置已保存，1 秒后重启");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
  return ESP_OK;
}

static httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = page_get };
static httpd_uri_t uri_post = { .uri = "/save", .method = HTTP_POST, .handler = page_post };

bool portal_run(uint32_t timeout_ms)
{
  g_got = false;
  ESP_ERROR_CHECK(esp_wifi_stop());   // app_main 里已 esp_wifi_init，这里只切模式
  // app_main 只建了 STA netif；AP 模式必须有 AP netif（DHCP、默认 IP 都挂在它上）
  if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))
    esp_netif_create_default_wifi_ap();
  wifi_config_t ap = {};
  strcpy((char *)ap.ap.ssid, "Friday-Voice");
  ap.ap.ssid_len = strlen("Friday-Voice");
  strcpy((char *)ap.ap.password, "12345678");
  ap.ap.max_connection = 2;
  ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());

  httpd_handle_t server = NULL;
  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.stack_size = 6144;
  if (httpd_start(&server, &hc) == ESP_OK) {
    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_post);
  }

  // 默认 AP netif 的固定 IP（esp_netif_create_default_wifi_ap 就是 192.168.4.1）
  app_set_status("配网 AP：Friday-Voice 密码 12345678 · http://192.168.4.1");

  uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
  while (!g_got && xTaskGetTickCount() * portTICK_PERIOD_MS - start < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (server) httpd_stop(server);
  esp_wifi_stop();
  return g_got;
}
