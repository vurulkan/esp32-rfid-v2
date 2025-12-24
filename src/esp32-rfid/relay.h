#pragma once

#include <Arduino.h>

namespace app {

void relay_init();
void relay_activate(uint8_t relay_id, uint32_t duration_ms);
void relay_set_state(uint8_t relay_id, bool enabled);

} // namespace app
