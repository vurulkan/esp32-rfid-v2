#pragma once
#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
namespace app {
uint32_t log_dropped_writes();
uint32_t log_write_errors();
uint32_t log_pending_writes();
bool log_flush(uint32_t timeout_ms);
std::vector<String> log_files();
struct LogEntry {
  uint32_t ts_ms;
  char msg[160];
};
class LogBuffer {
 public:
  void init();
  bool load();
  bool start_writer();
  void add(const char*, uint32_t);
  String to_json() const;
  void clear_ram();
  bool clear_all();

 private:
  static void writer_task(void*);
  void add_ram(const char*, uint32_t);
  QueueHandle_t write_queue_ = nullptr;
  LogEntry entries_[50];
  size_t head_ = 0, count_ = 0;
};
}  // namespace app
