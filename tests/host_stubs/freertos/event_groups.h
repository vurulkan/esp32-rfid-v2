#pragma once
#include "task.h"
#include <functional>
struct HostEvents {
  EventBits_t bits = 0;
};
using EventGroupHandle_t = HostEvents*;
namespace host {
extern std::function<void()> wait_hook;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, int, int,
                                       TickType_t) {
  if (host::wait_hook) host::wait_hook();
  return e ? (e->bits & bits) : bits;
}
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits) {
  e->bits &= ~bits;
  return e->bits;
}
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits) {
  e->bits |= bits;
  return e->bits;
}
