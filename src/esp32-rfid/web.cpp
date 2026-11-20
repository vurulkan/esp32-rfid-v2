#include "web.h"

#include <WebServer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_system.h>
#include <cstring>
#include <algorithm>
#include <unistd.h>

#include "messages.h"
#include "storage.h"
#include "validation.h"
#include "users.h"
#include "relay.h"
#include "logic.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include "log.h"
#include "reader_uart.h"
#include "rtc.h"
#include "settings.h"
#include "app_wifi.h"
#include "web/app.js.gz.h"
#include "web/index.html.gz.h"
#include "web/login.html.gz.h"
#include "web/style.css.gz.h"

namespace app {

namespace {
constexpr TickType_t kWebLoopDelay = pdMS_TO_TICKS(10);
constexpr const char* kBackupHeader = "#RFID_BACKUP";
constexpr const char* kApiKeyHeader = "X-API-Key";
constexpr const char* kCookieHeader = "Cookie";
constexpr const char* kSessionCookieName = "auth_token";
constexpr uint32_t kAuthTimeoutMs = 5 * 60 * 1000;
constexpr size_t kMaxSessions = 4;

struct SessionEntry {
  bool in_use = false;
  char token[33] = {0};
  uint64_t last_seen_ms = 0;
  uint32_t revision = 0;
};

SessionEntry g_sessions[kMaxSessions];

bool logic_request(AppQueues* queues, LogicRequest& req, LogicResponse* out, uint32_t timeout_ms) {
  if (!queues || !queues->logic_queue || !out) {
    return false;
  }
  // The web task owns this queue for its entire lifetime. A timed-out request
  // may still finish; its response is identified and discarded by request_id.
  bool user = req.type == LogicRequestType::GetUsers || req.type == LogicRequestType::AddUser ||
              req.type == LogicRequestType::DeleteUser ||
              req.type == LogicRequestType::RestoreUsers;
  static QueueHandle_t replies[2] = {nullptr, nullptr};
  QueueHandle_t& reply = replies[user ? 1 : 0];
  if (!reply) reply = xQueueCreate(1, sizeof(LogicResponse));
  static uint32_t next_id = 0;
  if (!reply) {
    return false;
  }
  while (xQueueReceive(reply, out, 0) == pdTRUE) {
  }
  req.reply_queue = reply;
  req.request_id = ++next_id;
  if (!req.request_id) req.request_id = ++next_id;
  req.deadline_ms = millis() + timeout_ms;
  if (xQueueSend(user ? queues->user_queue : queues->logic_queue, &req, pdMS_TO_TICKS(50)) !=
      pdTRUE) {
    req.request_id = 0;
    return false;
  }
  const TickType_t started = xTaskGetTickCount();
  const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
  for (;;) {
    const TickType_t elapsed = xTaskGetTickCount() - started;
    if (elapsed >= timeout || xQueueReceive(reply, out, timeout - elapsed) != pdTRUE) {
      return false;
    }
    if (out->request_id == req.request_id) {
      return true;
    }
  }
}

void send_gzip(WebServer& server, const char* content_type, const uint8_t* data, size_t len) {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, content_type, reinterpret_cast<const char*>(data), len);
}

bool parse_bool_arg(WebServer& server, const char* name, bool default_value) {
  if (!server.hasArg(name)) {
    return default_value;
  }
  String val = server.arg(name);
  val.toLowerCase();
  return val == "1" || val == "true" || val == "yes";
}

bool is_auth_enabled() { return settings_get().auth_enabled; }

bool check_api_key(WebServer& server) {
  auto settings = settings_get();
  if (!settings.auth_enabled) {
    return true;
  }
  String key = server.header(kApiKeyHeader);
  if (key.length() == 0 && server.hasArg("api_key")) {
    key = server.arg("api_key");
  }
  return key.length() > 0 && key == settings.api_key;
}

String mask_key(const char* key) {
  if (!key || key[0] == '\0') {
    return "";
  }
  String masked = "********";
  size_t len = strlen(key);
  if (len > 4) {
    masked += key + (len - 4);
  }
  return masked;
}

void generate_api_key(char* out, size_t len) {
  if (!out || len < 9) {
    return;
  }
  static const char* kHex = "0123456789ABCDEF";
  for (size_t i = 0; i < len - 1; ++i) {
    uint32_t r = esp_random();
    out[i] = kHex[r & 0x0F];
  }
  out[len - 1] = '\0';
}

void clear_sessions() {
  for (size_t i = 0; i < kMaxSessions; ++i) {
    g_sessions[i].in_use = false;
    g_sessions[i].token[0] = '\0';
    g_sessions[i].last_seen_ms = 0;
  }
}

bool extract_cookie_token(WebServer& server, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return false;
  }
  String cookie = server.header(kCookieHeader);
  if (cookie.length() == 0) {
    return false;
  }
  String key = String(kSessionCookieName) + "=";
  int start = cookie.indexOf(key);
  if (start < 0) {
    return false;
  }
  start += key.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) {
    end = cookie.length();
  }
  String value = cookie.substring(start, end);
  value.trim();
  if (value.length() == 0) {
    return false;
  }
  value.toCharArray(out, out_len);
  return true;
}

