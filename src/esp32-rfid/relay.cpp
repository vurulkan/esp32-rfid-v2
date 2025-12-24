#include "relay.h"

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

namespace app {

namespace {
constexpr uint8_t kRelay1Pin = 16;
constexpr uint8_t kRelay2Pin = 17;

TimerHandle_t g_relay1_timer = nullptr;
TimerHandle_t g_relay2_timer = nullptr;
bool g_relay1_manual = false;
bool g_relay2_manual = false;

void relay_off_callback(TimerHandle_t timer) {
  uint32_t relay_id = reinterpret_cast<uint32_t>(pvTimerGetTimerID(timer));
  if (relay_id == 1) {
    digitalWrite(kRelay1Pin, LOW);
  } else if (relay_id == 2) {
    digitalWrite(kRelay2Pin, LOW);
  }
}

void start_timer(TimerHandle_t timer, uint32_t duration_ms) {
  if (timer == nullptr) {
    return;
  }
  xTimerStop(timer, 0);
  xTimerChangePeriod(timer, pdMS_TO_TICKS(duration_ms), 0);
  xTimerStart(timer, 0);
}

} // namespace

void relay_init() {
  pinMode(kRelay1Pin, OUTPUT);
  pinMode(kRelay2Pin, OUTPUT);
  digitalWrite(kRelay1Pin, LOW);
  digitalWrite(kRelay2Pin, LOW);

  g_relay1_timer = xTimerCreate("relay1", pdMS_TO_TICKS(500), pdFALSE,
                                reinterpret_cast<void*>(1), relay_off_callback);
  g_relay2_timer = xTimerCreate("relay2", pdMS_TO_TICKS(500), pdFALSE,
                                reinterpret_cast<void*>(2), relay_off_callback);
}

void relay_activate(uint8_t relay_id, uint32_t duration_ms) {
  if (relay_id == 1) {
    if (g_relay1_manual) {
      return;
    }
    digitalWrite(kRelay1Pin, HIGH);
    start_timer(g_relay1_timer, duration_ms);
  } else if (relay_id == 2) {
    if (g_relay2_manual) {
      return;
    }
    digitalWrite(kRelay2Pin, HIGH);
    start_timer(g_relay2_timer, duration_ms);
  }
}

void relay_set_state(uint8_t relay_id, bool enabled) {
  if (relay_id == 1) {
    g_relay1_manual = enabled;
    if (enabled) {
      xTimerStop(g_relay1_timer, 0);
    }
    digitalWrite(kRelay1Pin, enabled ? HIGH : LOW);
  } else if (relay_id == 2) {
    g_relay2_manual = enabled;
    if (enabled) {
      xTimerStop(g_relay2_timer, 0);
    }
    digitalWrite(kRelay2Pin, enabled ? HIGH : LOW);
  }
}

} // namespace app
