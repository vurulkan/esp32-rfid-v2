#include "log.h"

#include <cstring>
#include <LittleFS.h>

namespace {
constexpr const char* kLogsPath = "/logs.txt";
constexpr const char* kLogsTmpPath = "/logs.tmp";
constexpr size_t kMaxFileLogs = 10000;

bool ensure_fs() {
  static bool started = false;
  if (!started) {
    started = LittleFS.begin();
  }
  return started;
}

size_t count_lines(File& file) {
  size_t count = 0;
  while (file.available()) {
    char c = static_cast<char>(file.read());
    if (c == '\n') {
      count++;
    }
  }
  return count;
}

bool trim_file_if_needed() {
  if (!ensure_fs()) {
    return false;
  }
  File src = LittleFS.open(kLogsPath, FILE_READ);
  if (!src) {
    return false;
  }
  size_t total = count_lines(src);
  src.close();
  if (total <= kMaxFileLogs) {
    return true;
  }

  size_t skip = total - kMaxFileLogs;
  src = LittleFS.open(kLogsPath, FILE_READ);
  if (!src) {
    return false;
  }
  File dst = LittleFS.open(kLogsTmpPath, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (skip > 0) {
      skip--;
      continue;
    }
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    dst.println(line);
  }
  src.close();
  dst.close();
  LittleFS.remove(kLogsPath);
  LittleFS.rename(kLogsTmpPath, kLogsPath);
  return true;
}

bool append_line(const String& line) {
  if (!ensure_fs()) {
    return false;
  }
  File file = LittleFS.open(kLogsPath, FILE_APPEND);
  if (!file) {
    file = LittleFS.open(kLogsPath, FILE_WRITE);
  }
  if (!file) {
    return false;
  }
  file.println(line);
  file.close();
  return true;
}
} // namespace

namespace app {

void LogBuffer::init() {
  head_ = 0;
  count_ = 0;
  for (auto & entry : entries_) {
    entry.ts_ms = 0;
    entry.msg[0] = '\0';
  }
}

bool LogBuffer::load() {
  if (!ensure_fs()) {
    return false;
  }
  if (!LittleFS.exists(kLogsPath)) {
    return true;
  }
  File file = LittleFS.open(kLogsPath, FILE_READ);
  if (!file) {
    return false;
  }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int p1 = line.indexOf(',');
    if (p1 < 0) {
      continue;
    }
    String ts = line.substring(0, p1);
    String msg = line.substring(p1 + 1);
    add_internal(msg.c_str(), static_cast<uint32_t>(ts.toInt()), false);
  }
  file.close();
  return true;
}

bool LogBuffer::save() const {
  if (!ensure_fs()) {
    return false;
  }
  File file = LittleFS.open(kLogsPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  for (size_t i = 0; i < count_; ++i) {
    size_t idx = (head_ + i) % kMaxLogs;
    file.print(entries_[idx].ts_ms);
    file.print(',');
    file.print(entries_[idx].msg);
    file.print('\n');
  }
  file.close();
  return true;
}

static void copy_log(char* dest, size_t dest_len, const char* src) {
  if (!dest || dest_len == 0) {
    return;
  }
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, dest_len - 1);
  dest[dest_len - 1] = '\0';
}

void LogBuffer::add_internal(const char* msg, uint32_t ts_ms, bool persist) {
  size_t idx = (head_ + count_) % kMaxLogs;
  if (count_ == kMaxLogs) {
    idx = head_;
    head_ = (head_ + 1) % kMaxLogs;
  } else {
    count_++;
  }

  entries_[idx].ts_ms = ts_ms;
  copy_log(entries_[idx].msg, sizeof(entries_[idx].msg), msg);
  if (persist) {
    String line;
    line.reserve(200);
    line += ts_ms;
    line += ',';
    line += msg;
    append_line(line);
    trim_file_if_needed();
  }
}

void LogBuffer::add(const char* msg, uint32_t ts_ms) {
  add_internal(msg, ts_ms, true);
}

String LogBuffer::to_json() const {
  String json = "{\"logs\":[";
  for (size_t i = 0; i < count_; ++i) {
    size_t idx = (head_ + i) % kMaxLogs;
    if (i > 0) {
      json += ',';
    }
    json += "{\"ts\":";
    json += entries_[idx].ts_ms;
    json += ",\"msg\":\"";
    json += entries_[idx].msg;
    json += "\"}";
  }
  json += "]}";
  return json;
}

String LogBuffer::to_text() const {
  String out;
  for (size_t i = 0; i < count_; ++i) {
    size_t idx = (head_ + i) % kMaxLogs;
    out += entries_[idx].ts_ms;
    out += ',';
    out += entries_[idx].msg;
    out += '\n';
  }
  return out;
}

bool LogBuffer::import_text(const char* text) {
  if (!text) {
    return false;
  }
  clear_all();
  String data(text);
  int start = 0;
  while (start < data.length()) {
    int end = data.indexOf('\n', start);
    if (end < 0) {
      end = data.length();
    }
    String line = data.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.length() == 0) {
      continue;
    }
    int p1 = line.indexOf(',');
    if (p1 < 0) {
      continue;
    }
    String ts = line.substring(0, p1);
    String msg = line.substring(p1 + 1);
    add_internal(msg.c_str(), static_cast<uint32_t>(ts.toInt()), false);
  }
  return save();
}

void LogBuffer::clear_ram() {
  head_ = 0;
  count_ = 0;
}

void LogBuffer::clear_all() {
  clear_ram();
  if (!ensure_fs()) {
    return;
  }
  LittleFS.remove(kLogsPath);
}

} // namespace app