bool session_valid(const char* token, bool refresh) {
  if (!token || token[0] == '\0') {
    return false;
  }
  uint64_t now = esp_timer_get_time() / 1000;
  for (size_t i = 0; i < kMaxSessions; ++i) {
    if (!g_sessions[i].in_use) {
      continue;
    }
    if (g_sessions[i].revision != settings_auth_revision() ||
        now - g_sessions[i].last_seen_ms >= kAuthTimeoutMs) {
      g_sessions[i].in_use = false;
      g_sessions[i].token[0] = '\0';
      g_sessions[i].last_seen_ms = 0;
      continue;
    }
    if (strncmp(g_sessions[i].token, token, sizeof(g_sessions[i].token)) == 0) {
      if (refresh) {
        g_sessions[i].last_seen_ms = now;
      }
      return true;
    }
  }
  return false;
}

bool check_session(WebServer& server, bool refresh) {
  if (!settings_ready()) return false;
  auto settings = settings_get();
  if (!settings.auth_enabled) {
    return true;
  }
  if (strlen(settings.auth_user) == 0 || strlen(settings.auth_pass) == 0) {
    return false;
  }
  char token[40] = {0};
  if (!extract_cookie_token(server, token, sizeof(token))) {
    return false;
  }
  return session_valid(token, refresh);
}

bool check_auth(WebServer& server) {
  if (!settings_ready()) return false;
  auto settings = settings_get();
  if (!settings.auth_enabled) {
    return true;
  }
  if (strlen(settings.auth_user) == 0 || strlen(settings.auth_pass) == 0) {
    return false;
  }
  if (check_api_key(server)) {
    return true;
  }
  return check_session(server, true);
}

void send_login_cookie(WebServer& server, const char* token) {
  String header = String(kSessionCookieName) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict";
  server.sendHeader("Set-Cookie", header);
}

void clear_login_cookie(WebServer& server) {
  String header = String(kSessionCookieName) + "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict";
  server.sendHeader("Set-Cookie", header);
}

void send_unauthorized(WebServer& server, const char* content_type, const char* body) {
  clear_login_cookie(server);
  server.send(401, content_type, body);
}

