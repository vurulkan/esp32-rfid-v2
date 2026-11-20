#include "log.h"
#include "storage.h"
#include "validation.h"
#include "app_context.h"
#include <LittleFS.h>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <freertos/semphr.h>
namespace app {
namespace {
constexpr size_t kSegmentBytes = 16384, kRecentBudget = 512 * 1024, kReserve = 128 * 1024;
constexpr const char* kArchive = "/logs-legacy.txt";
struct Command {
  enum Type : uint8_t { Append, Clear, Flush } type;
  LogEntry entry;
  uint32_t ticket;
};
std::atomic<uint32_t> dropped{0}, errors{0}, acked{0}, next_ticket{0};
QueueHandle_t queue = nullptr;
SemaphoreHandle_t control = nullptr;
struct Segment {
  String path;
  size_t bytes;
  size_t count;
};
std::vector<Segment> segments;
size_t current_bytes = 0, current_count = 0;
uint32_t sequence = 0;
String current_path = "/logs.txt";
String full_path(const String& path) { return String(RFID_FS_ROOT) + path; }
bool chunk_name(const String& n) {
  if (n.length() != 19 || !n.startsWith("/log-") || !n.endsWith(".txt")) return false;
  for (size_t i = 5; i < 15; ++i)
    if (n[i] < '0' || n[i] > '9') return false;
  return true;
}
size_t count_records(const String& path) {
  FILE* f = fopen(full_path(path).c_str(), "rb");
  if (!f) return 0;
  size_t count = 0;
  char line[192];
  while (fgets(line, sizeof(line), f))
    if (strchr(line, ',')) ++count;
  fclose(f);
  return count;
}
bool erase(const String& path) {
  return !LittleFS.exists(path.c_str()) || LittleFS.remove(path.c_str());
}
bool make_room(size_t bytes) {
  size_t recent = current_bytes, count = current_count;
  for (const auto& s : segments) {
    recent += s.bytes;
    count += s.count;
  }
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  while (!segments.empty() &&
         (recent + bytes > kRecentBudget || count >= 10000 || total < used + bytes + kReserve)) {
    Segment old = segments.front();
    if (!erase(old.path)) return false;
    recent -= old.bytes;
    count -= old.count;
    segments.erase(segments.begin());
    used = LittleFS.usedBytes();
  }
  // Preserve the legacy archive rather than silently deleting existing history.
  return total >= used + bytes + kReserve && recent + bytes <= kRecentBudget;
}
bool append(const Command& cmd) {
  StorageLock lock;
  if (!lock || !storage_ready()) return false;
  if (current_bytes >= kSegmentBytes) {
    char name[32];
    snprintf(name, sizeof(name), "/log-%010lu.txt", (unsigned long)++sequence);
    if (!LittleFS.rename(current_path.c_str(), name)) return false;
    segments.push_back({String(name), current_bytes, current_count});
    current_bytes = 0;
    current_count = 0;
  }
  String line = String(cmd.entry.ts_ms) + "," + cmd.entry.msg + "\n";
  if (!make_room(line.length())) return false;
  FILE* f = fopen(full_path(current_path).c_str(), "ab");
  if (!f) return false;
  bool ok = fwrite(line.c_str(), 1, line.length(), f) == line.length();
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (ok) {
    current_bytes += line.length();
    ++current_count;
  }
  return ok;
}
bool clear_files() {
  StorageLock lock;
  if (!lock || !storage_ready()) return false;
  bool ok = erase(kArchive) && erase(current_path) && erase("/logs.tmp");
  for (const auto& s : segments)
    if (!erase(s.path)) ok = false;
  if (ok) {
    segments.clear();
    current_bytes = 0;
    current_count = 0;
  }
  return ok;
}
}  // namespace
uint32_t log_dropped_writes() { return dropped; }
uint32_t log_write_errors() { return errors; }
uint32_t log_pending_writes() { return queue ? uxQueueMessagesWaiting(queue) : 0; }
std::vector<String> log_files() {
  StorageLock lock;
  std::vector<String> files;
  if (!lock || !storage_ready()) return files;
  if (LittleFS.exists(kArchive)) files.push_back(kArchive);
  for (const auto& s : segments) files.push_back(s.path);
  if (LittleFS.exists(current_path.c_str())) files.push_back(current_path);
  return files;
}
void LogBuffer::init() { head_ = count_ = 0; }
bool LogBuffer::load() {
  StorageLock lock;
  if (!lock || !storage_ready()) return false;
  if (!LittleFS.exists("/logs-v2")) {
    if (LittleFS.exists("/logs.txt") && !LittleFS.exists(kArchive) &&
        !LittleFS.rename("/logs.txt", kArchive))
      return false;
    if (!storage_write_text("/logs-v2", "2\n")) return false;
  }
  erase("/logs.tmp");  // Only an uncommitted copy from the previous compaction scheme.
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String n = file.name();
    if (!n.startsWith("/")) n = "/" + n;
    if (chunk_name(n)) {
      segments.push_back({n, file.size(), 0});
      sequence = std::max(sequence, uint32_t(strtoul(n.substring(5, 15).c_str(), nullptr, 10)));
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  std::sort(segments.begin(), segments.end(),
            [](const Segment& a, const Segment& b) { return a.path < b.path; });
  for (auto& s : segments) s.count = count_records(s.path);
  File current = LittleFS.open("/logs.txt", FILE_READ);
  if (current) {
    current_bytes = current.size();
    current.close();
    current_count = count_records(current_path);
  }
  for (const auto& path : log_files()) {
    FILE* f = fopen(full_path(path).c_str(), "rb");
    if (!f) return false;
    char line[192];
    size_t lines = 0;
    while (fgets(line, sizeof(line), f)) {
      char* split = strchr(line, ',');
      if (!split) continue;
      *split++ = 0;
      char* end;
      unsigned long ts = strtoul(line, &end, 10);
      if (*end) continue;
      size_t n = strlen(split);
      while (n && (split[n - 1] == '\n' || split[n - 1] == '\r')) split[--n] = 0;
      add_ram(split, ts);
      if (++lines % 32 == 0) vTaskDelay(1);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
  }
  return true;
}
bool LogBuffer::start_writer() {
  control = xSemaphoreCreateMutex();
  write_queue_ = xQueueCreate(32, sizeof(Command));
  queue = write_queue_;
  if (!control || !queue ||
      xTaskCreatePinnedToCore(writer_task, "log_writer", 4096, this, 1, nullptr, 1) != pdPASS) {
    ++errors;
    queue = nullptr;
    return false;
  }
  return true;
}
void LogBuffer::writer_task(void* param) {
  auto* self = static_cast<LogBuffer*>(param);
  Command cmd{};
  for (;;)
    if (xQueueReceive(self->write_queue_, &cmd, portMAX_DELAY) == pdTRUE) {
      if (cmd.type == Command::Flush) {
        acked = cmd.ticket;
        continue;
      }
      bool ok = cmd.type == Command::Clear ? clear_files() : append(cmd);
      if (!ok) ++errors;
    }
}
bool log_flush(uint32_t timeout) {
  if (!queue) return true;
  if (!control) return false;
  uint32_t start = millis();
  if (xSemaphoreTake(control, pdMS_TO_TICKS(timeout)) != pdTRUE) return false;
  Command cmd{};
  cmd.type = Command::Flush;
  cmd.ticket = ++next_ticket;
  uint32_t spent = millis() - start;
  bool ok = spent < timeout && xQueueSend(queue, &cmd, pdMS_TO_TICKS(timeout - spent)) == pdTRUE;
  while (ok && acked.load() != cmd.ticket) {
    if (millis() - start >= timeout) {
      ok = false;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  xSemaphoreGive(control);
  return ok;
}
void LogBuffer::add_ram(const char* msg, uint32_t ts) {
  size_t i = (head_ + count_) % 50;
  if (count_ == 50) {
    i = head_;
    head_ = (head_ + 1) % 50;
  } else
    ++count_;
  entries_[i].ts_ms = ts;
  strncpy(entries_[i].msg, msg, sizeof(entries_[i].msg) - 1);
  entries_[i].msg[159] = 0;
}
void LogBuffer::add(const char* msg, uint32_t ts) {
  add_ram(msg, ts);
  Command cmd{};
  cmd.type = Command::Append;
  cmd.entry = entries_[(head_ + count_ - 1) % 50];
  if (!queue || xQueueSend(queue, &cmd, 0) != pdTRUE) ++dropped;
}
String LogBuffer::to_json() const {
  String json = "{\"logs\":[";
  for (size_t i = 0; i < count_; ++i) {
    size_t n = (head_ + i) % 50;
    String item = "{\"ts\":";
    item += entries_[n].ts_ms;
    item += ",\"msg\":";
    item += json_quote(entries_[n].msg);
    item += '}';
    if (json.length() + item.length() + 4 > 6144) break;
    if (i) json += ',';
    json += item;
  }
  json += "]}";
  return json;
}
void LogBuffer::clear_ram() { head_ = count_ = 0; }
bool LogBuffer::clear_all() {
  Command cmd{};
  cmd.type = Command::Clear;
  if (!queue || xQueueSend(queue, &cmd, 0) != pdTRUE) return false;
  clear_ram();
  return true;
}
}  // namespace app
