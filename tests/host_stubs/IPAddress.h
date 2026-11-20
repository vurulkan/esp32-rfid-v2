#pragma once
#include <cstdio>
#include <Arduino.h>
#include <cstdint>
class IPAddress {
  uint8_t bytes[4]{};

 public:
  bool fromString(const char* s) {
    int a, b, c, d;
    char tail;
    if (sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4 || a < 0 || a > 255 || b < 0 ||
        b > 255 || c < 0 || c > 255 || d < 0 || d > 255)
      return false;
    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
    return true;
  }
  String toString() const {
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return b;
  }
  uint8_t operator[](size_t i) const { return bytes[i]; }
};