void issue_session(WebServer& server) {
  char token[33] = {0};
  generate_api_key(token, sizeof(token));
  uint64_t now = esp_timer_get_time() / 1000;
  size_t slot = kMaxSessions;
  for (size_t i = 0; i < kMaxSessions; ++i) {
    if (!g_sessions[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot == kMaxSessions) {
    slot = 0;
  }
  g_sessions[slot].in_use = true;
  strncpy(g_sessions[slot].token, token, sizeof(g_sessions[slot].token) - 1);
  g_sessions[slot].token[sizeof(g_sessions[slot].token) - 1] = '\0';
  g_sessions[slot].last_seen_ms = now;
  g_sessions[slot].revision = settings_auth_revision();
  send_login_cookie(server, token);
}

bool require_auth(WebServer& server) {
  if (check_auth(server)) return true;
  send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}
void result(WebServer& server, bool ok, const char* error = "operation_failed") {
  String json = ok ? "{\"ok\":true}" : String("{\"ok\":false,\"error\":") + json_quote(error) + "}";
  server.send(ok ? 200 : 400, "application/json", json);
}
bool section(const String& body, const char* key, String* out) {
  String begin = String("[") + key + "]", end = String("[/") + key + "]";
  int a = body.indexOf(begin), b = a < 0 ? -1 : body.indexOf(end, a + begin.length());
  if (a < 0 || b < 0) return false;
  *out = body.substring(a + begin.length(), b);
  if (out->startsWith("\r\n"))
    out->remove(0, 2);
  else if (out->startsWith("\n"))
    out->remove(0, 1);
  return true;
}
bool boolean_arg(WebServer& server, const char* key, bool* value) {
  if (!server.hasArg(key)) return true;
  String v = server.arg(key);
  if (v == "1" || v == "true" || v == "yes") {
    *value = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "no") {
    *value = false;
    return true;
  }
  return false;
}
bool text_arg(WebServer& server, const char* key, char* out, size_t capacity) {
  if (!server.hasArg(key)) return true;
  String v = server.arg(key);
  if (!valid_field(v.c_str(), capacity, false) || strlen(v.c_str()) != v.length()) return false;
  v.toCharArray(out, capacity);
  return true;
}
bool uint_arg(WebServer& server, const char* key, uint32_t* value, uint32_t min, uint32_t max) {
  if (!server.hasArg(key)) return true;
  String v = server.arg(key);
  if (!v.length() || v.length() > 10) return false;
  uint64_t n = 0;
  for (size_t i = 0; i < v.length(); ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
    n = n * 10 + v[i] - '0';
    if (n > max) return false;
  }
  if (n < min) return false;
  *value = n;
  return true;
}
void send_logic(WebServer& server, AppQueues* queues, LogicRequest& req, uint32_t timeout = 2000) {
  static LogicResponse resp;
  if (!logic_request(queues, req, &resp, timeout)) {
    server.send(504, "application/json", "{\"ok\":false,\"error\":\"request_timeout\"}");
    return;
  }
  server.send(resp.ok ? 200 : 400, "application/json", resp.json);
}
void stream_files(WebServer& server, const String& prefix, const std::vector<String>& paths,
                  const String& suffix) {
  StorageLock lock;
  if (!lock || !storage_ready()) {
    result(server, false, "storage_unavailable");
    return;
  }
  size_t total = prefix.length() + suffix.length();
  for (const auto& path : paths) {
    File f = LittleFS.open(path.c_str(), FILE_READ);
    if (!f) {
      result(server, false, "read_failed");
      return;
    }
    total += f.size();
    f.close();
  }
  server.setContentLength(total);
  server.send(200, "text/plain", "");
  server.sendContent(prefix);
  char buf[512];
  for (const auto& path : paths) {
    File f = LittleFS.open(path.c_str(), FILE_READ);
    size_t remaining = f.size();
    while (remaining) {
      size_t n = f.readBytes(buf, std::min(remaining, sizeof(buf)));
      if (!n) break;
      server.sendContent(buf, n);
      remaining -= n;
    }
    f.close();
  }
  server.sendContent(suffix);
}
}  // namespace

void web_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);
  WebServer server(80);
  bool ota_started = false, ota_completed = false, ota_failed = false;
  unsigned ota_parts = 0;
  FILE* restore_upload = nullptr;
  String restore_path;
  bool restore_received = false, restore_failed = false;
  unsigned restore_parts = 0;
  size_t restore_size = 0;
  const char* keys[] = {kApiKeyHeader, kCookieHeader};
  server.collectHeaders(keys, 2);
  xEventGroupWaitBits(queues->events, kAppReady, pdFALSE, pdTRUE, portMAX_DELAY);
  for (uint32_t start = millis();
       !wifi_is_ap_ready() && !wifi_is_sta_ready() && millis() - start < 5000;)
    vTaskDelay(pdMS_TO_TICKS(100));
  auto reject_maintenance = [&]() {
    if (!app_maintenance()) return false;
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"maintenance\"}");
    return true;
  };
  server.on("/", HTTP_GET, [&]() {
    bool logged = check_session(server, true);
    send_gzip(server, "text/html", logged ? index_html_gz : login_html_gz,
              logged ? index_html_gz_len : login_html_gz_len);
  });
  server.on("/login", HTTP_GET, [&]() {
    if (!is_auth_enabled() || check_session(server, true)) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
    } else
      send_gzip(server, "text/html", login_html_gz, login_html_gz_len);
  });
  server.on("/app.js", HTTP_GET,
            [&]() { send_gzip(server, "application/javascript", app_js_gz, app_js_gz_len); });
  server.on("/style.css", HTTP_GET,
            [&]() { send_gzip(server, "text/css", style_css_gz, style_css_gz_len); });
  server.on("/auth/login", HTTP_POST, [&]() {
    auto s = settings_get();
    if (!s.auth_enabled) {
      result(server, false, "auth_disabled");
      return;
    }
    if (!s.auth_user[0] || !s.auth_pass[0] || server.arg("user") != s.auth_user ||
        server.arg("pass") != s.auth_pass) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    issue_session(server);
    result(server, true);
  });
  server.on("/auth/logout", HTTP_POST, [&]() {
    char token[40] = {0};
    if (extract_cookie_token(server, token, sizeof(token)))
      for (auto& session : g_sessions)
        if (strcmp(session.token, token) == 0) session.in_use = false;
    clear_login_cookie(server);
    result(server, true);
  });
  server.on("/users", HTTP_ANY, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    LogicRequest req{};
    if (server.method() == HTTP_GET) {
      uint32_t offset = 0, limit = 50;
      if (!uint_arg(server, "offset", &offset, 0, 1000) ||
          !uint_arg(server, "limit", &limit, 1, 100)) {
        result(server, false, "invalid_page");
        return;
      }
      req.type = LogicRequestType::GetUsers;
      req.payload.get_users.offset = offset;
      req.payload.get_users.limit = limit;
    } else if (server.method() == HTTP_POST) {
      req.type = LogicRequestType::AddUser;
      String uid = server.arg("uid"), name = server.arg("name");
      bool a = false, b = false;
      if (!normalize_uid(uid.c_str(), req.payload.add_user.uid, sizeof(req.payload.add_user.uid)) ||
          !valid_field(name.c_str(), sizeof(req.payload.add_user.name)) || !name.length() ||
          !boolean_arg(server, "relay1", &a) || !boolean_arg(server, "relay2", &b)) {
        result(server, false, "invalid_user");
        return;
      }
      strcpy(req.payload.add_user.name, name.c_str());
      req.payload.add_user.relay1 = a;
      req.payload.add_user.relay2 = b;
    } else if (server.method() == HTTP_DELETE) {
      req.type = LogicRequestType::DeleteUser;
      if (!normalize_uid(server.arg("uid").c_str(), req.payload.del_user.uid,
                         sizeof(req.payload.del_user.uid))) {
        result(server, false, "invalid_uid");
        return;
      }
    } else {
      server.send(405, "application/json", "{\"ok\":false}");
      return;
    }
    send_logic(server, queues, req, 10000);
  });
  server.on("/logs", HTTP_ANY, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    LogicRequest req{};
    if (server.method() == HTTP_GET)
      req.type = LogicRequestType::GetLogs;
    else if (server.method() == HTTP_DELETE) {
      String scope = server.hasArg("scope") ? server.arg("scope") : "all";
      if (scope != "all" && scope != "ram") {
        result(server, false, "invalid_scope");
        return;
      }
      req.type = scope == "ram" ? LogicRequestType::ClearLogsRam : LogicRequestType::ClearLogsAll;
    } else {
      server.send(405, "application/json", "{\"ok\":false}");
      return;
    }
    send_logic(server, queues, req);
  });
  server.on("/logs/export", HTTP_GET, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    if (!log_flush(5000)) {
      result(server, false, "log_flush_timeout");
      return;
    }
    StorageLock lock;
    stream_files(server, "", log_files(), "");
  });
  server.on("/rfid", HTTP_GET, [&]() {
    if (!require_auth(server)) return;
    LogicRequest req{};
    req.type = LogicRequestType::GetLastRfid;
    send_logic(server, queues, req);
  });
  server.on("/backup", HTTP_GET, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    String type = server.hasArg("type") ? server.arg("type") : "full";
    if (type != "users" && type != "settings" && type != "full") {
      result(server, false, "invalid_type");
      return;
    }
    StorageLock lock;
    String prefix = "#RFID_BACKUP\n", suffix;
    std::vector<String> paths;
    if (type == "settings" || type == "full") {
      prefix += "[settings]\n";
      prefix += settings_export();
      prefix += "[/settings]\n";
    }
    if (type == "users" || type == "full") {
      prefix += "[users]\n";
      suffix = "[/users]\n";
      if (LittleFS.exists("/users.txt")) paths.push_back("/users.txt");
    }
    stream_files(server, prefix, paths, suffix);
  });
  auto restore_users = [&](const String& path, const Settings* config = nullptr) {
    LogicRequest req{};
    req.type = LogicRequestType::RestoreUsers;
    if (config) {
      req.payload.restore_users.has_settings = true;
      req.payload.restore_users.settings = *config;
    }
    path.toCharArray(req.payload.restore_users.path, sizeof(req.payload.restore_users.path));
    if (!app_prepare_maintenance()) {
      result(server, false, "maintenance_timeout");
      StorageLock lock;
      LittleFS.remove(path.c_str());
      return;
    }
    static LogicResponse resp;
    bool received = logic_request(queues, req, &resp, 15000);
    if (!received && req.request_id == 0) {
      StorageLock lock;
      LittleFS.remove(path.c_str());
      app_set_maintenance(false);
    }
    if (received) {
      if (resp.ok && settings_get().auth_enabled) issue_session(server);
      server.send(resp.ok ? 200 : 400, "application/json", resp.json);
    } else
      server.send(504, "application/json",
                  "{\"ok\":false,\"error\":\"restore_pending_or_failed\"}");
  };
  server.on(
      "/restore", HTTP_POST,
      [&]() {
        if (!require_auth(server) || reject_maintenance()) return;
        if (restore_parts) {
          if (restore_upload) {
            fclose(restore_upload);
            restore_upload = nullptr;
            restore_failed = true;
          }
          String path = restore_path;
          bool ok = restore_received && !restore_failed && restore_parts == 1;
          restore_parts = 0;
          restore_path = "";
          if (!ok) {
            StorageLock lock;
            if (path.length()) LittleFS.remove(path.c_str());
            result(server, false, "invalid_upload");
            return;
          }
          restore_users(path);
          return;
        }
        String body = server.arg("plain");
        if (body.length() > 65536 || strlen(body.c_str()) != body.length()) {
          result(server, false, "backup_too_large");
          return;
        }
        String users, settings;
        bool has_users = section(body, "users", &users),
             has_settings = section(body, "settings", &settings);
        Settings bundle_settings = settings_get();
        if (has_settings && !settings_parse_text(settings, &bundle_settings)) {
          result(server, false, "invalid_settings_backup");
          return;
        }
        if (has_settings && !has_users) {
          StorageLock lock;
          Settings candidate = settings_get();
          if (!settings_parse_text(settings, &candidate)) {
            result(server, false, "invalid_settings_backup");
            return;
          }
          Settings old = settings_get();
          bool ok = settings_apply(candidate);
          if (ok) {
            clear_sessions();
            if (candidate.auth_enabled) issue_session(server);
            rtc_init(candidate.rtc_enabled);
            relay_set_state(1, candidate.relay1_state);
            relay_set_state(2, candidate.relay2_state);
          }
          result(server, ok);
          return;
        }
        if (has_users) {
          char name[40];
          snprintf(name, sizeof(name), "/restore-%08lx.txt", (unsigned long)esp_random());
          if (!storage_write_text(name, users)) {
            result(server, false, "stage_failed");
            return;
          }
          restore_users(name, has_settings ? &bundle_settings : nullptr);
          return;
        }
        result(server, false, "no_sections");
      },
      [&]() {
        if (!check_auth(server) || app_maintenance()) return;
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          if (++restore_parts != 1) {
            restore_failed = true;
            return;
          }
          restore_failed = false;
          restore_received = false;
          restore_size = 0;
          char name[40];
          snprintf(name, sizeof(name), "/restore-%08lx.txt", (unsigned long)esp_random());
          restore_path = name;
          StorageLock lock;
          restore_upload = storage_ready()
                               ? fopen((String(RFID_FS_ROOT) + restore_path).c_str(), "wb")
                               : nullptr;
          if (!restore_upload) restore_failed = true;
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          restore_size += upload.currentSize;
          if (restore_size > 96000) restore_failed = true;
          if (restore_upload && !restore_failed) {
            StorageLock lock;
            if (fwrite(upload.buf, 1, upload.currentSize, restore_upload) != upload.currentSize)
              restore_failed = true;
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (restore_upload) {
            StorageLock lock;
            if (fflush(restore_upload) != 0 || fsync(fileno(restore_upload)) != 0)
              restore_failed = true;
            if (fclose(restore_upload) != 0) restore_failed = true;
            restore_upload = nullptr;
          }
          restore_received = !restore_failed;
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          StorageLock lock;
          if (restore_upload) fclose(restore_upload);
          restore_upload = nullptr;
          LittleFS.remove(restore_path.c_str());
          restore_path = "";
          restore_parts = 0;
          restore_failed = true;
        }
      });
  server.on("/status", HTTP_GET, [&]() {
    if (!require_auth(server)) return;
    RtcDateTime dt{};
    bool valid = rtc_get_datetime(&dt);
    StorageLock lock;
    bool sta = WiFi.status() == WL_CONNECTED;
    String json = "{\"device\":{\"name\":\"esp32-rfid\",\"firmware\":\"2.0.1\",\"chip_model\":";
    json += json_quote(ESP.getChipModel());
    json += ",\"chip_rev\":";
    json += ESP.getChipRevision();
    json += ",\"cores\":";
    json += ESP.getChipCores();
    json += ",\"cpu_mhz\":";
    json += ESP.getCpuFreqMHz();
    json += ",\"uptime_ms\":";
    json += millis();
    json += ",\"reset_reason\":";
    json += int(esp_reset_reason());
    json += "},\"memory\":{\"heap_free\":";
    json += ESP.getFreeHeap();
    json += ",\"heap_min\":";
    json += ESP.getMinFreeHeap();
    json += ",\"heap_largest\":";
    json += heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    json += ",\"heap_total\":";
    json += ESP.getHeapSize();
    json += ",\"flash_total\":";
    json += ESP.getFlashChipSize();
    json += ",\"flash_free\":";
    json += ESP.getFreeSketchSpace();
    json += ",\"littlefs_total\":";
    json += LittleFS.totalBytes();
    json += ",\"littlefs_free\":";
    json += LittleFS.totalBytes() - LittleFS.usedBytes();
    auto health = logic_health();
    json += "},\"health\":{\"logic_heartbeat_ms\":";
    json += health.heartbeat_ms;
    json += ",\"max_queue_delay_ms\":";
    json += health.max_queue_delay_ms;
    json += ",\"cards_processed\":";
    json += health.processed;
    json += ",\"cards_expired\":";
    json += health.expired;
    json += ",\"storage\":";
    json += storage_ready() ? "true" : "false";
    json += ",\"settings\":";
    json += settings_ready() ? "true" : "false";
    json += ",\"users\":";
    json += users_database().ready() ? "true" : "false";
    json += ",\"user_capacity\":";
    json += users_database().capacity();
    json += ",\"relay\":";
    json += relay_ready() ? "true" : "false";
    json += ",\"relay1_gpio\":";
    json += relay_output_state(1) ? "true" : "false";
    json += ",\"relay2_gpio\":";
    json += relay_output_state(2) ? "true" : "false";
    json += ",\"maintenance\":";
    json += app_maintenance() ? "true" : "false";
    json += "},\"rtc\":{\"enabled\":";
    json += rtc_is_enabled() ? "true" : "false";
    json += ",\"valid\":";
    json += valid ? "true" : "false";
    json += ",\"status\":";
    json += json_quote(rtc_state_name(rtc_get_state()));
    json += "},\"logging\":{\"dropped\":";
    json += log_dropped_writes();
    json += ",\"errors\":";
    json += log_write_errors();
    json += ",\"pending\":";
    json += log_pending_writes();
    json += "},\"reader\":{\"received\":";
    json += uart_received_cards();
    json += ",\"dropped\":";
    json += uart_dropped_cards();
    json += ",\"errors\":";
    json += uart_receive_errors();
    json += ",\"invalid\":";
    json += uart_bad_lines();
    json += ",\"feedback_dropped\":";
    json += uart_cmd_drops();
    json += ",\"last_card_ms\":";
    json += uart_last_card_ms();
    json += ",\"last_pong_ms\":";
    json += uart_last_pong_ms();
    json += "},\"network\":{\"mode\":";
    json += json_quote(sta ? "CLIENT" : "AP");
    json += ",\"ssid\":";
    json += json_quote((sta ? WiFi.SSID() : WiFi.softAPSSID()).c_str());
    json += ",\"ip\":";
    json += json_quote((sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
    json += ",\"gateway\":";
    json += json_quote((sta ? WiFi.gatewayIP() : WiFi.softAPIP()).toString().c_str());
    json += ",\"mask\":";
    json += json_quote((sta ? WiFi.subnetMask() : WiFi.softAPSubnetMask()).toString().c_str());
    json += ",\"mac\":";
    json += json_quote((sta ? WiFi.macAddress() : WiFi.softAPmacAddress()).c_str());
    json += "}}";
    server.send(200, "application/json", json);
  });
  server.on("/settings", HTTP_ANY, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    if (server.method() == HTTP_GET) {
      auto s = settings_get();
      RtcDateTime dt{};
      bool valid = rtc_get_datetime(&dt);
      String json = "{\"rtc_enabled\":";
      json += s.rtc_enabled ? "true" : "false";
      json += ",\"rtc_time_valid\":";
      json += valid ? "true" : "false";
      json += ",\"rtc_status\":";
      json += json_quote(rtc_state_name(rtc_get_state()));
#define B(key, field)      \
  json += ",\"" key "\":"; \
  json += s.field ? "true" : "false";
#define T(key, field)      \
  json += ",\"" key "\":"; \
  json += json_quote(s.field);
      B("wifi_client", wifi_client)
      T("wifi_ssid", wifi_ssid)
      B("wifi_static", wifi_static)
      T("wifi_ip", wifi_ip)
      T("wifi_gateway", wifi_gateway)
      T("wifi_mask", wifi_mask)
      T("relay1", relay1_name)
      T("relay2", relay2_name)
      B("relay1_state", relay1_state)
      B("relay2_state", relay2_state) B("auth_enabled", auth_enabled) T("auth_user", auth_user)
#undef B
#undef T
          json += ",\"relay_pulse_ms\":";
      json += s.relay_pulse_ms;
      json += ",\"api_key_mask\":";
      json += json_quote(mask_key(s.api_key).c_str());
      json += '}';
      server.send(200, "application/json", json);
      return;
    }
    if (server.method() != HTTP_POST) {
      server.send(405, "application/json", "{\"ok\":false}");
      return;
    }
    bool ok = true, generated = false, reboot = false, changed_auth = false;
    Settings candidate, old;
    {
      StorageLock lock;
      old = settings_get();
      candidate = old;
#define B(key, field) ok = boolean_arg(server, key, &candidate.field) && ok;
#define T(key, field) ok = text_arg(server, key, candidate.field, sizeof(candidate.field)) && ok;
      B("rtc_enabled", rtc_enabled)
      B("wifi_client", wifi_client)
      B("wifi_static", wifi_static)
      B("auth_enabled", auth_enabled)
      T("wifi_ssid", wifi_ssid)
      T("wifi_ip", wifi_ip)
      T("wifi_gateway", wifi_gateway)
      T("wifi_mask", wifi_mask)
      T("relay1", relay1_name)
      T("relay2", relay2_name) T("auth_user", auth_user)
#undef B
#undef T
          if (server.hasArg("wifi_pass") &&
              (server.arg("wifi_pass").length() || !candidate.wifi_client)) ok =
              text_arg(server, "wifi_pass", candidate.wifi_pass, sizeof(candidate.wifi_pass)) && ok;
      bool clear_wifi = false;
      ok = boolean_arg(server, "wifi_pass_clear", &clear_wifi) && ok;
      if (clear_wifi) candidate.wifi_pass[0] = 0;
      if (server.hasArg("auth_pass") && server.arg("auth_pass").length())
        ok = text_arg(server, "auth_pass", candidate.auth_pass, sizeof(candidate.auth_pass)) && ok;
      ok = uint_arg(server, "relay_pulse_ms", &candidate.relay_pulse_ms, 50, 10000) && ok;
      if (candidate.auth_enabled && !old.auth_enabled) {
        generate_api_key(candidate.api_key, sizeof(candidate.api_key));
        generated = true;
      }
      reboot = candidate.wifi_client != old.wifi_client ||
               strcmp(candidate.wifi_ssid, old.wifi_ssid) ||
               strcmp(candidate.wifi_pass, old.wifi_pass) ||
               candidate.wifi_static != old.wifi_static || strcmp(candidate.wifi_ip, old.wifi_ip) ||
               strcmp(candidate.wifi_gateway, old.wifi_gateway) ||
               strcmp(candidate.wifi_mask, old.wifi_mask);
      changed_auth = candidate.auth_enabled != old.auth_enabled ||
                     strcmp(candidate.auth_user, old.auth_user) ||
                     strcmp(candidate.auth_pass, old.auth_pass) ||
                     strcmp(candidate.api_key, old.api_key);
      ok = ok && settings_apply(candidate);
    }
    if (!ok) {
      result(server, false, "invalid_settings_or_save_failed");
      return;
    }
    if (candidate.rtc_enabled != old.rtc_enabled) rtc_init(candidate.rtc_enabled);
    if (changed_auth) {
      clear_sessions();
      if (candidate.auth_enabled)
        issue_session(server);
      else
        clear_login_cookie(server);
    }
    String json = "{\"ok\":true,\"reboot\":";
    json += reboot ? "true" : "false";
    json += ",\"rtc_time_valid\":";
    json += rtc_has_valid_time() ? "true" : "false";
    json += ",\"rtc_status\":";
    json += json_quote(rtc_state_name(rtc_get_state()));
    if (generated) {
      json += ",\"api_key\":";
      json += json_quote(candidate.api_key);
    }
    json += '}';
    server.send(200, "application/json", json);
  });
  server.on("/rtc", HTTP_ANY, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    RtcDateTime dt{};
    if (server.method() == HTTP_GET) {
      if (!rtc_get_datetime(&dt)) {
        result(server, false, rtc_state_name(rtc_get_state()));
        return;
      }
      char b[32];
      snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", dt.year, dt.month, dt.day, dt.hour,
               dt.minute, dt.second);
      String json = "{\"ok\":true,\"datetime\":";
      json += json_quote(b);
      json += '}';
      server.send(200, "application/json", json);
    } else if (server.method() == HTTP_POST) {
      if (!rtc_parse_datetime(server.arg("datetime").c_str(), &dt)) {
        result(server, false, "invalid_datetime");
        return;
      }
      if (!rtc_is_enabled() || !storage_write_text("/rtc-adjusting", "1\n")) {
        result(server, false, "rtc_disabled_or_storage_failed");
        return;
      }
      bool ok = rtc_set_datetime(dt);
      if (ok) {
        StorageLock lock;
        ok = LittleFS.remove("/rtc-adjusting");
      }
      result(server, ok, "rtc_update_failed");
    } else
      server.send(405, "application/json", "{\"ok\":false}");
  });
  server.on("/maintenance/format", HTTP_POST, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    if (!app_prepare_maintenance()) {
      result(server, false, "maintenance_timeout");
      return;
    }
    bool ok = storage_format();
    if (ok) {
      settings_init();
      ok = settings_save();
    }
    result(server, ok, "format_or_default_save_failed");
    delay(200);
    ESP.restart();
  });
  server.on("/maintenance/reboot", HTTP_POST, [&]() {
    if (!require_auth(server)) return;
    bool ok = app_maintenance() ? log_flush(10000) : app_prepare_maintenance();
    if (!ok) {
      result(server, false, "maintenance_timeout");
      return;
    }
    {
      StorageLock lock;
      result(server, true);
      delay(200);
      ESP.restart();
    }
  });
  server.on("/maintenance/uart-test", HTTP_POST, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    UartCmd cmd{};
    cmd.type = UartCmd::Type::Ping;
    uint32_t before = uart_last_pong_ms();
    if (xQueueSend(queues->uart_cmd_queue, &cmd, 0) != pdTRUE) {
      uart_note_cmd_drop();
      result(server, false, "uart_queue_full");
      return;
    }
    uint32_t start = millis();
    while (millis() - start < 1000 && uart_last_pong_ms() == before) vTaskDelay(pdMS_TO_TICKS(10));
    result(server, uart_last_pong_ms() != before, "nano_no_response");
  });
  server.on("/maintenance/reader-test", HTTP_POST, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    uint32_t id = 0;
    String action = server.hasArg("action") ? server.arg("action") : "allow";
    if (!uint_arg(server, "reader", &id, 1, 2) || !id || (action != "allow" && action != "deny")) {
      result(server, false, "invalid_reader_action");
      return;
    }
    UartCmd cmd{};
    cmd.type = UartCmd::Type::Feedback;
    cmd.reader_id = id;
    cmd.allowed = action == "allow";
    bool ok = xQueueSend(queues->uart_cmd_queue, &cmd, 0) == pdTRUE;
    if (!ok) uart_note_cmd_drop();
    result(server, ok, "uart_queue_full");
  });
  server.on("/maintenance/relay", HTTP_POST, [&]() {
    if (!require_auth(server) || reject_maintenance()) return;
    uint32_t id = 0, duration = settings_get().relay_pulse_ms;
    String action = server.hasArg("action") ? server.arg("action") : "pulse";
    if (!uint_arg(server, "relay", &id, 1, 2) || !id ||
        !uint_arg(server, "duration_ms", &duration, 50, 10000) || !relay_ready()) {
      result(server, false, "invalid_relay_or_unavailable");
      return;
    }
    if (action == "on" || action == "off") {
      bool on = action == "on";
      if (!settings_set_relay_state(id, on)) {
        result(server, false, "save_failed");
        return;
      }
      result(server, relay_set_state(id, on));
    } else if (action == "pulse") {
      LogicRequest req{};
      req.type = LogicRequestType::TriggerRelay;
      req.payload.trigger_relay.relay_id = id;
      req.payload.trigger_relay.duration_ms = duration;
      send_logic(server, queues, req);
    } else
      result(server, false, "invalid_action");
  });
  server.on(
      "/firmware", HTTP_POST,
      [&]() {
        if (!check_auth(server)) {
          if (Update.isRunning()) Update.abort();
          ota_parts = 0;
          ota_started = ota_completed = false;
          send_unauthorized(server, "application/json",
                            "{\"ok\":false,\"error\":\"unauthorized\"}");
          return;
        }
        bool ok =
            ota_parts == 1 && ota_started && ota_completed && !ota_failed && !Update.hasError();
        bool prepared = ok && app_prepare_maintenance();
        ok = ok && prepared;
        if (ok) ok = Update.end(true);  // Commit only after the whole HTTP request was accepted.
        if (!ok && Update.isRunning()) Update.abort();
        ota_parts = 0;
        ota_started = ota_completed = false;
        ota_failed = false;
        result(server, ok, "firmware_update_failed");
        if (ok) {
          delay(200);
          ESP.restart();
        } else if (prepared)
          app_set_maintenance(false);
      },
      [&]() {
        if (!check_auth(server) || app_maintenance()) {
          ota_failed = true;
          return;
        }
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          if (++ota_parts != 1) {
            ota_failed = true;
            if (Update.isRunning()) Update.abort();
            return;
          }
          ota_failed = false;
          ota_completed = false;
          if (Update.isRunning()) Update.abort();
          ota_started = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
          if (!ota_started) ota_failed = true;
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (ota_started && !ota_failed && Update.isRunning() &&
              Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.abort();
            ota_failed = true;
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          ota_completed = ota_started && !ota_failed && Update.isRunning() && upload.totalSize > 0;
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          if (Update.isRunning()) Update.abort();
          ota_parts = 0;
          ota_started = ota_completed = false;
          ota_failed = true;
        }
      });
  server.onNotFound(
      [&]() { server.send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}"); });
  server.begin();
  Serial.println("HTTP server ready");
  for (;;) {
    server.handleClient();
    vTaskDelay(kWebLoopDelay);
  }
}
}  // namespace app
