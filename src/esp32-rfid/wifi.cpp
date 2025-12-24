#include "wifi.h"

#include <WiFi.h>

#include "settings.h"

namespace app {

namespace {
constexpr const char* kApSsid = "RFID-ACCESS";
constexpr const char* kApPass = "rfid1234";

volatile bool g_ap_ready = false;
volatile bool g_sta_ready = false;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;
#else
void on_wifi_event(WiFiEvent_t event) {
#endif
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  if (event == ARDUINO_EVENT_WIFI_AP_START) {
    g_ap_ready = true;
  } else if (event == ARDUINO_EVENT_WIFI_AP_STOP) {
    g_ap_ready = false;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_sta_ready = true;
    Serial.printf("STA got IP: %s\n", WiFi.localIP().toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_sta_ready = false;
  }
#else
  if (event == SYSTEM_EVENT_AP_START) {
    g_ap_ready = true;
  } else if (event == SYSTEM_EVENT_AP_STOP) {
    g_ap_ready = false;
  } else if (event == SYSTEM_EVENT_STA_GOT_IP) {
    g_sta_ready = true;
    Serial.printf("STA got IP: %s\n", WiFi.localIP().toString().c_str());
  } else if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
    g_sta_ready = false;
  }
#endif
}

} // namespace

bool wifi_is_ap_ready() {
  return g_ap_ready;
}

bool wifi_is_sta_ready() {
  return g_sta_ready;
}

void wifi_task(void* param) {
  (void)param;

  WiFi.onEvent(on_wifi_event);
  Settings settings = settings_get();
  if (settings.wifi_client && settings.wifi_ssid[0] != '\0') {
    WiFi.mode(WIFI_STA);
    if (settings.wifi_static && settings.wifi_ip[0] != '\0' &&
        settings.wifi_gateway[0] != '\0' && settings.wifi_mask[0] != '\0') {
      IPAddress ip, gateway, mask;
      ip.fromString(settings.wifi_ip);
      gateway.fromString(settings.wifi_gateway);
      mask.fromString(settings.wifi_mask);
      WiFi.config(ip, gateway, mask);
    }
    WiFi.begin(settings.wifi_ssid, settings.wifi_pass);
    Serial.printf("STA start: SSID=%s\n", settings.wifi_ssid);
  } else {
    WiFi.mode(WIFI_AP);
    bool ap_ok = WiFi.softAP(kApSsid, kApPass);
    if (ap_ok) {
      g_ap_ready = true;
    }
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP start: %s, IP: %s\n", ap_ok ? "OK" : "FAIL", ip.toString().c_str());
  }

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

} // namespace app
