#pragma once

#include <Arduino.h>

namespace app {

bool relay_init();
bool relay_activate(uint8_t relay_id, uint32_t duration_ms);
bool relay_set_state(uint8_t relay_id, bool enabled);

bool relay_ready();
bool relay_output_state(uint8_t id);
}  // namespace app
