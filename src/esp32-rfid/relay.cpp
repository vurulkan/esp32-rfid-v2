#include "relay.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
namespace app {
namespace {
constexpr uint8_t pins[] = {16, 17};
struct State {
  bool manual = false;
  bool pulse = false;
  uint32_t deadline = 0;
};
State state[2];
SemaphoreHandle_t mutex = nullptr;
bool ready = false;
void update_outputs(uint32_t now) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (size_t i = 0; i < 2; ++i)
    if (state[i].pulse && int32_t(now - state[i].deadline) >= 0) {
      state[i].pulse = false;
      digitalWrite(pins[i], state[i].manual ? HIGH : LOW);
    }
  xSemaphoreGive(mutex);
}
void tick(void*) {
  for (;;) {
    update_outputs(millis());
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace
bool relay_init() {
  for (uint8_t pin : pins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }
  mutex = xSemaphoreCreateMutex();
  if (!mutex) return false;
  ready = xTaskCreatePinnedToCore(tick, "relay_tick", 2048, nullptr, 4, nullptr, 1) == pdPASS;
  return ready;
}
bool relay_activate(uint8_t id, uint32_t duration) {
  if (!ready || id < 1 || id > 2 || duration < 50 || duration > 10000) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  auto& s = state[id - 1];
  if (!s.manual) {
    s.pulse = true;
    s.deadline = millis() + duration;
    digitalWrite(pins[id - 1], HIGH);
  }
  xSemaphoreGive(mutex);
  return true;
}
bool relay_set_state(uint8_t id, bool enabled) {
  if (!ready || id < 1 || id > 2) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  state[id - 1].manual = enabled;
  state[id - 1].pulse = false;
  digitalWrite(pins[id - 1], enabled ? HIGH : LOW);
  xSemaphoreGive(mutex);
  return true;
}
bool relay_ready() { return ready; }
bool relay_output_state(uint8_t id) {
  if (!ready || id < 1 || id > 2) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool high = digitalRead(pins[id - 1]) == HIGH;
  xSemaphoreGive(mutex);
  return high;
}
}  // namespace app
