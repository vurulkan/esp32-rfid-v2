#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = std::recursive_timed_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_timed_mutex; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
inline int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) {
  if (t == portMAX_DELAY) {
    m->lock();
    return 1;
  }
  return m->try_lock_for(std::chrono::milliseconds(t));
}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t t) {
  return xSemaphoreTake(m, t);
}
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); }

inline void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }
