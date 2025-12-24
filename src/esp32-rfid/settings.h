#pragma once

#include <Arduino.h>

namespace app {

struct Settings {
  bool rtc_enabled;
  bool rtc_time_valid;
  bool wifi_client;
  char wifi_ssid[33];
  char wifi_pass[65];
  bool wifi_static;
  char wifi_ip[16];
  char wifi_gateway[16];
  char wifi_mask[16];
  char relay1_name[24];
  char relay2_name[24];
  bool relay1_state;
  bool relay2_state;
  bool auth_enabled;
  char auth_user[24];
  char auth_pass[40];
  char api_key[40];
};

void settings_init();
bool settings_load();
bool settings_save();
Settings settings_get();
bool settings_set_rtc_enabled(bool enabled);
bool settings_set_rtc_valid(bool valid);
bool settings_set_wifi(bool client_mode, const char* ssid, const char* pass);
bool settings_set_wifi_static(bool enabled, const char* ip, const char* gateway, const char* mask);
bool settings_set_relay_names(const char* relay1, const char* relay2);
bool settings_set_relay_state(uint8_t relay_id, bool enabled);
bool settings_set_auth(bool enabled, const char* user, const char* pass, const char* api_key);

} // namespace app
