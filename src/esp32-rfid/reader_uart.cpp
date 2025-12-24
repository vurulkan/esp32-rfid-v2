#include "reader_uart.h"

#include <Arduino.h>

#include "messages.h"

namespace app {

namespace {
constexpr uint32_t kUartBaud = 115200;
constexpr uint8_t kUartRxPin = 33;
constexpr uint8_t kUartTxPin = 32;
constexpr TickType_t kPollDelay = pdMS_TO_TICKS(10);
volatile uint32_t g_last_pong_ms = 0;

void send_feedback(uint8_t reader_id, bool allowed) {
  char cmd = allowed ? 'A' : 'D';
  Serial2.print(cmd);
  Serial2.print(',');
  Serial2.print(reader_id);
  Serial2.print('\n');
}

bool parse_line(const char* line, RfidEvent* out) {
  if (!line || !out) {
    return false;
  }
  const char* p = line;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p == '\0') {
    return false;
  }
  if ((p[0] == 'P' || p[0] == 'p') &&
      (p[1] == 'O' || p[1] == 'o') &&
      (p[2] == 'N' || p[2] == 'n') &&
      (p[3] == 'G' || p[3] == 'g') &&
      (p[4] == '\0')) {
    g_last_pong_ms = millis();
    return false;
  }
  if (*p == 'R' || *p == 'r') {
    ++p;
  }
  int reader = 0;
  while (*p >= '0' && *p <= '9') {
    reader = reader * 10 + (*p - '0');
    ++p;
  }
  if (reader != 1 && reader != 2) {
    return false;
  }
  while (*p == ' ' || *p == '\t' || *p == ',' || *p == ':') {
    ++p;
  }
  if (*p == '\0') {
    return false;
  }
  char uid[kUidMaxLen] = {0};
  size_t idx = 0;
  while (*p && idx + 1 < sizeof(uid)) {
    char c = *p++;
    if (c == '\r' || c == '\n') {
      break;
    }
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    uid[idx++] = c;
  }
  uid[idx] = '\0';
  if (idx == 0) {
    return false;
  }
  out->reader_id = static_cast<uint8_t>(reader);
  strncpy(out->uid, uid, sizeof(out->uid) - 1);
  out->uid[sizeof(out->uid) - 1] = '\0';
  return true;
}

} // namespace

void reader_uart_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);
  Serial2.begin(kUartBaud, SERIAL_8N1, kUartRxPin, kUartTxPin);

  char line[64] = {0};
  size_t len = 0;

  for (;;) {
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
    while (Serial2.available() > 0) {
      char c = static_cast<char>(Serial2.read());
      if (c == '\r' || c == '\n') {
        if (len > 0) {
          line[len] = '\0';
          RfidEvent event{};
          if (parse_line(line, &event)) {
            xQueueSend(queues->rfid_queue, &event, 0);
          }
          len = 0;
        }
        continue;
      }
      if (len + 1 < sizeof(line)) {
        line[len++] = c;
      } else {
        len = 0;
      }
    }
    vTaskDelay(kPollDelay);
  }
}

uint32_t uart_last_pong_ms() {
  return g_last_pong_ms;
}

} // namespace app
