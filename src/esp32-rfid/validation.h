#pragma once
#include <Arduino.h>
#include <cstring>
#include <stdint.h>
namespace app {
inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return uint32_t(now - since) >= interval;
}
inline bool valid_field(const char* s, size_t capacity, bool reject_separator = true) {
  if (!s || strnlen(s, capacity) >= capacity) return false;
  for (; *s; ++s)
    if (uint8_t(*s) < 32 || (reject_separator && *s == '|')) return false;
  return true;
}
inline bool normalize_uid(const char* src, char* dst, size_t capacity) {
  if (!src || !*src || strlen(src) >= capacity) return false;
  size_t i = 0;
  for (; src[i]; ++i) {
    char c = src[i];
    if (c >= 'a' && c <= 'f') c -= 32;
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    dst[i] = c;
  }
  dst[i] = 0;
  return true;
}
inline String json_quote(const char* src) {
  String out = "\"";
  if (src)
    for (; *src; ++src) {
      unsigned char c = *src;
      if (c == '"' || c == '\\') {
        out += '\\';
        out += char(c);
      } else if (c < 32) {
        char b[7];
        snprintf(b, sizeof(b), "\\u%04x", c);
        out += b;
      } else
        out += char(c);
    }
  out += '"';
  return out;
}
}  // namespace app
