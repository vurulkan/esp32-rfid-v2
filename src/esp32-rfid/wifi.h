#pragma once

#include <Arduino.h>

namespace app {

void wifi_task(void* param);
bool wifi_is_ap_ready();
bool wifi_is_sta_ready();

} // namespace app
