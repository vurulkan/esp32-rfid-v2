#include "web.h"

#include <WebServer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <cstring>

#include "messages.h"
#include "reader_uart.h"
#include "rtc.h"
#include "settings.h"
#include "wifi.h"
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
  uint32_t expires_at = 0;
};

SessionEntry g_sessions[kMaxSessions];

bool logic_request(AppQueues* queues, LogicRequest& req, LogicResponse* out, uint32_t timeout_ms) {
  if (!queues || !queues->logic_queue || !out) {
    return false;
  }
  QueueHandle_t reply = xQueueCreate(1, sizeof(LogicResponse));
  if (!reply) {
    return false;
  }
  req.reply_queue = reply;
  if (xQueueSend(queues->logic_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
    vQueueDelete(reply);
    return false;
  }
  bool ok = (xQueueReceive(reply, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
  vQueueDelete(reply);
  return ok;
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

bool is_auth_enabled() {
  return settings_get().auth_enabled;
}

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
    g_sessions[i].expires_at = 0;
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
  uint32_t now = millis();
  for (size_t i = 0; i < kMaxSessions; ++i) {
    if (!g_sessions[i].in_use) {
      continue;
    }
    if (g_sessions[i].expires_at != 0 && now > g_sessions[i].expires_at) {
      g_sessions[i].in_use = false;
      g_sessions[i].token[0] = '\0';
      g_sessions[i].expires_at = 0;
      continue;
    }
    if (strncmp(g_sessions[i].token, token, sizeof(g_sessions[i].token)) == 0) {
      if (refresh) {
        g_sessions[i].expires_at = now + kAuthTimeoutMs;
      }
      return true;
    }
  }
  return false;
}

bool check_session(WebServer& server, bool refresh) {
  auto settings = settings_get();
  if (!settings.auth_enabled) {
    return true;
  }
  if (strlen(settings.auth_user) == 0 || strlen(settings.auth_pass) == 0) {
    return true;
  }
  char token[40] = {0};
  if (!extract_cookie_token(server, token, sizeof(token))) {
    return false;
  }
  return session_valid(token, refresh);
}

bool check_auth(WebServer& server) {
  auto settings = settings_get();
  if (!settings.auth_enabled) {
    return true;
  }
  if (strlen(settings.auth_user) == 0 || strlen(settings.auth_pass) == 0) {
    return true;
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
  uint32_t now = millis();
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
  g_sessions[slot].expires_at = now + kAuthTimeoutMs;
  send_login_cookie(server, token);
}

String settings_to_text() {
  auto settings = settings_get();
  String out;
  out += "rtc=";
  out += settings.rtc_enabled ? "1" : "0";
  out += "\nrtc_valid=";
  out += settings.rtc_time_valid ? "1" : "0";
  out += "\nwifi_client=";
  out += settings.wifi_client ? "1" : "0";
  out += "\nwifi_ssid=";
  out += settings.wifi_ssid;
  out += "\nwifi_pass=";
  out += settings.wifi_pass;
  out += "\nwifi_static=";
  out += settings.wifi_static ? "1" : "0";
  out += "\nwifi_ip=";
  out += settings.wifi_ip;
  out += "\nwifi_gateway=";
  out += settings.wifi_gateway;
  out += "\nwifi_mask=";
  out += settings.wifi_mask;
  out += "\nrelay1=";
  out += settings.relay1_name;
  out += "\nrelay2=";
  out += settings.relay2_name;
  out += "\nrelay1_state=";
  out += settings.relay1_state ? "1" : "0";
  out += "\nrelay2_state=";
  out += settings.relay2_state ? "1" : "0";
  out += "\nauth_enabled=";
  out += settings.auth_enabled ? "1" : "0";
  out += "\nauth_user=";
  out += settings.auth_user;
  out += "\nauth_pass=";
  out += settings.auth_pass;
  out += "\napi_key=";
  out += settings.api_key;
  out += '\n';
  return out;
}

bool apply_settings_text(const String& text) {
  Settings settings = settings_get();
  int start = 0;
  while (start < text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) {
      end = text.length();
    }
    String line = text.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.length() == 0) {
      continue;
    }
    if (line.startsWith("rtc=")) {
      String value = line.substring(4);
      value.trim();
      settings.rtc_enabled = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("rtc_valid=")) {
      String value = line.substring(10);
      value.trim();
      settings.rtc_time_valid = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("wifi_client=")) {
      String value = line.substring(12);
      value.trim();
      settings.wifi_client = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("wifi_ssid=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(settings.wifi_ssid, sizeof(settings.wifi_ssid));
    } else if (line.startsWith("wifi_pass=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(settings.wifi_pass, sizeof(settings.wifi_pass));
    } else if (line.startsWith("wifi_static=")) {
      String value = line.substring(12);
      value.trim();
      settings.wifi_static = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("wifi_ip=")) {
      String value = line.substring(8);
      value.trim();
      value.toCharArray(settings.wifi_ip, sizeof(settings.wifi_ip));
    } else if (line.startsWith("wifi_gateway=")) {
      String value = line.substring(13);
      value.trim();
      value.toCharArray(settings.wifi_gateway, sizeof(settings.wifi_gateway));
    } else if (line.startsWith("wifi_mask=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(settings.wifi_mask, sizeof(settings.wifi_mask));
    } else if (line.startsWith("relay1=")) {
      String value = line.substring(7);
      value.trim();
      value.toCharArray(settings.relay1_name, sizeof(settings.relay1_name));
    } else if (line.startsWith("relay2=")) {
      String value = line.substring(7);
      value.trim();
      value.toCharArray(settings.relay2_name, sizeof(settings.relay2_name));
    } else if (line.startsWith("relay1_state=")) {
      String value = line.substring(13);
      value.trim();
      settings.relay1_state = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("relay2_state=")) {
      String value = line.substring(13);
      value.trim();
      settings.relay2_state = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("auth_enabled=")) {
      String value = line.substring(13);
      value.trim();
      settings.auth_enabled = (value == "1" || value == "true" || value == "yes");
    } else if (line.startsWith("auth_user=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(settings.auth_user, sizeof(settings.auth_user));
    } else if (line.startsWith("auth_pass=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(settings.auth_pass, sizeof(settings.auth_pass));
    } else if (line.startsWith("api_key=")) {
      String value = line.substring(8);
      value.trim();
      value.toCharArray(settings.api_key, sizeof(settings.api_key));
    }
  }

  bool ok = settings_set_rtc_enabled(settings.rtc_enabled);
  if (!ok) {
    return false;
  }
  settings_set_rtc_valid(settings.rtc_time_valid);
  settings_set_wifi(settings.wifi_client, settings.wifi_ssid, settings.wifi_pass);
  settings_set_wifi_static(settings.wifi_static, settings.wifi_ip, settings.wifi_gateway, settings.wifi_mask);
  settings_set_relay_names(settings.relay1_name, settings.relay2_name);
  settings_set_relay_state(1, settings.relay1_state);
  settings_set_relay_state(2, settings.relay2_state);
  settings_set_auth(settings.auth_enabled, settings.auth_user, settings.auth_pass, settings.api_key);
  rtc_init(settings.rtc_enabled);
  rtc_set_time_valid(settings.rtc_time_valid);
  return true;
}

String extract_section(const String& body, const char* name) {
  String open = "[";
  open += name;
  open += "]";
  String close = "[/";
  close += name;
  close += "]";
  int start = body.indexOf(open);
  if (start < 0) {
    return "";
  }
  start += open.length();
  int end = body.indexOf(close, start);
  if (end < 0) {
    return "";
  }
  String section = body.substring(start, end);
  section.trim();
  return section;
}

String read_file_or_empty(const char* path) {
  if (!LittleFS.begin()) {
    return "";
  }
  if (!LittleFS.exists(path)) {
    return "";
  }
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    return "";
  }
  String out;
  while (file.available()) {
    out += static_cast<char>(file.read());
  }
  file.close();
  return out;
}

bool write_file_text(const char* path, const String& data) {
  if (!LittleFS.begin()) {
    return false;
  }
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print(data);
  file.close();
  return true;
}

} // namespace

void web_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);
  WebServer server(80);
  const char* header_keys[] = {kApiKeyHeader, kCookieHeader};
  server.collectHeaders(header_keys, 2);

  Serial.println("Web task starting...");
  uint32_t waited_ms = 0;
  while (!wifi_is_ap_ready() && waited_ms < 5000) {
    vTaskDelay(pdMS_TO_TICKS(200));
    waited_ms += 200;
  }
  if (wifi_is_ap_ready()) {
    Serial.println("AP event seen, starting web server.");
  }

  server.on("/", HTTP_GET, [&]() {
    Serial.println("HTTP GET /");
    if (!check_session(server, true)) {
      send_gzip(server, "text/html", login_html_gz, login_html_gz_len);
      return;
    }
    send_gzip(server, "text/html", index_html_gz, index_html_gz_len);
  });

  server.on("/login", HTTP_GET, [&]() {
    if (!is_auth_enabled()) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      return;
    }
    if (check_session(server, true)) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      return;
    }
    send_gzip(server, "text/html", login_html_gz, login_html_gz_len);
  });

  server.on("/app.js", HTTP_GET, [&]() {
    Serial.println("HTTP GET /app.js");
    send_gzip(server, "application/javascript", app_js_gz, app_js_gz_len);
  });

  server.on("/style.css", HTTP_GET, [&]() {
    Serial.println("HTTP GET /style.css");
    send_gzip(server, "text/css", style_css_gz, style_css_gz_len);
  });

  server.on("/auth/login", HTTP_POST, [&]() {
    if (!is_auth_enabled()) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"auth_disabled\"}");
      return;
    }
    if (!server.hasArg("user") || !server.hasArg("pass")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_credentials\"}");
      return;
    }
    String user = server.arg("user");
    String pass = server.arg("pass");
    auto settings = settings_get();
    if (user != settings.auth_user || pass != settings.auth_pass) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    issue_session(server);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/auth/logout", HTTP_ANY, [&]() {
    char token[40] = {0};
    if (extract_cookie_token(server, token, sizeof(token))) {
      for (size_t i = 0; i < kMaxSessions; ++i) {
        if (g_sessions[i].in_use && strncmp(g_sessions[i].token, token, sizeof(g_sessions[i].token)) == 0) {
          g_sessions[i].in_use = false;
          g_sessions[i].token[0] = '\0';
          g_sessions[i].expires_at = 0;
        }
      }
    }
    clear_login_cookie(server);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/users", HTTP_ANY, [&]() {
    Serial.printf("HTTP /users method %d\n", server.method());
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    static LogicRequest req;
    static LogicResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    if (server.method() == HTTP_GET) {
      req.type = LogicRequestType::GetUsers;
      if (logic_request(queues, req, &resp, 300)) {
        server.send(200, "application/json", resp.json);
      } else {
        server.send(500, "application/json", "{\"ok\":false}");
      }
      return;
    }

    if (server.method() == HTTP_POST) {
      if (!server.hasArg("uid") || !server.hasArg("name")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing uid or name\"}");
        return;
      }
      String uid = server.arg("uid");
      String name = server.arg("name");
      bool relay1 = parse_bool_arg(server, "relay1", false);
      bool relay2 = parse_bool_arg(server, "relay2", false);

      req.type = LogicRequestType::AddUser;
      strncpy(req.payload.add_user.uid, uid.c_str(), sizeof(req.payload.add_user.uid) - 1);
      strncpy(req.payload.add_user.name, name.c_str(), sizeof(req.payload.add_user.name) - 1);
      req.payload.add_user.relay1 = relay1 ? 1 : 0;
      req.payload.add_user.relay2 = relay2 ? 1 : 0;

      if (logic_request(queues, req, &resp, 300)) {
        server.send(200, "application/json", resp.json);
      } else {
        server.send(500, "application/json", "{\"ok\":false}");
      }
      return;
    }

    if (server.method() == HTTP_DELETE) {
      if (!server.hasArg("uid")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing uid\"}");
        return;
      }
      String uid = server.arg("uid");
      req.type = LogicRequestType::DeleteUser;
      strncpy(req.payload.del_user.uid, uid.c_str(), sizeof(req.payload.del_user.uid) - 1);

      if (logic_request(queues, req, &resp, 300)) {
        server.send(200, "application/json", resp.json);
      } else {
        server.send(500, "application/json", "{\"ok\":false}");
      }
      return;
    }

    server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
  });

  server.on("/logs", HTTP_ANY, [&]() {
    Serial.printf("HTTP /logs method %d\n", server.method());
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    static LogicRequest req;
    static LogicResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    if (server.method() == HTTP_GET) {
      req.type = LogicRequestType::GetLogs;
      if (logic_request(queues, req, &resp, 400)) {
        server.send(200, "application/json", resp.json);
      } else {
        server.send(500, "application/json", "{\"ok\":false}");
      }
      return;
    }

    if (server.method() == HTTP_DELETE) {
      String scope = server.hasArg("scope") ? server.arg("scope") : "all";
      scope.toLowerCase();
      req.type = (scope == "ram") ? LogicRequestType::ClearLogsRam : LogicRequestType::ClearLogsAll;
      if (logic_request(queues, req, &resp, 400)) {
        server.send(200, "application/json", resp.json);
      } else {
        server.send(500, "application/json", "{\"ok\":false}");
      }
      return;
    }

    server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
  });

  server.on("/logs/export", HTTP_GET, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "text/plain", "unauthorized");
      return;
    }
    String data = read_file_or_empty("/logs.txt");
    server.send(200, "text/plain", data);
  });

  server.on("/rfid", HTTP_GET, [&]() {
    Serial.println("HTTP GET /rfid");
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    static LogicRequest req;
    static LogicResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.type = LogicRequestType::GetLastRfid;
    if (logic_request(queues, req, &resp, 200)) {
      server.send(200, "application/json", resp.json);
    } else {
      server.send(500, "application/json", "{\"ok\":false}");
    }
  });

  server.on("/backup", HTTP_GET, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "text/plain", "unauthorized");
      return;
    }
    String type = server.hasArg("type") ? server.arg("type") : "full";
    type.toLowerCase();

    String out;
    out += kBackupHeader;
    out += "\n";

    if (type == "settings" || type == "full") {
      out += "[settings]\n";
      out += settings_to_text();
      out += "[/settings]\n";
    }

    if (type == "users" || type == "full") {
      out += "[users]\n";
      out += read_file_or_empty("/users.txt");
      out += "[/users]\n";
    }

    server.send(200, "text/plain", out);
  });

  server.on("/restore", HTTP_POST, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
      return;
    }
    String body = server.arg("plain");
    String settings_text = extract_section(body, "settings");
    String users_text = extract_section(body, "users");
    bool has_sections = settings_text.length() > 0 || users_text.length() > 0;

    if (!has_sections) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"no sections\"}");
      return;
    }

    bool ok = true;
    if (settings_text.length() > 0) {
      ok = apply_settings_text(settings_text);
      if (ok) {
        auto settings = settings_get();
        static LogicRequest req;
        static LogicResponse resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        req.type = LogicRequestType::SetRelayState;
        req.payload.relay_state.relay_id = 1;
        req.payload.relay_state.enabled = settings.relay1_state ? 1 : 0;
        if (!logic_request(queues, req, &resp, 300)) {
          ok = false;
        }
        if (ok) {
          memset(&req, 0, sizeof(req));
          memset(&resp, 0, sizeof(resp));
          req.type = LogicRequestType::SetRelayState;
          req.payload.relay_state.relay_id = 2;
          req.payload.relay_state.enabled = settings.relay2_state ? 1 : 0;
          if (!logic_request(queues, req, &resp, 300)) {
            ok = false;
          }
        }
      }
    }

    if (ok && users_text.length() > 0) {
      ok = write_file_text("/users.txt", users_text);
      if (ok) {
        static LogicRequest req;
        static LogicResponse resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        req.type = LogicRequestType::ReloadUsers;
        if (!logic_request(queues, req, &resp, 1000)) {
          ok = false;
        }
      }
    }

    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server.on("/status", HTTP_GET, [&]() {
    Serial.println("HTTP GET /status");
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    String json = "{\"device\":{";
    json += "\"name\":\"esp32-rfid\",";
    json += "\"chip_model\":\"";
    json += ESP.getChipModel();
    json += "\",\"chip_rev\":";
    json += ESP.getChipRevision();
    json += ",\"cores\":";
    json += ESP.getChipCores();
    json += ",\"cpu_mhz\":";
    json += ESP.getCpuFreqMHz();
    json += ",\"uptime_ms\":";
    json += millis();
    json += "},\"memory\":{";
    json += "\"heap_free\":";
    json += ESP.getFreeHeap();
    json += ",\"heap_total\":";
    json += ESP.getHeapSize();
    json += ",\"flash_total\":";
    json += ESP.getFlashChipSize();
    json += ",\"flash_free\":";
    json += ESP.getFreeSketchSpace();
    json += ",\"littlefs_total\":";
    json += LittleFS.totalBytes();
    json += ",\"littlefs_free\":";
    json += LittleFS.usedBytes() > LittleFS.totalBytes() ? 0 : (LittleFS.totalBytes() - LittleFS.usedBytes());
    json += "},\"network\":{";
    bool sta = (WiFi.getMode() == WIFI_STA);
    json += "\"mode\":\"";
    json += sta ? "CLIENT" : "AP";
    json += "\",\"ssid\":\"";
    json += sta ? WiFi.SSID() : WiFi.softAPSSID();
    json += "\",\"ip\":\"";
    json += sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    json += "\",\"gateway\":\"";
    json += sta ? WiFi.gatewayIP().toString() : WiFi.softAPIP().toString();
    json += "\",\"mask\":\"";
    json += sta ? WiFi.subnetMask().toString() : WiFi.softAPSubnetMask().toString();
    json += "\",\"mac\":\"";
    json += sta ? WiFi.macAddress() : WiFi.softAPmacAddress();
    json += "\"}}";
    server.send(200, "application/json", json);
  });

  server.on("/settings", HTTP_ANY, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (server.method() == HTTP_GET) {
      auto settings = settings_get();
      String json = "{\"rtc_enabled\":";
      json += settings.rtc_enabled ? "true" : "false";
      json += ",\"rtc_time_valid\":";
      json += settings.rtc_time_valid ? "true" : "false";
      json += ",\"wifi_client\":";
      json += settings.wifi_client ? "true" : "false";
      json += ",\"wifi_ssid\":\"";
      json += settings.wifi_ssid;
      json += "\",\"wifi_static\":";
      json += settings.wifi_static ? "true" : "false";
      json += ",\"wifi_ip\":\"";
      json += settings.wifi_ip;
      json += "\",\"wifi_gateway\":\"";
      json += settings.wifi_gateway;
      json += "\",\"wifi_mask\":\"";
      json += settings.wifi_mask;
      json += "\",\"relay1\":\"";
      json += settings.relay1_name;
      json += "\",\"relay2\":\"";
      json += settings.relay2_name;
      json += "\",\"relay1_state\":";
      json += settings.relay1_state ? "true" : "false";
      json += ",\"relay2_state\":";
      json += settings.relay2_state ? "true" : "false";
      json += ",\"auth_enabled\":";
      json += settings.auth_enabled ? "true" : "false";
      json += ",\"auth_user\":\"";
      json += settings.auth_user;
      json += "\",\"api_key_mask\":\"";
      json += mask_key(settings.api_key);
      json += "\"";
      json += "}";
      server.send(200, "application/json", json);
      return;
    }

    if (server.method() == HTTP_POST) {
      bool ok = true;
      bool generated_key = false;
      bool enabled = false;
      if (server.hasArg("rtc_enabled")) {
        enabled = parse_bool_arg(server, "rtc_enabled", false);
        ok = settings_set_rtc_enabled(enabled);
        rtc_init(enabled);
        if (!enabled) {
          settings_set_rtc_valid(false);
          rtc_set_time_valid(false);
        }
      }
      auto current = settings_get();
      bool wifi_client = server.hasArg("wifi_client") ? parse_bool_arg(server, "wifi_client", current.wifi_client) : current.wifi_client;
      bool has_ssid = server.hasArg("wifi_ssid");
      bool has_pass = server.hasArg("wifi_pass");
      String ssid = has_ssid ? server.arg("wifi_ssid") : String(current.wifi_ssid);
      String pass = has_pass ? server.arg("wifi_pass") : String(current.wifi_pass);
      bool wifi_static = parse_bool_arg(server, "wifi_static", false);
      String ip = server.hasArg("wifi_ip") ? server.arg("wifi_ip") : "";
      String gateway = server.hasArg("wifi_gateway") ? server.arg("wifi_gateway") : "";
      String mask = server.hasArg("wifi_mask") ? server.arg("wifi_mask") : "";
      String relay1 = server.hasArg("relay1") ? server.arg("relay1") : "";
      String relay2 = server.hasArg("relay2") ? server.arg("relay2") : "";
      String auth_user = server.hasArg("auth_user") ? server.arg("auth_user") : "";
      String auth_pass = server.hasArg("auth_pass") ? server.arg("auth_pass") : "";
      bool auth_enabled = server.hasArg("auth_enabled") ? parse_bool_arg(server, "auth_enabled", false) : settings_get().auth_enabled;
      bool reboot = false;
      if (has_ssid || has_pass || server.hasArg("wifi_client")) {
        if (!has_ssid) {
          ssid = current.wifi_ssid;
        }
        if (!has_pass || pass.isEmpty()) {
          pass = current.wifi_pass;
        }
        settings_set_wifi(wifi_client, ssid.c_str(), pass.c_str());
        if (wifi_client && strlen(ssid.c_str()) > 0) {
          reboot = true;
        }
      }
      if (server.hasArg("wifi_static")) {
        settings_set_wifi_static(wifi_static, ip.c_str(), gateway.c_str(), mask.c_str());
      }
      if (server.hasArg("relay1") || server.hasArg("relay2")) {
        settings_set_relay_names(relay1.c_str(), relay2.c_str());
      }
      if (server.hasArg("auth_enabled")) {
        auto current_auth = settings_get();
        char new_key[40] = {0};
        if (auth_enabled && !current_auth.auth_enabled) {
          generate_api_key(new_key, sizeof(new_key));
          generated_key = true;
        }
        settings_set_auth(auth_enabled,
                          auth_user.length() ? auth_user.c_str() : current_auth.auth_user,
                          auth_pass.length() ? auth_pass.c_str() : current_auth.auth_pass,
                          generated_key ? new_key : current_auth.api_key);
        if (!auth_enabled && current_auth.auth_enabled) {
          clear_sessions();
        }
        if (auth_enabled && !current_auth.auth_enabled) {
          issue_session(server);
        }
      }
      if (generated_key) {
        String json = "{\"ok\":true,\"api_key\":\"";
        json += settings_get().api_key;
        json += "\"";
        if (reboot) {
          json += ",\"reboot\":true";
        }
        json += "}";
        server.send(200, "application/json", json);
      } else if (reboot) {
        server.send(200, "application/json", ok ? "{\"ok\":true,\"reboot\":true}" : "{\"ok\":false}");
      } else {
        server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
      }
      return;
    }

    server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
  });

  server.on("/rtc", HTTP_ANY, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (server.method() == HTTP_GET) {
      RtcDateTime dt{};
      bool ok = rtc_get_datetime(&dt);
      if (!ok) {
        server.send(500, "application/json", "{\"ok\":false}");
        return;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
               dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
      String json = "{\"ok\":true,\"datetime\":\"";
      json += buf;
      json += "\"}";
      server.send(200, "application/json", json);
      return;
    }

    if (server.method() == HTTP_POST) {
      if (!rtc_is_enabled()) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"rtc_disabled\"}");
        return;
      }
      if (!server.hasArg("datetime")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing datetime\"}");
        return;
      }
      String value = server.arg("datetime");
      value.replace('T', ' ');
      if (value.length() < 16) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid datetime\"}");
        return;
      }
      RtcDateTime dt{};
      dt.year = static_cast<uint16_t>(value.substring(0, 4).toInt());
      dt.month = static_cast<uint8_t>(value.substring(5, 7).toInt());
      dt.day = static_cast<uint8_t>(value.substring(8, 10).toInt());
      dt.hour = static_cast<uint8_t>(value.substring(11, 13).toInt());
      dt.minute = static_cast<uint8_t>(value.substring(14, 16).toInt());
      dt.second = value.length() >= 19 ? static_cast<uint8_t>(value.substring(17, 19).toInt()) : 0;
      bool ok = rtc_set_datetime(dt);
      if (ok) {
        settings_set_rtc_valid(true);
      }
      server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
      return;
    }

    server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
  });

  server.on("/maintenance/format", HTTP_POST, [&]() {
    Serial.println("HTTP POST /maintenance/format");
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    bool ok = LittleFS.format();
    if (ok) {
      LittleFS.begin();
      settings_init();
      settings_save();
      rtc_init(false);
    }
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server.on("/maintenance/reboot", HTTP_POST, [&]() {
    Serial.println("HTTP POST /maintenance/reboot");
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(100);
    ESP.restart();
  });

  server.on("/maintenance/uart-test", HTTP_POST, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (!queues || !queues->uart_cmd_queue) {
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"no_uart_queue\"}");
      return;
    }
    UartCmd cmd{};
    cmd.type = UartCmd::Type::Ping;
    cmd.reader_id = 0;
    cmd.allowed = 0;
    xQueueSend(queues->uart_cmd_queue, &cmd, 0);
    uint32_t start = millis();
    uint32_t last = uart_last_pong_ms();
    bool ok = false;
    while (millis() - start < 1000) {
      uint32_t pong = uart_last_pong_ms();
      if (pong != last) {
        ok = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server.on("/maintenance/reader-test", HTTP_POST, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (!queues || !queues->uart_cmd_queue) {
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"no_uart_queue\"}");
      return;
    }
    if (!server.hasArg("reader")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_reader\"}");
      return;
    }
    uint8_t reader_id = static_cast<uint8_t>(server.arg("reader").toInt());
    if (reader_id != 1 && reader_id != 2) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_reader\"}");
      return;
    }
    String action = server.hasArg("action") ? server.arg("action") : "allow";
    action.toLowerCase();
    UartCmd cmd{};
    cmd.type = UartCmd::Type::Feedback;
    cmd.reader_id = reader_id;
    cmd.allowed = (action != "deny") ? 1 : 0;
    xQueueSend(queues->uart_cmd_queue, &cmd, 0);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/maintenance/relay", HTTP_POST, [&]() {
    if (!check_auth(server)) {
      send_unauthorized(server, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }
    if (!server.hasArg("relay")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_relay\"}");
      return;
    }
    uint8_t relay_id = static_cast<uint8_t>(server.arg("relay").toInt());
    String action = server.hasArg("action") ? server.arg("action") : "pulse";
    action.toLowerCase();
    static LogicRequest req;
    static LogicResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    if (action == "on" || action == "off") {
      req.type = LogicRequestType::SetRelayState;
      req.payload.relay_state.relay_id = relay_id;
      req.payload.relay_state.enabled = (action == "on") ? 1 : 0;
    } else {
      req.type = LogicRequestType::TriggerRelay;
      req.payload.trigger_relay.relay_id = relay_id;
      uint32_t duration = 0;
      if (server.hasArg("duration_ms")) {
        duration = static_cast<uint32_t>(server.arg("duration_ms").toInt());
        if (duration < 50) {
          duration = 50;
        } else if (duration > 10000) {
          duration = 10000;
        }
      }
      req.payload.trigger_relay.duration_ms = duration;
    }
    if (logic_request(queues, req, &resp, 400)) {
      server.send(200, "application/json", resp.json);
    } else {
      server.send(500, "application/json", "{\"ok\":false}");
    }
  });

  server.onNotFound([&]() {
    Serial.printf("HTTP 404 %s\n", server.uri().c_str());
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });

  server.begin();
  Serial.println("Web server started on port 80.");

  for (;;) {
    server.handleClient();
    vTaskDelay(kWebLoopDelay);
  }
}

} // namespace app
