#pragma once

#include "app_context.h"

namespace app {

struct LogicHealth {
  uint32_t heartbeat_ms, max_queue_delay_ms, processed, expired;
};
LogicHealth logic_health();
void logic_task(void* param);

}  // namespace app
