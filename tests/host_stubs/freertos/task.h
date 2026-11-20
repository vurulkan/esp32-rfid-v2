#pragma once
#include "FreeRTOS.h"
#include "queue.h"
#include <vector>
#include <thread>
using TaskFunction_t = void (*)(void*);
namespace host {
extern bool task_ok;
extern std::vector<std::pair<TaskFunction_t, void*>> tasks;
extern uint32_t now;
extern uint32_t stop_at;
}  // namespace host
inline int xTaskCreatePinnedToCore(TaskFunction_t f, const char*, uint32_t, void* p, UBaseType_t,
                                   void*, BaseType_t) {
  if (!host::task_ok) return 0;
  host::tasks.push_back({f, p});
  return 1;
}
inline void vTaskDelay(TickType_t n) {
  host::now += n;
  if (host::now >= host::stop_at) throw host::Stop{};
  std::this_thread::yield();
}
inline TickType_t xTaskGetTickCount() { return host::now; }
