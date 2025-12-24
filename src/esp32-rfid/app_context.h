#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace app {

struct AppQueues {
  QueueHandle_t rfid_queue;
  QueueHandle_t logic_queue;
  QueueHandle_t uart_cmd_queue;
};

} // namespace app
