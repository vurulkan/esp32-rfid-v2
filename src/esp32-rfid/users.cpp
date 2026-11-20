#include "users.h"
#include "storage.h"
#include "validation.h"
#include "messages.h"
#include "app_context.h"
#include "rtc.h"
#include "relay.h"
#include <LittleFS.h>
#include <cstdlib>
namespace app {
namespace {
struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(SemaphoreHandle_t x) : m(x) {
    if (m) xSemaphoreTake(m, portMAX_DELAY);
  }
  ~Lock() {
    if (m) xSemaphoreGive(m);
  }
};
bool read_records(FILE* f, UserRecord* records, size_t capacity, Settings* config,
                  bool* has_config) {
  char line[96];
  size_t count = 0;
  bool wrapped = false, seen_users = false, seen_settings = false;
  enum Section { None, Users, SettingsSection };
  Section section = None;
  String settings_text;
  *has_config = false;
  while (true) {
    long before = ftell(f);
    if (!fgets(line, sizeof(line), f)) break;
    if (ftell(f) - before != long(strlen(line))) return false;
    size_t n = strlen(line);
    if (n == sizeof(line) - 1 && line[n - 1] != '\n') return false;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    if (!wrapped && !count && strcmp(line, "#RFID_BACKUP") == 0) {
      wrapped = true;
      continue;
    }
    if (wrapped) {
      if (section == None) {
        if (strcmp(line, "[users]") == 0 && !seen_users) {
          section = Users;
          seen_users = true;
          continue;
        }
        if (strcmp(line, "[settings]") == 0 && !seen_settings) {
          section = SettingsSection;
          seen_settings = true;
          continue;
        }
        return false;
      }
      if (section == Users && strcmp(line, "[/users]") == 0) {
        section = None;
        continue;
      }
      if (section == SettingsSection) {
        if (strcmp(line, "[/settings]") == 0) {
          section = None;
          continue;
        }
        if (settings_text.length() + n + 1 > 4096 || !settings_text.concat(line, n) ||
            !settings_text.concat("\n", 1))
          return false;
        continue;
      }
    }
    if (count >= capacity) return false;
    char* a = strchr(line, '|');
    if (!a) return false;
    *a++ = 0;
    char* b = strchr(a, '|');
    if (!b) return false;
    *b++ = 0;
    char* c = strchr(b, '|');
    if (!c) return false;
    *c++ = 0;
    if ((strcmp(b, "0") && strcmp(b, "1")) || (strcmp(c, "0") && strcmp(c, "1"))) return false;
    UserRecord r{};
    if (!normalize_uid(line, r.uid, sizeof(r.uid)) || !valid_field(a, sizeof(r.name))) return false;
    for (size_t i = 0; i < count; ++i)
      if (strcmp(records[i].uid, r.uid) == 0) return false;
    strcpy(r.name, a);
    r.in_use = true;
    r.relay1 = *b == '1';
    r.relay2 = *c == '1';
    records[count++] = r;
  }
  if (ferror(f) || (wrapped && (!seen_users || section != None))) return false;
  if (seen_settings) {
    if (!settings_parse_text(settings_text, config)) return false;
    *has_config = true;
  }
  return true;
}
bool write_record(FILE* f, const UserRecord& r) {
  return !r.in_use || fprintf(f, "%s|%s|%d|%d\n", r.uid, r.name, r.relay1, r.relay2) > 0;
}
}  // namespace
UsersDb& users_database() {
  static UsersDb db;
  return db;
}
UsersDb::~UsersDb() {
  free(users_);
  if (mutex_) vSemaphoreDelete(mutex_);
}
void UsersDb::init() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  if (mutex_) capacity_ = kMaxUsers;
}
bool UsersDb::replace_file(const char* path, bool persist, const Settings* config) {
  StorageLock fs;
  if (!fs || !storage_ready() || !mutex_) return false;
  auto* replacement = static_cast<UserRecord*>(calloc(capacity_, sizeof(UserRecord)));
  if (!replacement) return false;
  String full = RFID_FS_ROOT;
  full += path;
  FILE* f = fopen(full.c_str(), "rb");
  Settings parsed_settings = settings_get();
  bool has_parsed_settings = false;
  bool ok = f && read_records(f, replacement, capacity_, &parsed_settings, &has_parsed_settings);
  if (has_parsed_settings) {
    if (config || !persist)
      ok = false;
    else
      config = &parsed_settings;
  }
  if (f) fclose(f);
  if (ok && config && !settings_validate(*config)) ok = false;
  if (ok && persist) {
    auto write = [&](FILE* out) {
      for (size_t i = 0; i < capacity_; ++i)
        if (!write_record(out, replacement[i])) return false;
      return true;
    };
    ok = config ? storage_replace_access_state(write, settings_encode(*config))
                : storage_atomic_write("/users.txt", write);
  }
  if (ok) {
    UserRecord* old;
    {
      Lock lock(mutex_);
      old = users_;
      users_ = replacement;
      ready_ = true;
      if (config) settings_publish_committed(*config);
    }
    free(old);
  } else
    free(replacement);
  return ok;
}
bool UsersDb::load() {
  StorageLock fs;
  if (!fs || !storage_ready() || !mutex_) return false;
  if (!LittleFS.exists("/users.txt")) {
    Lock lock(mutex_);
    if (!users_) users_ = static_cast<UserRecord*>(calloc(capacity_, sizeof(UserRecord)));
    ready_ = users_ != nullptr;
    return ready_;
  }
  return replace_file("/users.txt", false);
}
bool UsersDb::restore_file(const char* path, const Settings* config) {
  return replace_file(path, true, config);
}
bool UsersDb::save_overlay(size_t index, const UserRecord& candidate) const {
  return storage_atomic_write("/users.txt", [&](FILE* f) {
    for (size_t i = 0; i < capacity_; ++i) {
      UserRecord r;
      {
        Lock lock(mutex_);
        r = i == index ? candidate : users_[i];
      }
      if (!write_record(f, r)) return false;
    }
    return true;
  });
}
bool UsersDb::add_user(const char* uid, const char* name, bool a, bool b) {
  if (!ready_) return false;
  UserRecord r{};
  if (!normalize_uid(uid, r.uid, sizeof(r.uid)) || !valid_field(name, sizeof(r.name))) return false;
  strcpy(r.name, name);
  r.in_use = true;
  r.relay1 = a;
  r.relay2 = b;
  StorageLock fs;
  if (!fs) return false;
  size_t slot = capacity_;
  {
    Lock lock(mutex_);
    for (size_t i = 0; i < capacity_; ++i) {
      if (users_[i].in_use && strcmp(users_[i].uid, r.uid) == 0) return false;
      if (!users_[i].in_use && slot == capacity_) slot = i;
    }
  }
  if (slot == capacity_ || !save_overlay(slot, r)) return false;
  {
    Lock lock(mutex_);
    users_[slot] = r;
  }
  return true;
}
bool UsersDb::remove(const char* uid) {
  if (!ready_) return false;
  char key[20];
  if (!normalize_uid(uid, key, sizeof(key))) return false;
  StorageLock fs;
  if (!fs) return false;
  size_t slot = capacity_;
  {
    Lock lock(mutex_);
    for (size_t i = 0; i < capacity_; ++i)
      if (users_[i].in_use && strcmp(users_[i].uid, key) == 0) {
        slot = i;
        break;
      }
  }
  UserRecord empty{};
  if (slot == capacity_ || !save_overlay(slot, empty)) return false;
  {
    Lock lock(mutex_);
    users_[slot] = empty;
  }
  return true;
}
bool UsersDb::get_user(const char* uid, UserRecord* out) const {
  if (!ready_ || !out) return false;
  char key[20];
  if (!normalize_uid(uid, key, sizeof(key))) return false;
  Lock lock(mutex_);
  for (size_t i = 0; i < capacity_; ++i)
    if (users_[i].in_use && strcmp(users_[i].uid, key) == 0) {
      *out = users_[i];
      return true;
    }
  return false;
}
bool UsersDb::authorized(const char* uid, uint8_t id) const {
  UserRecord r{};
  return get_user(uid, &r) && ((id == 1 && r.relay1) || (id == 2 && r.relay2));
}
String UsersDb::to_json_page(size_t offset, size_t limit, size_t& total) const {
  total = 0;
  String json = "{\"users\":[";
  if (!ready_) return "{\"ok\":false,\"error\":\"users_unavailable\"}";
  Lock lock(mutex_);
  size_t emitted = 0, position = 0;
  for (size_t i = 0; i < capacity_; ++i)
    if (users_[i].in_use) ++total;
  for (size_t i = 0; i < capacity_; ++i) {
    const auto& r = users_[i];
    if (!r.in_use) continue;
    size_t pos = position++;
    if (pos < offset || emitted >= limit) continue;
    String item = "{\"uid\":";
    item += json_quote(r.uid);
    item += ",\"name\":";
    item += json_quote(r.name);
    item += ",\"relay1\":";
    item += r.relay1 ? "true" : "false";
    item += ",\"relay2\":";
    item += r.relay2 ? "true" : "false";
    item += '}';
    if (json.length() + item.length() + 100 > kLogicResponseMax) break;
    if (emitted) json += ',';
    json += item;
    ++emitted;
  }
  json += "],\"total\":";
  json += total;
  json += ",\"offset\":";
  json += offset;
  json += ",\"limit\":";
  json += limit;
  json += "}";
  return json;
}
void users_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);
  xEventGroupWaitBits(queues->events, kAppReady, pdFALSE, pdTRUE, portMAX_DELAY);
  static LogicResponse resp;
  for (;;) {
    LogicRequest req{};
    if (xQueueReceive(queues->user_queue, &req, portMAX_DELAY) != pdTRUE) continue;
    bool ok = false;
    String json;
    if (app_maintenance() && req.type != LogicRequestType::RestoreUsers)
      json = "{\"ok\":false,\"error\":\"maintenance\"}";
    else
      switch (req.type) {
        case LogicRequestType::GetUsers: {
          size_t total;
          json = users_database().to_json_page(req.payload.get_users.offset,
                                               req.payload.get_users.limit, total);
          ok = users_database().ready();
          break;
        }
        case LogicRequestType::AddUser: {
          UserRecord old{};
          if (users_database().get_user(req.payload.add_user.uid, &old))
            json = "{\"ok\":false,\"error\":\"uid_exists\"}";
          else
            ok =
                users_database().add_user(req.payload.add_user.uid, req.payload.add_user.name,
                                          req.payload.add_user.relay1, req.payload.add_user.relay2);
          break;
        }
        case LogicRequestType::DeleteUser:
          ok = users_database().remove(req.payload.del_user.uid);
          break;
        case LogicRequestType::RestoreUsers:
          ok = users_database().restore_file(req.payload.restore_users.path,
                                             req.payload.restore_users.has_settings
                                                 ? &req.payload.restore_users.settings
                                                 : nullptr);
          if (ok) {
            auto s = settings_get();
            rtc_init(s.rtc_enabled);
            relay_set_state(1, s.relay1_state);
            relay_set_state(2, s.relay2_state);
          }
          break;
        default:
          break;
      }
    if (req.type == LogicRequestType::RestoreUsers) {
      StorageLock fs;
      LittleFS.remove(req.payload.restore_users.path);
      if (storage_ready()) app_set_maintenance(false);
    }
    if (!json.length())
      json = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"user_operation_failed\"}";
    memset(&resp, 0, sizeof(resp));
    resp.request_id = req.request_id;
    resp.ok = ok;
    strncpy(resp.json, json.c_str(), sizeof(resp.json) - 1);
    if (req.reply_queue) xQueueOverwrite(req.reply_queue, &resp);
  }
}
}  // namespace app
