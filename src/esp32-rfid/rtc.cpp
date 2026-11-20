#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace app {
namespace {
constexpr uint8_t kRtcAddress = 0x68;
constexpr uint8_t kI2cSda = 21;
constexpr uint8_t kI2cScl = 22;
constexpr uint8_t kControlReg = 0x0E;
constexpr uint8_t kStatusReg = 0x0F;
constexpr uint8_t kOscillatorBit = 0x80;  // EOSC in control, OSF in status.

std::atomic<bool> g_enabled{false};
std::atomic<RtcState> g_state{RtcState::Disabled};
SemaphoreHandle_t g_mutex = nullptr;
bool g_bus_started = false;
std::atomic<bool> g_adjust_failed{false};

class RtcLock {
 public:
  RtcLock() : held_(g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {}
  ~RtcLock() {
    if (held_) xSemaphoreGive(g_mutex);
  }
  explicit operator bool() const { return held_; }

 private:
  bool held_;
};

bool valid_date(const RtcDateTime& dt) {
  if (dt.year < 2000 || dt.year > 2099 || dt.month < 1 || dt.month > 12 || dt.hour > 23 ||
      dt.minute > 59 || dt.second > 59 || dt.day < 1)
    return false;
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t maximum = days[dt.month - 1];
  if (dt.month == 2 && dt.year % 4 == 0) ++maximum;
  return dt.day <= maximum;
}

bool decode_bcd(uint8_t raw, uint8_t* value) {
  if ((raw & 0x0F) > 9 || (raw >> 4) > 9) return false;
  *value = (raw >> 4) * 10 + (raw & 0x0F);
  return true;
}
uint8_t encode_bcd(uint8_t value) { return (value / 10) * 16 + value % 10; }

bool i2c_write(uint8_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(kRtcAddress);
  Wire.write(reg);
  size_t written = 0;
  for (size_t i = 0; i < len; ++i) written += Wire.write(data[i]);
  const bool ok = Wire.endTransmission() == 0 && written == len;
  if (!ok) g_state = RtcState::Unavailable;
  return ok;
}

bool i2c_read(uint8_t reg, uint8_t* data, size_t len) {
  Wire.beginTransmission(kRtcAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(kRtcAddress, static_cast<uint8_t>(len)) != len) {
    g_state = RtcState::Unavailable;
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    int value = Wire.read();
    if (value < 0) {
      g_state = RtcState::Unavailable;
      return false;
    }
    data[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool ensure_bus() {
  if (!g_bus_started) {
    g_bus_started = Wire.begin(kI2cSda, kI2cScl, 100000);
    Wire.setTimeOut(50);
  }
  if (!g_bus_started) g_state = RtcState::Unavailable;
  return g_bus_started;
}

bool read_datetime(RtcDateTime* out) {
  uint8_t flags[2];
  if (!i2c_read(kControlReg, flags, sizeof(flags))) return false;
  if (flags[1] & kOscillatorBit) {
    g_state = RtcState::LostPower;
    return false;
  }
  if (flags[0] & kOscillatorBit) {
    g_state = RtcState::OscillatorDisabled;
    return false;
  }
  uint8_t data[7];
  if (!i2c_read(0x00, data, sizeof(data))) return false;
  RtcDateTime dt{};
  uint8_t year;
  // This application supports 2000..2099. Never silently wrap a century to 2000.
  if ((data[0] & 0x80) || (data[1] & 0x80) || (data[2] & 0x80) || (data[4] & 0xC0) ||
      (data[5] & 0xE0) || !decode_bcd(data[0], &dt.second) || !decode_bcd(data[1], &dt.minute) ||
      !decode_bcd(data[4], &dt.day) || !decode_bcd(data[5], &dt.month) ||
      !decode_bcd(data[6], &year)) {
    g_state = RtcState::InvalidDate;
    return false;
  }
  dt.year = 2000 + year;
  if (data[2] & 0x40) {
    uint8_t hour;
    if (!decode_bcd(data[2] & 0x1F, &hour) || hour < 1 || hour > 12) {
      g_state = RtcState::InvalidDate;
      return false;
    }
    dt.hour = (hour % 12) + ((data[2] & 0x20) ? 12 : 0);
  } else if (!decode_bcd(data[2], &dt.hour)) {
    g_state = RtcState::InvalidDate;
    return false;
  }
  if (!valid_date(dt)) {
    g_state = RtcState::InvalidDate;
    return false;
  }
  *out = dt;
  g_state = RtcState::Ready;
  return true;
}
}  // namespace

bool rtc_init(bool enabled) {
  // app_setup calls this before any worker tasks; subsequent reconfiguration
  // reuses the same lock and I2C bus rather than reinitializing a live transfer.
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  // Remember the requested mode even when a concurrent transfer holds the bus.
  // A later read can retry; a disabled feature must not remain enabled.
  g_enabled = enabled;
  RtcLock lock;
  if (!lock) {
    g_state = RtcState::Unavailable;
    return false;
  }
  if (!enabled) {
    g_state = RtcState::Disabled;
    return true;
  }
  if (g_adjust_failed) {
    g_state = RtcState::AdjustmentPending;
    return false;
  }
  if (!ensure_bus()) return false;
  uint8_t control;
  if (!i2c_read(kControlReg, &control, 1)) return false;
  if (control & kOscillatorBit) {
    control &= ~kOscillatorBit;
    // Enable battery-backed oscillation. Do not clear OSF or overwrite time.
    if (!i2c_write(kControlReg, &control, 1)) return false;
  }
  RtcDateTime dt{};
  return read_datetime(&dt);
}

void rtc_invalidate() {
  g_adjust_failed = true;
  g_state = RtcState::AdjustmentPending;
}
bool rtc_is_enabled() { return g_enabled.load(); }
bool rtc_has_valid_time() {
  return g_enabled.load() && !g_adjust_failed.load() && g_state.load() == RtcState::Ready;
}
RtcState rtc_get_state() { return g_state.load(); }
const char* rtc_state_name(RtcState state) {
  switch (state) {
    case RtcState::Disabled:
      return "disabled";
    case RtcState::LostPower:
      return "lost_power";
    case RtcState::InvalidDate:
      return "invalid_datetime";
    case RtcState::OscillatorDisabled:
      return "oscillator_disabled";
    case RtcState::AdjustmentPending:
      return "adjustment_pending";
    case RtcState::Busy:
      return "busy";
    case RtcState::Ready:
      return "ready";
    default:
      return "unavailable";
  }
}

bool rtc_parse_datetime(const char* text, RtcDateTime* out) {
  if (!text || !out) return false;
  const size_t len = strlen(text);
  if ((len != 16 && len != 19) || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != ' ') || text[13] != ':' || (len == 19 && text[16] != ':'))
    return false;
  for (size_t i = 0; i < len; ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
    if (text[i] < '0' || text[i] > '9') return false;
  }
  auto pair = [text](size_t pos) { return (text[pos] - '0') * 10 + text[pos + 1] - '0'; };
  RtcDateTime dt{};
  dt.year = pair(0) * 100 + pair(2);
  dt.month = pair(5);
  dt.day = pair(8);
  dt.hour = pair(11);
  dt.minute = pair(14);
  dt.second = len == 19 ? pair(17) : 0;
  if (!valid_date(dt)) return false;
  *out = dt;
  return true;
}

bool rtc_set_datetime(const RtcDateTime& dt) {
  if (!valid_date(dt)) return false;
  RtcLock lock;
  if (!lock) {
    g_state = RtcState::Busy;
    return false;
  }
  if (!g_enabled || !ensure_bus()) return false;
  uint8_t flags[2];
  if (!i2c_read(kControlReg, flags, sizeof(flags))) return false;
  uint8_t control = flags[0] & ~kOscillatorBit;
  if (!i2c_write(kControlReg, &control, 1)) return false;
  uint8_t payload[7] = {encode_bcd(dt.second), encode_bcd(dt.minute), encode_bcd(dt.hour),      1,
                        encode_bcd(dt.day),    encode_bcd(dt.month),  encode_bcd(dt.year % 100)};
  // A partially written clock must not be reported as valid.
  g_state = RtcState::Unavailable;
  g_adjust_failed = true;
  if (!i2c_write(0x00, payload, sizeof(payload))) return false;
  // Only an explicit successful time adjustment acknowledges oscillator loss.
  uint8_t status;
  if (!i2c_read(kStatusReg, &status, 1)) return false;
  status &= ~kOscillatorBit;
  if (!i2c_write(kStatusReg, &status, 1)) return false;
  RtcDateTime verified{};
  if (!read_datetime(&verified)) return false;
  auto seconds = [](const RtcDateTime& value) {
    uint32_t days = 0;
    for (uint16_t year = 2000; year < value.year; ++year) days += year % 4 == 0 ? 366 : 365;
    static const uint8_t months[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (uint8_t m = 1; m < value.month; ++m)
      days += months[m - 1] + (m == 2 && value.year % 4 == 0);
    return ((days + value.day - 1) * 24 + value.hour) * 3600 + value.minute * 60 + value.second;
  };
  uint32_t wanted = seconds(dt), actual = seconds(verified);
  if (actual < wanted || actual - wanted > 1) {
    g_state = RtcState::InvalidDate;
    return false;
  }
  g_adjust_failed = false;
  return true;
}

bool rtc_get_datetime(RtcDateTime* out) {
  if (!out) return false;
  RtcLock lock;
  if (!lock) {
    g_state = RtcState::Busy;
    return false;
  }
  if (!g_enabled) return false;
  if (g_adjust_failed) {
    g_state = RtcState::AdjustmentPending;
    return false;
  }
  if (!ensure_bus()) return false;
  // Do not gate retries on a flash setting or a previous failed I2C read.
  return read_datetime(out);
}

}  // namespace app
