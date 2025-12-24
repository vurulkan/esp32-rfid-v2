#include "settings.h"

#include <LittleFS.h>
#include <cstring>

namespace app {

namespace {
constexpr const char* kSettingsPath = "/settings.txt";
Settings g_settings{false, false, false, "", "", false, "", "", "", "Relay 1", "Relay 2", false, false, false, "", "", ""};
} // namespace

void settings_init() {
  g_settings.rtc_enabled = false;
  g_settings.rtc_time_valid = false;
  g_settings.wifi_client = false;
  g_settings.wifi_ssid[0] = '\0';
  g_settings.wifi_pass[0] = '\0';
  g_settings.wifi_static = false;
  g_settings.wifi_ip[0] = '\0';
  g_settings.wifi_gateway[0] = '\0';
  g_settings.wifi_mask[0] = '\0';
  strncpy(g_settings.relay1_name, "Relay 1", sizeof(g_settings.relay1_name) - 1);
  g_settings.relay1_name[sizeof(g_settings.relay1_name) - 1] = '\0';
  strncpy(g_settings.relay2_name, "Relay 2", sizeof(g_settings.relay2_name) - 1);
  g_settings.relay2_name[sizeof(g_settings.relay2_name) - 1] = '\0';
  g_settings.relay1_state = false;
  g_settings.relay2_state = false;
  g_settings.auth_enabled = false;
  g_settings.auth_user[0] = '\0';
  g_settings.auth_pass[0] = '\0';
  g_settings.api_key[0] = '\0';
}

bool settings_load() {
  if (!LittleFS.begin()) {
    return false;
  }
  if (!LittleFS.exists(kSettingsPath)) {
    g_settings.rtc_enabled = false;
    return true;
  }
  File file = LittleFS.open(kSettingsPath, FILE_READ);
  if (!file) {
    return false;
  }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.startsWith("rtc=")) {
      String value = line.substring(4);
      value.trim();
      g_settings.rtc_enabled = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("rtc_valid=")) {
      String value = line.substring(10);
      value.trim();
      g_settings.rtc_time_valid = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("wifi_client=")) {
      String value = line.substring(12);
      value.trim();
      g_settings.wifi_client = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("wifi_ssid=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
    }
    if (line.startsWith("wifi_pass=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(g_settings.wifi_pass, sizeof(g_settings.wifi_pass));
    }
    if (line.startsWith("wifi_static=")) {
      String value = line.substring(12);
      value.trim();
      g_settings.wifi_static = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("wifi_ip=")) {
      String value = line.substring(8);
      value.trim();
      value.toCharArray(g_settings.wifi_ip, sizeof(g_settings.wifi_ip));
    }
    if (line.startsWith("wifi_gateway=")) {
      String value = line.substring(13);
      value.trim();
      value.toCharArray(g_settings.wifi_gateway, sizeof(g_settings.wifi_gateway));
    }
    if (line.startsWith("wifi_mask=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(g_settings.wifi_mask, sizeof(g_settings.wifi_mask));
    }
    if (line.startsWith("relay1=")) {
      String value = line.substring(7);
      value.trim();
      value.toCharArray(g_settings.relay1_name, sizeof(g_settings.relay1_name));
    }
    if (line.startsWith("relay2=")) {
      String value = line.substring(7);
      value.trim();
      value.toCharArray(g_settings.relay2_name, sizeof(g_settings.relay2_name));
    }
    if (line.startsWith("relay1_state=")) {
      String value = line.substring(13);
      value.trim();
      g_settings.relay1_state = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("relay2_state=")) {
      String value = line.substring(13);
      value.trim();
      g_settings.relay2_state = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("auth_enabled=")) {
      String value = line.substring(13);
      value.trim();
      g_settings.auth_enabled = (value == "1" || value == "true" || value == "yes");
    }
    if (line.startsWith("auth_user=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(g_settings.auth_user, sizeof(g_settings.auth_user));
    }
    if (line.startsWith("auth_pass=")) {
      String value = line.substring(10);
      value.trim();
      value.toCharArray(g_settings.auth_pass, sizeof(g_settings.auth_pass));
    }
    if (line.startsWith("api_key=")) {
      String value = line.substring(8);
      value.trim();
      value.toCharArray(g_settings.api_key, sizeof(g_settings.api_key));
    }
  }
  file.close();
  return true;
}

bool settings_save() {
  if (!LittleFS.begin()) {
    return false;
  }
  File file = LittleFS.open(kSettingsPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print("rtc=");
  file.println(g_settings.rtc_enabled ? "1" : "0");
  file.print("rtc_valid=");
  file.println(g_settings.rtc_time_valid ? "1" : "0");
  file.print("wifi_client=");
  file.println(g_settings.wifi_client ? "1" : "0");
  file.print("wifi_ssid=");
  file.println(g_settings.wifi_ssid);
  file.print("wifi_pass=");
  file.println(g_settings.wifi_pass);
  file.print("wifi_static=");
  file.println(g_settings.wifi_static ? "1" : "0");
  file.print("wifi_ip=");
  file.println(g_settings.wifi_ip);
  file.print("wifi_gateway=");
  file.println(g_settings.wifi_gateway);
  file.print("wifi_mask=");
  file.println(g_settings.wifi_mask);
  file.print("relay1=");
  file.println(g_settings.relay1_name);
  file.print("relay2=");
  file.println(g_settings.relay2_name);
  file.print("relay1_state=");
  file.println(g_settings.relay1_state ? "1" : "0");
  file.print("relay2_state=");
  file.println(g_settings.relay2_state ? "1" : "0");
  file.print("auth_enabled=");
  file.println(g_settings.auth_enabled ? "1" : "0");
  file.print("auth_user=");
  file.println(g_settings.auth_user);
  file.print("auth_pass=");
  file.println(g_settings.auth_pass);
  file.print("api_key=");
  file.println(g_settings.api_key);
  file.close();
  return true;
}

Settings settings_get() {
  return g_settings;
}

bool settings_set_rtc_enabled(bool enabled) {
  g_settings.rtc_enabled = enabled;
  return settings_save();
}

bool settings_set_rtc_valid(bool valid) {
  g_settings.rtc_time_valid = valid;
  return settings_save();
}

bool settings_set_wifi(bool client_mode, const char* ssid, const char* pass) {
  g_settings.wifi_client = client_mode;
  if (ssid) {
    strncpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid) - 1);
    g_settings.wifi_ssid[sizeof(g_settings.wifi_ssid) - 1] = '\0';
  }
  if (pass) {
    strncpy(g_settings.wifi_pass, pass, sizeof(g_settings.wifi_pass) - 1);
    g_settings.wifi_pass[sizeof(g_settings.wifi_pass) - 1] = '\0';
  }
  return settings_save();
}

bool settings_set_wifi_static(bool enabled, const char* ip, const char* gateway, const char* mask) {
  g_settings.wifi_static = enabled;
  if (ip) {
    strncpy(g_settings.wifi_ip, ip, sizeof(g_settings.wifi_ip) - 1);
    g_settings.wifi_ip[sizeof(g_settings.wifi_ip) - 1] = '\0';
  }
  if (gateway) {
    strncpy(g_settings.wifi_gateway, gateway, sizeof(g_settings.wifi_gateway) - 1);
    g_settings.wifi_gateway[sizeof(g_settings.wifi_gateway) - 1] = '\0';
  }
  if (mask) {
    strncpy(g_settings.wifi_mask, mask, sizeof(g_settings.wifi_mask) - 1);
    g_settings.wifi_mask[sizeof(g_settings.wifi_mask) - 1] = '\0';
  }
  return settings_save();
}

bool settings_set_relay_names(const char* relay1, const char* relay2) {
  if (relay1) {
    strncpy(g_settings.relay1_name, relay1, sizeof(g_settings.relay1_name) - 1);
    g_settings.relay1_name[sizeof(g_settings.relay1_name) - 1] = '\0';
  }
  if (relay2) {
    strncpy(g_settings.relay2_name, relay2, sizeof(g_settings.relay2_name) - 1);
    g_settings.relay2_name[sizeof(g_settings.relay2_name) - 1] = '\0';
  }
  return settings_save();
}

bool settings_set_relay_state(uint8_t relay_id, bool enabled) {
  if (relay_id == 1) {
    g_settings.relay1_state = enabled;
  } else if (relay_id == 2) {
    g_settings.relay2_state = enabled;
  } else {
    return false;
  }
  return settings_save();
}

bool settings_set_auth(bool enabled, const char* user, const char* pass, const char* api_key) {
  g_settings.auth_enabled = enabled;
  if (user) {
    strncpy(g_settings.auth_user, user, sizeof(g_settings.auth_user) - 1);
    g_settings.auth_user[sizeof(g_settings.auth_user) - 1] = '\0';
  }
  if (pass) {
    strncpy(g_settings.auth_pass, pass, sizeof(g_settings.auth_pass) - 1);
    g_settings.auth_pass[sizeof(g_settings.auth_pass) - 1] = '\0';
  }
  if (api_key) {
    strncpy(g_settings.api_key, api_key, sizeof(g_settings.api_key) - 1);
    g_settings.api_key[sizeof(g_settings.api_key) - 1] = '\0';
  }
  return settings_save();
}

} // namespace app
