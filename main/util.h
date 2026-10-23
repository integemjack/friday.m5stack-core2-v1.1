// util.h — 小工具
#pragma once
#include <stdint.h>
#include <stddef.h>

// base64（mbedtls 封装）
size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap);
int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap);

// 小写随机 hex（uuid 替代：chatserver 不校验格式，requestId 只要唯一字符串）
void random_hex(char *out, int hex_chars);

// 当前毫秒
uint32_t now_ms(void);
uint64_t now_ms64(void);

// JWT 字段（chatserver 发的是 JWT：本地按 exp 算真实过期时间，别再猜 TTL）
// 返回 exp 的毫秒时间戳（本机 epoch ms，与 now_ms64 同源）；解析失败返回 0
int64_t jwt_exp_ms(const char *jwt);
// 取 payload 里的 "iat"（签发时间，毫秒）；没有返回 0
int64_t jwt_iat_ms(const char *jwt);
// 取 payload 里的 "typ"（access / legacy / refresh …）；没有返回空串
void jwt_type(const char *jwt, char *out, size_t cap);
