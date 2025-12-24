#pragma once

#include "app_context.h"

namespace app {

void reader_uart_task(void* param);
uint32_t uart_last_pong_ms();

} // namespace app
