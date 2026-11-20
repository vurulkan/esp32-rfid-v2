#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

namespace app {

struct AppQueues {
  QueueHandle_t user_queue;
  QueueSetHandle_t queue_set;
  EventGroupHandle_t events;
  QueueHandle_t rfid_queue;
  QueueHandle_t logic_queue;
  QueueHandle_t uart_cmd_queue;
};

constexpr EventBits_t kAppReady = 1;
constexpr EventBits_t kLogicIdle = 2;
bool app_maintenance();
void app_set_maintenance(bool value);
bool app_prepare_maintenance(uint32_t timeout_ms = 10000);
}  // namespace app
