#include "reader_uart.h"

#include <Arduino.h>
#include <atomic>

#include "messages.h"
#include "validation.h"

namespace app {

namespace {
constexpr uint32_t kUartBaud = 115200;
constexpr uint8_t kUartRxPin = 33;
constexpr uint8_t kUartTxPin = 32;
constexpr TickType_t kPollDelay = pdMS_TO_TICKS(10);
std::atomic<uint32_t> g_rx_errors{0}, g_bad_lines{0}, g_cmd_drops{0};
std::atomic<uint32_t> g_last_pong_ms{0};
std::atomic<uint32_t> g_last_card_ms{0};
std::atomic<uint32_t> g_received_cards{0};
std::atomic<uint32_t> g_dropped_cards{0};

void send_feedback(uint8_t reader_id, bool allowed) {
  char cmd = allowed ? 'A' : 'D';
  Serial2.print(cmd);
  Serial2.print(',');
  Serial2.print(reader_id);
  Serial2.print('\n');
}

bool parse_line(const char* line, RfidEvent* out) {
  if (!line || !out) return false;
  if (strcmp(line, "PONG") == 0) {
    g_last_pong_ms = millis();
    return false;
  }
  if ((line[0] != '1' && line[0] != '2') || line[1] != ',') return false;
  size_t n = strlen(line + 2);
  if (n != 6 && n != 8) return false;
  if (!normalize_uid(line + 2, out->uid, sizeof(out->uid))) return false;
  out->reader_id = line[0] - '0';
  out->received_ms = millis();
  return true;
}

}  // namespace

void reader_uart_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);
  xEventGroupWaitBits(queues->events, kAppReady, pdFALSE, pdTRUE, portMAX_DELAY);
  Serial2.setRxBufferSize(512);
  Serial2.begin(kUartBaud, SERIAL_8N1, kUartRxPin, kUartTxPin);
  Serial2.onReceiveError([](hardwareSerial_error_t) { ++g_rx_errors; });

  char line[64] = {0};
  size_t len = 0;
  bool discard = false;
  uint32_t ping_at = millis() - 5000;

  for (;;) {
    if (elapsed(millis(), ping_at, 5000)) {
      Serial2.print("PING\n");
      ping_at = millis();
    }
    if (queues && queues->uart_cmd_queue) {
      UartCmd cmd{};
      while (xQueueReceive(queues->uart_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd.type == UartCmd::Type::Ping) {
          Serial2.print("PING\n");
          continue;
        }
        if (cmd.type == UartCmd::Type::Feedback) {
          if (cmd.reader_id == 1 || cmd.reader_id == 2) {
            send_feedback(cmd.reader_id, cmd.allowed != 0);
          }
        }
      }
    }
    size_t budget = 256;
    while (budget-- && Serial2.available() > 0) {
      char c = static_cast<char>(Serial2.read());
      if (c == '\r' || c == '\n') {
        if (discard) {
          discard = false;
          len = 0;
          continue;
        }
        if (len > 0) {
          line[len] = '\0';
          RfidEvent event{};
          if (parse_line(line, &event)) {
            g_last_card_ms = millis();
            ++g_received_cards;
            if (app_maintenance() || xQueueSend(queues->rfid_queue, &event, 0) != pdTRUE) {
              ++g_dropped_cards;
            }
          } else if (strcmp(line, "PONG") != 0)
            ++g_bad_lines;
          len = 0;
        }
        continue;
      }
      if (discard) continue;
      if (len + 1 < sizeof(line)) {
        line[len++] = c;
      } else {
        len = 0;
        discard = true;
        ++g_bad_lines;
      }
    }
    vTaskDelay(kPollDelay);
  }
}

uint32_t uart_last_pong_ms() { return g_last_pong_ms.load(); }

uint32_t uart_last_card_ms() { return g_last_card_ms.load(); }
uint32_t uart_received_cards() { return g_received_cards.load(); }
uint32_t uart_dropped_cards() { return g_dropped_cards.load(); }

uint32_t uart_receive_errors() { return g_rx_errors; }
uint32_t uart_bad_lines() { return g_bad_lines; }
uint32_t uart_cmd_drops() { return g_cmd_drops; }
void uart_note_cmd_drop() { ++g_cmd_drops; }
}  // namespace app
