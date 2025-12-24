#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>

namespace app {

namespace {
constexpr uint8_t kRtcAddress = 0x68;
constexpr uint8_t kI2cSda = 21;
constexpr uint8_t kI2cScl = 22;

bool g_enabled = false;
bool g_time_valid = false;

uint8_t bcd_to_dec(uint8_t val) {
  return (val / 16 * 10) + (val % 16);
}

uint8_t dec_to_bcd(uint8_t val) {
  return (val / 10 * 16) + (val % 10);
}

bool i2c_write(uint8_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(kRtcAddress);
  Wire.write(reg);
  for (size_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

bool i2c_read(uint8_t reg, uint8_t* data, size_t len) {
  Wire.beginTransmission(kRtcAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  size_t read = Wire.requestFrom(kRtcAddress, static_cast<uint8_t>(len));
  if (read != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

} // namespace

void rtc_init(bool enabled) {
  g_enabled = enabled;
  if (!g_enabled) {
    g_time_valid = false;
  }
  if (g_enabled) {
    Wire.begin(kI2cSda, kI2cScl);
  }
}

bool rtc_is_enabled() {
  return g_enabled;
}

bool rtc_has_valid_time() {
  return g_enabled && g_time_valid;
}

void rtc_set_time_valid(bool valid) {
  g_time_valid = valid;
}

bool rtc_set_datetime(const RtcDateTime& dt) {
  if (!g_enabled) {
    return false;
  }
  uint8_t payload[7];
  payload[0] = dec_to_bcd(dt.second);
  payload[1] = dec_to_bcd(dt.minute);
  payload[2] = dec_to_bcd(dt.hour);
  payload[3] = 1; // day of week (1..7), not used
  payload[4] = dec_to_bcd(dt.day);
  payload[5] = dec_to_bcd(dt.month);
  payload[6] = dec_to_bcd(static_cast<uint8_t>(dt.year % 100));
  bool ok = i2c_write(0x00, payload, sizeof(payload));
  if (ok) {
    g_time_valid = true;
  }
  return ok;
}

bool rtc_get_datetime(RtcDateTime* out) {
  if (!g_enabled || !out || !g_time_valid) {
    return false;
  }
  uint8_t data[7];
  if (!i2c_read(0x00, data, sizeof(data))) {
    return false;
  }
  out->second = bcd_to_dec(data[0] & 0x7F);
  out->minute = bcd_to_dec(data[1] & 0x7F);
  out->hour = bcd_to_dec(data[2] & 0x3F);
  out->day = bcd_to_dec(data[4] & 0x3F);
  out->month = bcd_to_dec(data[5] & 0x1F);
  out->year = static_cast<uint16_t>(2000 + bcd_to_dec(data[6]));
  return true;
}

} // namespace app
