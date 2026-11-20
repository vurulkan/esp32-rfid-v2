#include <Arduino.h>
#include <LittleFS.h>

#include "app_context.h"
#include "logic.h"
#include "reader_uart.h"
#include "rtc.h"
#include "settings.h"
#include "web.h"
#include "app_wifi.h"
#include "messages.h"
#include "storage.h"
#include "relay.h"
#include "log.h"
#include "users.h"
#include <atomic>

namespace app {

static AppQueues g_queues{};
static std::atomic<bool> g_maintenance{false};
bool app_maintenance() { return g_maintenance; }
void app_set_maintenance(bool value) {
  g_maintenance = value;
  if (!value && g_queues.events) xEventGroupClearBits(g_queues.events, kLogicIdle);
}
bool app_prepare_maintenance(uint32_t timeout_ms) {
  bool expected = false;
  if (!g_maintenance.compare_exchange_strong(expected, true)) return false;
  xEventGroupClearBits(g_queues.events, kLogicIdle);
  if (!(xEventGroupWaitBits(g_queues.events, kLogicIdle, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(timeout_ms)) &
        kLogicIdle) ||
      !log_flush(timeout_ms)) {
    g_maintenance = false;
    return false;
  }
  return true;
}

void maintenance_task(void* param);

namespace {
constexpr uint8_t kButtonPin = 0;
constexpr uint32_t kHoldWifiMs = 2000;
constexpr uint32_t kHoldAuthMs = 5000;
constexpr uint32_t kHoldFormatMs = 10000;
}  // namespace

}  // namespace app

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
          if (app_prepare_maintenance()) {
            bool ok = storage_format();
            if (ok) {
              settings_init();
              ok = settings_save();
            }
            Serial.println(ok ? "Format completed; restarting."
                              : "Format failed; restarting for recovery.");
            ESP.restart();
          }
          Serial.println("Format preparation timed out; no format performed.");
        } else if (app_maintenance()) {
          Serial.println("Maintenance already active.");
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

  app::storage_init();
  app::settings_init();
  app::settings_load();
  app::rtc_init(app::settings_get().rtc_enabled);
  {
    app::StorageLock lock;
    if (app::storage_ready() && LittleFS.exists("/rtc-adjusting")) app::rtc_invalidate();
  }

  app::relay_init();
  app::g_queues.events = xEventGroupCreate();
  app::g_queues.rfid_queue = xQueueCreate(8, sizeof(app::RfidEvent));
  app::g_queues.logic_queue = xQueueCreate(8, sizeof(app::LogicRequest));
  app::g_queues.user_queue = xQueueCreate(4, sizeof(app::LogicRequest));
  app::g_queues.uart_cmd_queue = xQueueCreate(8, sizeof(app::UartCmd));
  app::g_queues.queue_set = xQueueCreateSet(16);
  if (!app::g_queues.events || !app::g_queues.rfid_queue || !app::g_queues.logic_queue ||
      !app::g_queues.user_queue || !app::g_queues.uart_cmd_queue || !app::g_queues.queue_set ||
      xQueueAddToSet(app::g_queues.rfid_queue, app::g_queues.queue_set) != pdPASS ||
      xQueueAddToSet(app::g_queues.logic_queue, app::g_queues.queue_set) != pdPASS) {
    Serial.println("Queue initialization failed; outputs remain off.");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  struct Task {
    TaskFunction_t fn;
    const char* name;
    uint32_t stack;
    UBaseType_t priority;
    BaseType_t core;
  };
  const Task tasks[] = {{app::logic_task, "logic_task", 8192, 3, 1},
                        {app::users_task, "users_task", 4096, 2, 0},
                        {app::reader_uart_task, "reader_uart_task", 4096, 2, 1},
                        {app::wifi_task, "wifi_task", 4096, 2, 0},
                        {app::web_task, "web_task", 8192, 2, 0},
                        {app::maintenance_task, "maint_task", 4096, 1, 0}};
  for (const auto& task : tasks)
    if (xTaskCreatePinnedToCore(task.fn, task.name, task.stack, &app::g_queues, task.priority,
                                nullptr, task.core) != pdPASS) {
      app::app_set_maintenance(true);
      app::relay_set_state(1, false);
      app::relay_set_state(2, false);
      Serial.printf("Task creation failed: %s\n", task.name);
      for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
