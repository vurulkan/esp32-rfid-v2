#pragma once

#include "app_context.h"

namespace app {

uint32_t uart_receive_errors();
uint32_t uart_bad_lines();
uint32_t uart_cmd_drops();
void uart_note_cmd_drop();
void reader_uart_task(void* param);
uint32_t uart_last_pong_ms();
uint32_t uart_last_card_ms();
uint32_t uart_received_cards();
uint32_t uart_dropped_cards();

}  // namespace app
