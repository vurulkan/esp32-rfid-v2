#include "rtc.h"
#include <Wire.h>
#include <cassert>
#include <cstring>
#include <thread>
namespace rtc_test {
bool lock_fail = false;
}
FakeWire Wire;
using namespace app;

const RtcDateTime expected{2026, 11, 19, 14, 35, 42};
void seed() { Wire.registers = {{0x42, 0x35, 0x14, 0x05, 0x19, 0x11, 0x26}}; }
void expect_time(const RtcDateTime& value, const RtcDateTime& wanted = expected) {
  assert(value.year == wanted.year && value.month == wanted.month && value.day == wanted.day);
  assert(value.hour == wanted.hour && value.minute == wanted.minute &&
         value.second == wanted.second);
}
void preserves_time_across_restart_and_toggle() {
  seed();
  rtc_init(true);
  RtcDateTime dt{};
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
  rtc_init(false);
  assert(!rtc_get_datetime(&dt));
  assert(rtc_get_state() == RtcState::Disabled);
  rtc_init(true);
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
  rtc_init(true);
  assert(Wire.begins == 1);
  assert(Wire.clock_writes == 0);
  assert(Wire.timeout == 50);
}
void rejects_lost_power_until_adjusted() {
  seed();
  Wire.registers[0x0F] = 0x8B;
  rtc_init(true);
  RtcDateTime dt{};
  assert(!rtc_get_datetime(&dt));
  assert(rtc_get_state() == RtcState::LostPower);
  assert(Wire.registers[0x0F] == 0x8B);
  assert(Wire.clock_writes == 0);
  rtc_init(true);
  assert(!rtc_get_datetime(&dt));
  assert(rtc_set_datetime(expected));
  assert(Wire.registers[0x0F] == 0x0B);
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
}
void recovers_from_bus_failures() {
  seed();
  Wire.begin_ok = false;
  rtc_init(true);
  assert(rtc_get_state() == RtcState::Unavailable);
  Wire.begin_ok = true;
  RtcDateTime dt{};
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
  Wire.online = false;
  assert(!rtc_get_datetime(&dt));
  assert(rtc_get_state() == RtcState::Unavailable);
  Wire.online = true;
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
  Wire.short_read = true;
  dt.year = 1234;
  assert(!rtc_get_datetime(&dt));
  assert(dt.year == 1234);
  Wire.short_read = false;
  assert(rtc_get_datetime(&dt));
  rtc_test::lock_fail = true;
  assert(!rtc_get_datetime(&dt));
  assert(rtc_get_state() == RtcState::Busy);
  rtc_test::lock_fail = false;
  assert(rtc_get_datetime(&dt));
}
void decodes_clock_modes_and_rejects_bad_registers() {
  seed();
  rtc_init(true);
  RtcDateTime dt{};
  for (const auto& pair : {std::pair<int, int>{0x52, 0}, {0x72, 12}, {0x61, 13}, {0x23, 23}}) {
    Wire.registers[2] = pair.first;
    assert(rtc_get_datetime(&dt));
    assert(dt.hour == pair.second);
  }
  Wire.registers[2] = 0x14;
  Wire.registers[0] = 0x5A;
  assert(!rtc_get_datetime(&dt));
  assert(rtc_get_state() == RtcState::InvalidDate);
  seed();
  Wire.registers[5] = 0x91;
  assert(!rtc_get_datetime(&dt));  // Century cannot wrap silently.
  seed();
  Wire.registers[4] = 0x31;
  assert(!rtc_get_datetime(&dt));  // November 31.
  seed();
  Wire.registers[2] = 0x40;
  assert(!rtc_get_datetime(&dt));  // 12-hour mode, hour zero.
  seed();
  Wire.registers[0] = 0x80;
  assert(!rtc_get_datetime(&dt));
}
void validates_calendar_and_api_input() {
  RtcDateTime dt{};
  assert(rtc_parse_datetime("2026-11-19T14:35:42", &dt));
  expect_time(dt);
  assert(rtc_parse_datetime("2026-11-19 14:35", &dt));
  assert(dt.second == 0);
  assert(rtc_parse_datetime("2024-02-29T23:59:59", &dt));
  for (const char* text :
       {"2026-02-29T14:35", "2026-04-31T14:35", "1999-12-31T23:59", "2100-01-01T00:00",
        "2026-00-01T00:00", "2026-01-00T00:00", "2026-11-19T24:00", "2026-11-19T14:60",
        "2026-11-19T14:35:60", "2026/11/19T14:35", "2026-11-19T1x:35", "2026-11-19T14:35:42junk",
        "", "2026-11-19T14:35:", "2026-11-19T14:35Z"}) {
    assert(!rtc_parse_datetime(text, &dt));
  }
  assert(!rtc_parse_datetime(nullptr, &dt));
  assert(!rtc_parse_datetime("2026-11-19T14:35", nullptr));
  seed();
  rtc_init(true);
  for (const RtcDateTime& bad :
       {RtcDateTime{2026, 2, 29, 0, 0, 0}, {2026, 1, 1, 0, 0, 60}, {2100, 1, 1, 0, 0, 0}}) {
    assert(!rtc_set_datetime(bad));
  }
  assert(Wire.clock_writes == 0);
}
void enables_backup_without_erasing_loss_evidence() {
  seed();
  Wire.registers[0x0E] = 0x9D;
  Wire.registers[0x0F] = 0x80;
  rtc_init(true);
  assert(Wire.registers[0x0E] == 0x1D);
  assert(Wire.registers[0x0F] == 0x80);
  assert(Wire.clock_writes == 0);
  assert(rtc_get_state() == RtcState::LostPower);
}
void failed_adjustment_preserves_loss_flag() {
  seed();
  Wire.registers[0x0F] = 0x80;
  rtc_init(true);
  Wire.failed_write_reg = 0;
  assert(!rtc_set_datetime(expected));
  assert(Wire.registers[0x0F] == 0x80);
  assert(!rtc_has_valid_time());
  Wire.failed_write_reg = 0x0F;
  assert(!rtc_set_datetime(expected));
  assert(Wire.registers[0x0F] == 0x80);
  Wire.failed_write_reg = -1;
  assert(rtc_set_datetime(expected));
  assert(rtc_has_valid_time());
}
void serializes_concurrent_clock_access() {
  seed();
  rtc_init(true);
  Wire.slow = true;
  std::thread reader([] {
    for (int i = 0; i < 10; ++i) {
      RtcDateTime dt{};
      rtc_get_datetime(&dt);
    }
  });
  std::thread writer([] {
    for (int i = 0; i < 10; ++i) rtc_set_datetime(expected);
  });
  reader.join();
  writer.join();
  assert(Wire.collisions == 0);
  RtcDateTime dt{};
  assert(rtc_get_datetime(&dt));
  expect_time(dt);
}
void rejects_readback_mismatch() {
  seed();
  rtc_init(true);
  RtcDateTime wanted = expected;
  wanted.year = 2027;
  Wire.ignore_clock_writes = true;
  assert(!rtc_set_datetime(wanted));
  RtcDateTime dt{};
  assert(!rtc_get_datetime(&dt));
  Wire.ignore_clock_writes = false;
  assert(rtc_set_datetime(wanted));
  assert(rtc_get_datetime(&dt));
  expect_time(dt, wanted);
}
void failed_reconfigure_preserves_requested_mode_and_latch() {
  seed();
  assert(rtc_init(true));
  rtc_test::lock_fail = true;
  assert(!rtc_init(false));
  assert(!rtc_is_enabled() && !rtc_has_valid_time());
  rtc_test::lock_fail = false;
  assert(rtc_init(true));
  rtc_invalidate();
  assert(!rtc_init(true));
  assert(!rtc_has_valid_time());
  assert(rtc_get_state() == RtcState::AdjustmentPending);
  assert(rtc_set_datetime(expected));
  assert(rtc_has_valid_time());
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const char* name = argv[1];
#define RUN(test)                      \
  if (std::strcmp(name, #test) == 0) { \
    test();                            \
    return 0;                          \
  }
  RUN(rejects_readback_mismatch)
  RUN(failed_reconfigure_preserves_requested_mode_and_latch)
  RUN(preserves_time_across_restart_and_toggle)
  RUN(rejects_lost_power_until_adjusted)
  RUN(recovers_from_bus_failures)
  RUN(decodes_clock_modes_and_rejects_bad_registers)
  RUN(validates_calendar_and_api_input)
  RUN(enables_backup_without_erasing_loss_evidence)
  RUN(failed_adjustment_preserves_loss_flag)
  RUN(serializes_concurrent_clock_access)
  return 2;
}
