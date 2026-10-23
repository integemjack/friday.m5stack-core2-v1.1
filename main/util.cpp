#include "util.h"
#include <string.h>
#include <stdio.h>
#include "mbedtls/base64.h"
#include "esp_random.h"
#include "esp_timer.h"

size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap)
{
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char *)out, out_cap, &olen,
                            (const unsigned char *)in, len) != 0)
    return 0;
  return olen;
}

int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap)
{
  size_t olen = 0;
  if (mbedtls_base64_decode(out, out_cap, &olen,
                            (const unsigned char *)in, in_len) != 0)
    return -1;
  return (int)olen;
}

void random_hex(char *out, int hex_chars)
{
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < hex_chars; i += 2) {
    uint8_t b = esp_random();
    out[i] = hex[b >> 4];
    out[i + 1] = hex[b & 0xf];
  }
  out[hex_chars] = '\0';
}

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
uint64_t now_ms64(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

// ---- JWT ----
// chatserver 发的是 JWT（header.payload.signature）。payload 是 base64url(JSON)。
// 把 payload 解出来（不验签，只用来看 exp/typ）
static int jwt_payload(const char *jwt, char *out, size_t cap)
{
  if (!jwt) return -1;
  const char *p = strchr(jwt, '.');
  if (!p) return -1;
  p++;
  const char *q = strchr(p, '.');
  if (!q) return -1;
  size_t len = (size_t)(q - p);
  if (len == 0 || len > 2048 || len + 4 >= cap) return -1;
  // base64url → base64，补 '=' 到 4 的倍数
  char b64[2060];
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    char c = p[i];
    b64[o++] = (c == '-') ? '+' : (c == '_') ? '/' : c;
  }
  while (o % 4) b64[o++] = '=';
  b64[o] = '\0';
  int n = b64_decode(b64, o, (uint8_t *)out, cap - 1);
  if (n <= 0) return -1;
  out[n] = '\0';
  return n;
}

static int64_t jwt_num_field(const char *json, const char *key)
{
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return 0;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  long long v = 0;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return (int64_t)(neg ? -v : v) * 1000;   // 秒 → 毫秒
}

int64_t jwt_exp_ms(const char *jwt)
{
  char buf[2048];
  if (jwt_payload(jwt, buf, sizeof(buf)) < 0) return 0;
  return jwt_num_field(buf, "exp");
}

int64_t jwt_iat_ms(const char *jwt)
{
  char buf[2048];
  if (jwt_payload(jwt, buf, sizeof(buf)) < 0) return 0;
  return jwt_num_field(buf, "iat");
}

void jwt_type(const char *jwt, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  char buf[2048];
  if (jwt_payload(jwt, buf, sizeof(buf)) < 0) return;
  const char *p = strstr(buf, "\"typ\"");
  if (!p) return;
  p = strchr(p + 5, '"');
  if (!p) return;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
  out[i] = '\0';
}
