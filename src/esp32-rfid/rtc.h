#pragma once

#include <stdint.h>

namespace app {

struct RtcDateTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

enum class RtcState : uint8_t {
  Disabled,
  Unavailable,
  LostPower,
  InvalidDate,
  OscillatorDisabled,
  AdjustmentPending,
  Busy,
  Ready
};

bool rtc_init(bool enabled);
void rtc_invalidate();
bool rtc_is_enabled();
bool rtc_has_valid_time();
RtcState rtc_get_state();  // Last hardware observation; reads refresh this state.
const char* rtc_state_name(RtcState state);
bool rtc_parse_datetime(const char* text, RtcDateTime* out);
bool rtc_set_datetime(const RtcDateTime& dt);
bool rtc_get_datetime(RtcDateTime* out);

}  // namespace app
