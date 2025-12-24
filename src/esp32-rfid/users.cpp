#include "users.h"

#include <cstring>
#include <cstdlib>
#include <LittleFS.h>

namespace {
constexpr const char* kUsersPath = "/users.txt";

bool parse_bool(const char* token) {
  if (!token) {
    return false;
  }
  return token[0] == '1' || token[0] == 't' || token[0] == 'T' || token[0] == 'y' || token[0] == 'Y';
}
} // namespace

namespace app {

void UsersDb::init() {
  if (users_ == nullptr) {
    users_ = static_cast<UserRecord*>(malloc(sizeof(UserRecord) * kMaxUsers));
    capacity_ = users_ ? kMaxUsers : 0;
    if (!users_) {
      users_ = static_cast<UserRecord*>(malloc(sizeof(UserRecord) * 500));
      capacity_ = users_ ? 500 : 0;
    }
  }
  if (!users_ || capacity_ == 0) {
    return;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    users_[i].in_use = false;
    users_[i].uid[0] = '\0';
    users_[i].name[0] = '\0';
    users_[i].relay1 = false;
    users_[i].relay2 = false;
  }
}

void UsersDb::clear() {
  if (!users_ || capacity_ == 0) {
    return;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    users_[i].in_use = false;
    users_[i].uid[0] = '\0';
    users_[i].name[0] = '\0';
    users_[i].relay1 = false;
    users_[i].relay2 = false;
  }
}

bool UsersDb::load() {
  if (!LittleFS.begin()) {
    return false;
  }
  if (!LittleFS.exists(kUsersPath)) {
    return true;
  }
  File file = LittleFS.open(kUsersPath, FILE_READ);
  if (!file) {
    return false;
  }
  suppress_save_ = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) {
      continue;
    }
    String uid = line.substring(0, p1);
    String name = line.substring(p1 + 1, p2);
    String d1 = line.substring(p2 + 1, p3);
    String d2 = line.substring(p3 + 1);
    add_user(uid.c_str(), name.c_str(), parse_bool(d1.c_str()), parse_bool(d2.c_str()));
  }
  suppress_save_ = false;
  file.close();
  return true;
}

bool UsersDb::save() const {
  if (!LittleFS.begin()) {
    return false;
  }
  File file = LittleFS.open(kUsersPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  if (!users_ || capacity_ == 0) {
    file.close();
    return true;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    const auto & user = users_[i];
    if (!user.in_use) {
      continue;
    }
    file.print(user.uid);
    file.print('|');
    file.print(user.name);
    file.print('|');
    file.print(user.relay1 ? '1' : '0');
    file.print('|');
    file.print(user.relay2 ? '1' : '0');
    file.print('\n');
  }
  file.close();
  return true;
}

static void copy_field(char* dest, size_t dest_len, const char* src) {
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

bool UsersDb::add_user(const char* uid, const char* name, bool relay1, bool relay2) {
  if (!uid || uid[0] == '\0') {
    return false;
  }

  if (!users_ || capacity_ == 0) {
    return false;
  }

  for (size_t i = 0; i < capacity_; ++i) {
    auto & user = users_[i];
    if (user.in_use && strcmp(user.uid, uid) == 0) {
      return false;
    }
  }

  for (size_t i = 0; i < capacity_; ++i) {
    auto & user = users_[i];
    if (!user.in_use) {
      user.in_use = true;
      copy_field(user.uid, sizeof(user.uid), uid);
      copy_field(user.name, sizeof(user.name), name);
      user.relay1 = relay1;
      user.relay2 = relay2;
      if (!suppress_save_) {
        save();
      }
      return true;
    }
  }

  return false;
}

String UsersDb::to_text() const {
  String out;
  if (!users_ || capacity_ == 0) {
    return out;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    const auto & user = users_[i];
    if (!user.in_use) {
      continue;
    }
    out += user.uid;
    out += '|';
    out += user.name;
    out += '|';
    out += (user.relay1 ? '1' : '0');
    out += '|';
    out += (user.relay2 ? '1' : '0');
    out += '\n';
  }
  return out;
}

bool UsersDb::import_text(const char* text) {
  if (!text) {
    return false;
  }
  suppress_save_ = true;
  clear();
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
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) {
      continue;
    }
    String uid = line.substring(0, p1);
    String name = line.substring(p1 + 1, p2);
    String d1 = line.substring(p2 + 1, p3);
    String d2 = line.substring(p3 + 1);
    add_user(uid.c_str(), name.c_str(), parse_bool(d1.c_str()), parse_bool(d2.c_str()));
  }
  suppress_save_ = false;
  return save();
}

bool UsersDb::remove(const char* uid) {
  if (!uid || uid[0] == '\0') {
    return false;
  }
  if (!users_ || capacity_ == 0) {
    return false;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    auto & user = users_[i];
    if (user.in_use && strcmp(user.uid, uid) == 0) {
      user.in_use = false;
      user.uid[0] = '\0';
      user.name[0] = '\0';
      user.relay1 = false;
      user.relay2 = false;
      if (!suppress_save_) {
        save();
      }
      return true;
    }
  }
  return false;
}

bool UsersDb::authorized(const char* uid, uint8_t relay_id) const {
  if (!uid || uid[0] == '\0') {
    return false;
  }
  if (!users_ || capacity_ == 0) {
    return false;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    const auto & user = users_[i];
    if (user.in_use && strcmp(user.uid, uid) == 0) {
      if (relay_id == 1) {
        return user.relay1;
      }
      if (relay_id == 2) {
        return user.relay2;
      }
      return false;
    }
  }
  return false;
}

bool UsersDb::get_user(const char* uid, UserRecord* out) const {
  if (!uid || uid[0] == '\0' || !out) {
    return false;
  }
  if (!users_ || capacity_ == 0) {
    return false;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    const auto & user = users_[i];
    if (user.in_use && strcmp(user.uid, uid) == 0) {
      *out = user;
      return true;
    }
  }
  return false;
}

String UsersDb::to_json() const {
  String json = "{\"users\":[";
  bool first = true;
  if (!users_ || capacity_ == 0) {
    json += "]}";
    return json;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    const auto & user = users_[i];
    if (!user.in_use) {
      continue;
    }
    if (!first) {
      json += ',';
    }
    first = false;
    json += "{\"uid\":\"";
    json += user.uid;
    json += "\",\"name\":\"";
    json += user.name;
    json += "\",\"relay1\":";
    json += (user.relay1 ? "true" : "false");
    json += ",\"relay2\":";
    json += (user.relay2 ? "true" : "false");
    json += '}';
  }
  json += "]}";
  return json;
}

} // namespace app
