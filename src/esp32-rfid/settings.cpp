#include "settings.h"
#include "storage.h"
#include "validation.h"
#include <LittleFS.h>
#include <IPAddress.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
namespace app {
namespace {
Settings defaults() {
  Settings s{};
  strcpy(s.relay1_name, "Relay 1");
  strcpy(s.relay2_name, "Relay 2");
  s.relay_pulse_ms = kDefaultRelayPulseMs;
  return s;
}
Settings g_settings = defaults();
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_valid{false};
std::atomic<uint32_t> g_auth_revision{0};
void publish(const Settings& s) {
  portENTER_CRITICAL(&g_mux);
  if (s.auth_enabled != g_settings.auth_enabled || strcmp(s.auth_user, g_settings.auth_user) ||
      strcmp(s.auth_pass, g_settings.auth_pass) || strcmp(s.api_key, g_settings.api_key))
    ++g_auth_revision;
  g_settings = s;
  portEXIT_CRITICAL(&g_mux);
}
bool boolean(const String& value, bool* out) {
  if (value == "1" || value == "true" || value == "yes") {
    *out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no") {
    *out = false;
    return true;
  }
  return false;
}
bool copy_value(const String& value, char* out, size_t len) {
  if (value.length() >= len || !valid_field(value.c_str(), len, false)) return false;
  value.toCharArray(out, len);
  return true;
}
bool ip_valid(const char* text) {
  IPAddress ip;
  return ip.fromString(text);
}
}  // namespace
uint32_t settings_auth_revision() { return g_auth_revision; }
void settings_publish_committed(const Settings& s) {
  publish(s);
  g_valid = true;
}
String settings_export() { return settings_encode(settings_get()); }
void settings_init() {
  publish(defaults());
  g_valid = false;
}
Settings settings_get() {
  portENTER_CRITICAL(&g_mux);
  Settings s = g_settings;
  portEXIT_CRITICAL(&g_mux);
  return s;
}
bool settings_ready() { return g_valid; }
bool settings_validate(const Settings& s) {
  if (!valid_field(s.wifi_ssid, sizeof(s.wifi_ssid), false) ||
      !valid_field(s.wifi_pass, sizeof(s.wifi_pass), false) ||
      !valid_field(s.relay1_name, sizeof(s.relay1_name), false) ||
      !valid_field(s.relay2_name, sizeof(s.relay2_name), false) ||
      !valid_field(s.auth_user, sizeof(s.auth_user), false) ||
      !valid_field(s.auth_pass, sizeof(s.auth_pass), false) ||
      !valid_field(s.api_key, sizeof(s.api_key), false) || s.relay_pulse_ms < 50 ||
      s.relay_pulse_ms > 10000)
    return false;
  if (s.auth_enabled && (!s.auth_user[0] || !s.auth_pass[0] || !s.api_key[0])) return false;
  if (s.wifi_client && !s.wifi_ssid[0]) return false;
  if (s.wifi_static) {
    if (!ip_valid(s.wifi_ip) || !ip_valid(s.wifi_gateway) || !ip_valid(s.wifi_mask)) return false;
    IPAddress mask;
    mask.fromString(s.wifi_mask);
    uint32_t m =
        (uint32_t(mask[0]) << 24) | (uint32_t(mask[1]) << 16) | (uint32_t(mask[2]) << 8) | mask[3];
    uint32_t inv = ~m;
    if (!m || (inv & (inv + 1))) return false;
  }
  return true;
}
String settings_encode(const Settings& s) {
  String out;
  if (!out.reserve(1000)) return String();
#define BOOL(key, field)      \
  out += key "=";             \
  out += s.field ? "1" : "0"; \
  out += '\n';
#define TEXT(key, field) \
  out += key "=";        \
  out += s.field;        \
  out += '\n';
  BOOL("rtc", rtc_enabled)
  BOOL("rtc_valid", rtc_time_valid)
  BOOL("wifi_client", wifi_client)
  TEXT("wifi_ssid", wifi_ssid) TEXT("wifi_pass", wifi_pass) BOOL("wifi_static", wifi_static)
      TEXT("wifi_ip", wifi_ip) TEXT("wifi_gateway", wifi_gateway) TEXT("wifi_mask", wifi_mask)
          TEXT("relay1", relay1_name) TEXT("relay2", relay2_name) BOOL("relay1_state", relay1_state)
              BOOL("relay2_state", relay2_state) out += "relay_pulse_ms=";
  out += s.relay_pulse_ms;
  out += '\n';
  BOOL("auth_enabled", auth_enabled)
  TEXT("auth_user", auth_user)
  TEXT("auth_pass", auth_pass)
  TEXT("api_key", api_key)
#undef BOOL
#undef TEXT
      return out;
}
bool settings_parse_text(const String& text, Settings* s) {
  if (!s || text.length() > 4096 || strlen(text.c_str()) != text.length()) return false;
  int start = 0;
  bool any = false;
  String seen = "|";
  while (start < int(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    start = end + 1;
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (!line.length()) continue;
    int split = line.indexOf('=');
    if (split <= 0) return false;
    String key = line.substring(0, split), value = line.substring(split + 1);
    String marker = key + "|";
    if (seen.indexOf("|" + marker) >= 0) return false;
    seen += marker;
    bool ok = false;
#define B(keyname, field)           \
  if (key == keyname)               \
    ok = boolean(value, &s->field); \
  else
#define T(keyname, field)                               \
  if (key == keyname)                                   \
    ok = copy_value(value, s->field, sizeof(s->field)); \
  else
    B("rtc", rtc_enabled)
    B("rtc_valid", rtc_time_valid)
    B("wifi_client", wifi_client)
    T("wifi_ssid", wifi_ssid) T("wifi_pass", wifi_pass) B("wifi_static", wifi_static)
        T("wifi_ip", wifi_ip) T("wifi_gateway", wifi_gateway) T("wifi_mask", wifi_mask)
            T("relay1", relay1_name) T("relay2", relay2_name) B("relay1_state", relay1_state)
                B("relay2_state", relay2_state) B("auth_enabled", auth_enabled)
                    T("auth_user", auth_user) T("auth_pass", auth_pass)
                        T("api_key", api_key) if (key == "relay_pulse_ms") {
      ok = value.length() > 0;
      for (size_t i = 0; i < value.length(); ++i)
        if (value[i] < '0' || value[i] > '9') ok = false;
      long n = value.toInt();
      if (n < 50 || n > 10000) ok = false;
      if (ok) s->relay_pulse_ms = n;
    }
    else return false;
#undef B
#undef T
    if (!ok) return false;
    any = true;
  }
  return any && settings_validate(*s);
}
bool settings_apply(const Settings& s) {
  StorageLock lock;
  if (!lock || !settings_validate(s)) return false;
  String encoded = settings_encode(s);
  if (!encoded.length() || !storage_write_text("/settings.txt", encoded)) return false;
  publish(s);
  g_valid = true;
  return true;
}
bool settings_load() {
  StorageLock lock;
  if (!lock || !storage_ready()) return false;
  if (!LittleFS.exists("/settings.txt")) {
    g_valid = true;
    return true;
  }
  String text;
  Settings s = defaults();
  bool ok = storage_read_text("/settings.txt", &text, 4096);
  for (const char* key :
       {"auth_enabled=", "auth_user=", "auth_pass=", "api_key=", "wifi_client=", "rtc="})
    if (!text.startsWith(key) && text.indexOf(String("\n") + key) < 0) ok = false;
  if (ok) ok = settings_parse_text(text, &s);
  if (ok) {
    publish(s);
    g_valid = true;
  } else {
    s = defaults();
    s.auth_enabled = true;
    publish(s);
    g_valid = false;
  }
  return ok;
}
bool settings_save() { return settings_apply(settings_get()); }
template <class F>
bool mutate(F change) {
  StorageLock lock;
  if (!lock) return false;
  Settings s = settings_get();
  if (!change(s)) return false;
  return settings_apply(s);
}
bool settings_set_rtc_enabled(bool value) {
  return mutate([&](Settings& s) {
    s.rtc_enabled = value;
    return true;
  });
}
bool settings_set_rtc_valid(bool value) {
  return mutate([&](Settings& s) {
    s.rtc_time_valid = value;
    return true;
  });
}
bool settings_set_wifi(bool client, const char* ssid, const char* pass) {
  return mutate([&](Settings& s) {
    s.wifi_client = client;
    return (!ssid || copy_value(ssid, s.wifi_ssid, sizeof(s.wifi_ssid))) &&
           (!pass || copy_value(pass, s.wifi_pass, sizeof(s.wifi_pass)));
  });
}
bool settings_set_wifi_static(bool value, const char* ip, const char* gw, const char* mask) {
  return mutate([&](Settings& s) {
    s.wifi_static = value;
    return copy_value(ip ? ip : "", s.wifi_ip, sizeof(s.wifi_ip)) &&
           copy_value(gw ? gw : "", s.wifi_gateway, sizeof(s.wifi_gateway)) &&
           copy_value(mask ? mask : "", s.wifi_mask, sizeof(s.wifi_mask));
  });
}
bool settings_set_relay_names(const char* a, const char* b) {
  return mutate([&](Settings& s) {
    return (!a || copy_value(a, s.relay1_name, sizeof(s.relay1_name))) &&
           (!b || copy_value(b, s.relay2_name, sizeof(s.relay2_name)));
  });
}
bool settings_set_relay_state(uint8_t id, bool value) {
  return mutate([&](Settings& s) {
    if (id == 1)
      s.relay1_state = value;
    else if (id == 2)
      s.relay2_state = value;
    else
      return false;
    return true;
  });
}
bool settings_set_relay_pulse_ms(uint32_t value) {
  return mutate([&](Settings& s) {
    s.relay_pulse_ms = value;
    return true;
  });
}
bool settings_set_auth(bool value, const char* user, const char* pass, const char* key) {
  return mutate([&](Settings& s) {
    s.auth_enabled = value;
    return (!user || copy_value(user, s.auth_user, sizeof(s.auth_user))) &&
           (!pass || copy_value(pass, s.auth_pass, sizeof(s.auth_pass))) &&
           (!key || copy_value(key, s.api_key, sizeof(s.api_key)));
  });
}
}  // namespace app
