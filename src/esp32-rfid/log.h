#pragma once

#include <Arduino.h>

namespace app {

struct LogEntry {
  uint32_t ts_ms;
  char msg[160];
};

class LogBuffer {
 public:
  void init();
  bool load();
  bool save() const;
  void add(const char* msg, uint32_t ts_ms);
  String to_json() const;
  String to_text() const;
  bool import_text(const char* text);
  void clear_ram();
  void clear_all();

 private:
  void add_internal(const char* msg, uint32_t ts_ms, bool persist);
  static constexpr size_t kMaxLogs = 50;
  LogEntry entries_[kMaxLogs];
  size_t head_ = 0;
  size_t count_ = 0;
};

} // namespace app
