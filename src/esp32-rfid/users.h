#pragma once
#include <Arduino.h>
#include "settings.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
namespace app {
struct UserRecord {
  bool in_use;
  char uid[20];
  char name[32];
  bool relay1;
  bool relay2;
};
class UsersDb {
 public:
  ~UsersDb();
  UsersDb() = default;
  UsersDb(const UsersDb&) = delete;
  UsersDb& operator=(const UsersDb&) = delete;
  void init();
  bool load();
  bool restore_file(const char* path, const Settings* settings = nullptr);
  bool ready() const { return ready_; }
  size_t capacity() const { return capacity_; }
  bool add_user(const char*, const char*, bool, bool);
  bool remove(const char*);
  bool authorized(const char*, uint8_t) const;
  bool get_user(const char*, UserRecord*) const;
  String to_json_page(size_t, size_t, size_t&) const;

 private:
  bool replace_file(const char*, bool, const Settings* settings = nullptr);
  bool save_overlay(size_t, const UserRecord&) const;
  static constexpr size_t kMaxUsers = 1000;
  UserRecord* users_ = nullptr;
  size_t capacity_ = 0;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  std::atomic<bool> ready_{false};
};
UsersDb& users_database();
void users_task(void* param);
}  // namespace app
