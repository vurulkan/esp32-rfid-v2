#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
struct FakeWire {
  std::array<uint8_t, 32> registers{};
  std::vector<uint8_t> tx, rx;
  size_t read_pos = 0;
  uint8_t pointer = 0;
  bool ignore_clock_writes = false;
  bool online = true, begin_ok = true, short_read = false, slow = false;
  int failed_write_reg = -1, begins = 0, clock_writes = 0;
  uint16_t timeout = 0;
  std::atomic<bool> transaction{false};
  std::atomic<int> collisions{0};
  bool begin(uint8_t, uint8_t, uint32_t) {
    ++begins;
    return begin_ok;
  }
  void setTimeOut(uint16_t ms) { timeout = ms; }
  void beginTransmission(uint8_t) {
    if (transaction.exchange(true)) ++collisions;
    if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tx.clear();
  }
  size_t write(uint8_t b) {
    tx.push_back(b);
    return 1;
  }
  int endTransmission(bool stop = true) {
    if (!online || (tx.size() > 1 && tx[0] == failed_write_reg)) {
      transaction = false;
      return 4;
    }
    pointer = tx.at(0);
    if (tx.size() > 1) {
      if (pointer == 0) ++clock_writes;
      for (size_t i = 1; i < tx.size(); ++i) {
        if (!ignore_clock_writes || tx[0] != 0) registers.at(pointer) = tx[i];
        ++pointer;
      }
    }
    if (stop) transaction = false;
    return 0;
  }
  size_t requestFrom(uint8_t, uint8_t count) {
    rx.clear();
    read_pos = 0;
    size_t len = short_read ? count - 1 : count;
    for (size_t i = 0; i < len; ++i) rx.push_back(registers.at(pointer++));
    transaction = false;
    return len;
  }
  int read() { return read_pos < rx.size() ? rx[read_pos++] : -1; }
};
extern FakeWire Wire;
