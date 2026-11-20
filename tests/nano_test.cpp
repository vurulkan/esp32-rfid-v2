#include <Arduino.h>
#include <vector>
#include "freertos/task.h"
#include <cassert>
namespace host {
uint32_t now = 0, now_us = 0, stop_at = UINT32_MAX;
int pins[64]{}, modes[64]{};
int atomic_depth = 0;
bool require_atomic = false;
bool task_ok = true, stop_empty = false;
std::vector<std::pair<TaskFunction_t, void*>> tasks;
}  // namespace host
FakeSerial Serial, Serial2;
volatile uint8_t PIND = 0xFF, PCICR = 0, PCMSK2 = 0;
#include "../nano/wiegand_nano/wiegand_nano.ino"
uint64_t frame(uint32_t payload, uint8_t bits) {
  uint64_t value = uint64_t(payload) << 1;
  uint8_t half = bits / 2;
  auto parity = [](uint64_t v) {
    int n = 0;
    while (v) {
      n ^= v & 1;
      v >>= 1;
    }
    return n;
  };
  if (parity(value >> half)) value |= 1ULL << (bits - 1);
  if (!parity(value & ((1ULL << half) - 1))) value |= 1;
  return value;
}
void accepts_valid_parity_and_rejects_corruption() {
  setup();
  PIND = 0xEF;
  PCINT2_vect();
  assert(g_r2.bits == 1 && g_r2.data == 0);
  PIND = 0xFF;
  PCINT2_vect();
  PIND = 0xDF;
  PCINT2_vect();
  assert(g_r2.bits == 2 && g_r2.data == 1);
  emit_uid(1, 26, frame(0xA1B2C3, 26));
  assert(Serial.output == "1,A1B2C3\n");
  Serial.output.clear();
  emit_uid(2, 34, frame(0x12345678, 34));
  assert(Serial.output == "2,12345678\n");
  Serial.output.clear();
  emit_uid(1, 26, frame(0xA1B2C3, 26) ^ 2);
  emit_uid(1, 25, 0);
  assert(Serial.output.empty());
}
void flush_is_atomic_wrap_safe_and_discards_overflow() {
  ReaderState r{26, frame(0xA1B2C3, 26), 0xfffffff0u, false};
  host::require_atomic = true;
  host::now_us = uint32_t(r.last_us + 24999);
  assert(!try_flush_reader(1, r));
  host::now_us = uint32_t(r.last_us + 25000);
  assert(try_flush_reader(1, r));
  assert(r.bits == 0 && Serial.output == "1,A1B2C3\n");
  host::require_atomic = false;
  Serial.output.clear();
  for (int i = 0; i < 70; ++i) {
    ++host::now_us;
    handle_bit(r, 0);
  }
  assert(r.overflow && r.bits == 64);
  host::now_us += 25000;
  host::require_atomic = true;
  assert(!try_flush_reader(1, r));
  assert(!r.overflow && r.bits == 0 && Serial.output.empty());
}
void feedback_and_uart_overflow_recover() {
  FeedbackState fb{};
  host::now = 0xfffffff0u;
  feedback_start(fb, true);
  feedback_tick(6, 7, fb);
  assert(host::modes[6] == OUTPUT && host::modes[7] == OUTPUT);
  host::now = 63;
  feedback_tick(6, 7, fb);
  assert(host::modes[7] == OUTPUT);
  host::now = 64;
  feedback_tick(6, 7, fb);
  assert(host::modes[7] == INPUT);
  host::now = 284;
  feedback_tick(6, 7, fb);
  assert(!fb.active && host::modes[6] == INPUT);
  std::string input = std::string(20, 'X') + "A,1\nPING\n";
  for (char c : input) Serial.input.push_back(c);
  handle_uart();
  assert(!g_fb1.active && Serial.output == "PONG\n");
}
int main(int argc, char** argv) {
  assert(argc == 2);
  std::string name = argv[1];
#define RUN(f)      \
  if (name == #f) { \
    f();            \
    return 0;       \
  }
  RUN(accepts_valid_parity_and_rejects_corruption)
  RUN(flush_is_atomic_wrap_safe_and_discards_overflow)
  RUN(feedback_and_uart_overflow_recover) return 2;
}
