#include <Arduino.h>
#include <LittleFS.h>

#include "app_context.h"
#include "logic.h"
#include "reader_uart.h"
#include "rtc.h"
#include "settings.h"
#include "web.h"
#include "wifi.h"
#include "messages.h"

namespace app {

static AppQueues g_queues{};

void maintenance_task(void* param);

namespace {
constexpr uint8_t kButtonPin = 0;
constexpr uint32_t kHoldWifiMs = 2000;
constexpr uint32_t kHoldAuthMs = 5000;
constexpr uint32_t kHoldFormatMs = 10000;
} // namespace

} // namespace app

void app::maintenance_task(void* param) {
  (void)param;
  pinMode(kButtonPin, INPUT_PULLUP);
  uint32_t pressed_at = 0;

  for (;;) {
    bool pressed = (digitalRead(kButtonPin) == LOW);
    uint32_t now = millis();
    if (pressed) {
      if (pressed_at == 0) {
        pressed_at = now;
      }
    } else {
      if (pressed_at != 0) {
        uint32_t held = now - pressed_at;
        pressed_at = 0;
        if (held >= kHoldFormatMs) {
          Serial.println("IO0 10s hold: formatting LittleFS...");
          bool ok = LittleFS.format();
          if (ok) {
            LittleFS.begin();
            app::settings_init();
            app::settings_save();
            app::rtc_init(false);
            Serial.println("LittleFS formatted. Restarting...");
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP.restart();
          } else {
            Serial.println("LittleFS format failed.");
          }
        } else if (held >= kHoldAuthMs) {
          Serial.println("IO0 5s hold: disabling authentication.");
          auto settings = app::settings_get();
          app::settings_set_auth(false, settings.auth_user, settings.auth_pass, settings.api_key);
        } else if (held >= kHoldWifiMs) {
          Serial.println("IO0 2s hold: resetting WiFi settings (AP mode).");
          app::settings_set_wifi(false, "", "");
          app::settings_set_wifi_static(false, "", "", "");
          vTaskDelay(pdMS_TO_TICKS(200));
          ESP.restart();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void app_setup() {
  Serial.begin(115200);

  LittleFS.begin();
  app::settings_init();
  app::settings_load();
  app::rtc_init(app::settings_get().rtc_enabled);
  app::rtc_set_time_valid(app::settings_get().rtc_time_valid);

  app::g_queues.rfid_queue = xQueueCreate(8, sizeof(app::RfidEvent));
  app::g_queues.logic_queue = xQueueCreate(8, sizeof(app::LogicRequest));
  app::g_queues.uart_cmd_queue = xQueueCreate(8, sizeof(app::UartCmd));

  xTaskCreatePinnedToCore(app::wifi_task, "wifi_task", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(app::logic_task, "logic_task", 8192, &app::g_queues, 3, nullptr, 1);
  xTaskCreatePinnedToCore(app::reader_uart_task, "reader_uart_task", 4096, &app::g_queues, 2, nullptr, 1);
  xTaskCreatePinnedToCore(app::web_task, "web_task", 8192, &app::g_queues, 2, nullptr, 0);
  xTaskCreatePinnedToCore(app::maintenance_task, "maint_task", 4096, nullptr, 1, nullptr, 0);
}

void app_loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
