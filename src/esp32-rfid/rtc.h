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

void rtc_init(bool enabled);
bool rtc_is_enabled();
bool rtc_has_valid_time();
void rtc_set_time_valid(bool valid);
bool rtc_set_datetime(const RtcDateTime& dt);
bool rtc_get_datetime(RtcDateTime* out);

} // namespace app
