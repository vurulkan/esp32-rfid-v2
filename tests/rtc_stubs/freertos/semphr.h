#pragma once
#include <chrono>
#include <mutex>
#include "FreeRTOS.h"
namespace rtc_test {
extern bool lock_fail;
}
using SemaphoreHandle_t = std::timed_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
  return !rtc_test::lock_fail && mutex->try_lock_for(std::chrono::milliseconds(ticks));
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
