#pragma once
#include <Arduino.h>
#include <cstdio>
#include <functional>
#ifndef RFID_FS_ROOT
#define RFID_FS_ROOT "/littlefs"
#endif
namespace app {
bool storage_init();
bool storage_recover();
bool storage_replace_access_state(const std::function<bool(FILE*)>& users_writer,
                                  const String& settings_text);
bool storage_ready();
bool storage_format();
class StorageLock {
 public:
  StorageLock();
  ~StorageLock();
  explicit operator bool() const { return held_; }

 private:
  bool held_;
  StorageLock(const StorageLock&) = delete;
  StorageLock& operator=(const StorageLock&) = delete;
};
bool storage_atomic_write(const char* path, const std::function<bool(FILE*)>& writer);
bool storage_write_text(const char* path, const String& text);
bool storage_read_text(const char* path, String* out, size_t max_bytes);
}  // namespace app
