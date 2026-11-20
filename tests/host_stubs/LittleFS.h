#pragma once
#include <Arduino.h>
#include <filesystem>
#include <memory>
#include <vector>
#include "freertos/task.h"
#ifndef RFID_FS_ROOT
#error host test requires RFID_FS_ROOT
#endif
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
class File {
  std::shared_ptr<FILE> handle;
  std::string name_;
  std::vector<std::string> directory;
  size_t index = 0;

 public:
  File() = default;
  explicit File(std::string path, const char* mode = "r") : name_(path) {
    auto full = std::string(RFID_FS_ROOT) + path;
    if (path == "/") {
      for (const auto& e : std::filesystem::directory_iterator(full))
        if (e.is_regular_file()) directory.push_back("/" + e.path().filename().string());
    } else {
      FILE* f = fopen(full.c_str(), mode);
      if (f) handle = std::shared_ptr<FILE>(f, [](FILE* p) { fclose(p); });
    }
  }
  explicit operator bool() const { return handle || name_ == "/"; }
  const char* name() const { return name_.c_str(); }
  size_t size() const {
    if (!handle) return 0;
    return std::filesystem::file_size(std::string(RFID_FS_ROOT) + name_);
  }
  size_t readBytes(char* out, size_t n) { return fread(out, 1, n, handle.get()); }
  void close() {
    handle.reset();
    name_ = "";
  }
  File openNextFile() { return index < directory.size() ? File(directory[index++]) : File(); }
};
struct FakeFS {
  size_t capacity = 0x160000;
  bool mount_ok = true;
  unsigned begins = 0;
  bool begin(bool = false) {
    ++begins;
    if (mount_ok) std::filesystem::create_directories(RFID_FS_ROOT);
    return mount_ok;
  }
  void end() {}
  bool format() {
    for (const auto& p : std::filesystem::directory_iterator(RFID_FS_ROOT))
      std::filesystem::remove(p.path());
    return true;
  }
  bool exists(const char* p) { return std::filesystem::exists(std::string(RFID_FS_ROOT) + p); }
  bool rename(const char* a, const char* b) {
    return ::rename((std::string(RFID_FS_ROOT) + a).c_str(),
                    (std::string(RFID_FS_ROOT) + b).c_str()) == 0;
  }
  bool remove(const char* a) { return ::remove((std::string(RFID_FS_ROOT) + a).c_str()) == 0; }
  File open(const char* p, const char* mode = "r") { return File(p, mode); }
  size_t totalBytes() { return capacity; }
  size_t usedBytes() {
    size_t n = 0;
    for (const auto& p : std::filesystem::directory_iterator(RFID_FS_ROOT))
      if (p.is_regular_file()) n += p.file_size();
    return n;
  }
};
extern FakeFS LittleFS;
