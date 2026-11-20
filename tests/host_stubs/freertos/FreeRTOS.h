#pragma once
#include <cstdint>
#include <mutex>
#include <limits>
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
using EventBits_t = uint32_t;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = std::numeric_limits<uint32_t>::max();
#define pdMS_TO_TICKS(x) (x)
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define portENTER_CRITICAL(x) (x)->lock()
#define portEXIT_CRITICAL(x) (x)->unlock()
