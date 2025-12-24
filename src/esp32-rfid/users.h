#pragma once

#include <Arduino.h>

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
  void init();
  bool load();
  bool save() const;
  bool add_user(const char* uid, const char* name, bool relay1, bool relay2);
  void clear();
  bool remove(const char* uid);
  bool authorized(const char* uid, uint8_t relay_id) const;
  bool get_user(const char* uid, UserRecord* out) const;
  String to_json() const;
  String to_text() const;
  bool import_text(const char* text);

 private:
  static constexpr size_t kMaxUsers = 1000;
  UserRecord* users_ = nullptr;
  size_t capacity_ = 0;
  bool suppress_save_ = false;
};

} // namespace app
