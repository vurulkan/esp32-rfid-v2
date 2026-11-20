#include "storage.h"
#include <LittleFS.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <unistd.h>
#include <vector>
namespace app {
namespace {
SemaphoreHandle_t g_lock = nullptr;
std::atomic<bool> g_ready{false};
}  // namespace
bool storage_init() {
  if (!g_lock) g_lock = xSemaphoreCreateRecursiveMutex();
  StorageLock lock;
  if (!lock) return false;
  if (!g_ready) g_ready = LittleFS.begin(false);
  if (g_ready && !storage_recover()) g_ready = false;
  if (g_ready) {
    File root = LittleFS.open("/");
    std::vector<String> abandoned;
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (!name.startsWith("/")) name = "/" + name;
      f.close();
      if (name.startsWith("/restore-") && name.endsWith(".txt")) abandoned.push_back(name);
      f = root.openNextFile();
    }
    root.close();
    for (const auto& path : abandoned) LittleFS.remove(path.c_str());
    for (const char* path :
         {"/users.next", "/settings.next", "/users.next.tmp", "/settings.next.tmp"})
      LittleFS.remove(path);
  }
  return g_ready;
}
bool storage_ready() { return g_ready.load(); }
StorageLock::StorageLock()
    : held_(g_lock && xSemaphoreTakeRecursive(g_lock, portMAX_DELAY) == pdTRUE) {}
StorageLock::~StorageLock() {
  if (held_) xSemaphoreGiveRecursive(g_lock);
}
bool storage_format() {
  StorageLock lock;
  if (!lock) return false;
  g_ready = false;
  LittleFS.end();
  bool ok = LittleFS.format();
  g_ready = LittleFS.begin(false);
  return ok && g_ready;
}
bool storage_atomic_write(const char* path, const std::function<bool(FILE*)>& writer) {
  StorageLock lock;
  if (!lock || !g_ready || !path || path[0] != '/' || strchr(path, '\\') || strstr(path, ".."))
    return false;
  String target = RFID_FS_ROOT;
  target += path;
  String temp = target + ".tmp";
  FILE* file = fopen(temp.c_str(), "wb");
  if (!file) return false;
  bool ok = writer(file);
  if (ferror(file) || fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (ok) ok = rename(temp.c_str(), target.c_str()) == 0;
  if (!ok) remove(temp.c_str());
  return ok;
}
bool storage_write_text(const char* path, const String& text) {
  return storage_atomic_write(
      path, [&](FILE* f) { return fwrite(text.c_str(), 1, text.length(), f) == text.length(); });
}
bool storage_read_text(const char* path, String* out, size_t max_bytes) {
  StorageLock lock;
  if (!lock || !g_ready || !out) return false;
  String full = RFID_FS_ROOT;
  full += path;
  FILE* file = fopen(full.c_str(), "rb");
  if (!file) return false;
  bool ok = fseek(file, 0, SEEK_END) == 0;
  long size = ftell(file);
  if (size < 0 || size_t(size) > max_bytes || !out->reserve(size + 1)) ok = false;
  rewind(file);
  *out = "";
  char buf[256];
  size_t count;
  while (ok && (count = fread(buf, 1, sizeof(buf), file)) > 0)
    if (!out->concat(buf, count)) ok = false;
  if (ferror(file)) ok = false;
  fclose(file);
  return ok;
}

bool storage_recover() {
  StorageLock lock;
  if (!lock || !g_ready) return false;
  const char* targets[] = {"/users.txt", "/settings.txt"};
  const char* backups[] = {"/users.rollback", "/settings.rollback"};
  if (!LittleFS.exists("/restore.pending")) return true;
  String marker;
  if (!storage_read_text("/restore.pending", &marker, 64)) return false;
  int old[2];
  if (marker.length() != 5 || sscanf(marker.c_str(), "1 %d %d", &old[0], &old[1]) != 2 ||
      (old[0] != 0 && old[0] != 1) || (old[1] != 0 && old[1] != 1))
    return false;
  for (size_t i = 0; i < 2; ++i) {
    if (LittleFS.exists(backups[i])) {
      if (LittleFS.exists(targets[i]) && !LittleFS.remove(targets[i])) return false;
      if (!LittleFS.rename(backups[i], targets[i])) return false;
    } else if (!old[i] && LittleFS.exists(targets[i]) && !LittleFS.remove(targets[i]))
      return false;
  }
  return LittleFS.remove("/restore.pending");
}
bool storage_replace_access_state(const std::function<bool(FILE*)>& writer,
                                  const String& settings_text) {
  StorageLock lock;
  if (!lock || !g_ready || !settings_text.length() || !storage_recover()) return false;
  for (const char* path : {"/users.rollback", "/settings.rollback"})
    if (LittleFS.exists(path) && !LittleFS.remove(path)) return false;
  if (!storage_atomic_write("/users.next", writer) ||
      !storage_write_text("/settings.next", settings_text))
    return false;
  String marker = "1 ";
  marker += LittleFS.exists("/users.txt") ? "1" : "0";
  marker += ' ';
  marker += LittleFS.exists("/settings.txt") ? "1" : "0";
  if (!storage_write_text("/restore.pending", marker)) return false;
  bool ok = true;
  for (const char* target : {"/users.txt", "/settings.txt"}) {
    String backup = String(target) == "/users.txt" ? "/users.rollback" : "/settings.rollback";
    if (LittleFS.exists(target) && !LittleFS.rename(target, backup.c_str())) {
      ok = false;
      break;
    }
  }
  if (ok)
    ok = LittleFS.rename("/users.next", "/users.txt") &&
         LittleFS.rename("/settings.next", "/settings.txt");
  if (ok) ok = LittleFS.remove("/restore.pending");
  if (!ok) {
    if (!storage_recover()) g_ready = false;
    return false;
  }
  LittleFS.remove("/users.rollback");
  LittleFS.remove("/settings.rollback");
  return true;
}
}  // namespace app
