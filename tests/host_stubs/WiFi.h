#pragma once
#include <Arduino.h>
#include <IPAddress.h>
using WiFiEvent_t = int;
constexpr int ARDUINO_EVENT_WIFI_AP_START = 1, ARDUINO_EVENT_WIFI_AP_STOP = 2,
              ARDUINO_EVENT_WIFI_STA_GOT_IP = 3, ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 4;
constexpr int WIFI_AP = 1, WIFI_STA = 2, WIFI_AP_STA = 3;
struct HostWiFi {
  std::function<void(int)> callback;
  int mode_ = 0, ap_calls = 0, begin_calls = 0;
  bool reconnect = false;
  void onEvent(std::function<void(int)> f) { callback = f; }
  void mode(int mode) {
    if (mode_ == WIFI_AP && mode == WIFI_STA && callback) callback(ARDUINO_EVENT_WIFI_AP_STOP);
    mode_ = mode;
  }
  bool softAP(const char*, const char*) {
    ++ap_calls;
    if (callback) callback(ARDUINO_EVENT_WIFI_AP_START);
    return true;
  }
  bool config(IPAddress, IPAddress, IPAddress) { return true; }
  void setAutoReconnect(bool v) { reconnect = v; }
  void begin(const char*, const char*) { ++begin_calls; }
  IPAddress localIP() {
    IPAddress ip;
    ip.fromString("1.2.3.4");
    return ip;
  }
  IPAddress softAPIP() {
    IPAddress ip;
    ip.fromString("192.168.4.1");
    return ip;
  }
};
extern HostWiFi WiFi;
