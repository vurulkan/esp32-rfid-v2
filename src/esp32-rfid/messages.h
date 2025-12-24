#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace app {

constexpr size_t kUidMaxLen = 20;
constexpr size_t kNameMaxLen = 32;
constexpr size_t kLogicResponseMax = 6144;

struct RfidEvent {
  uint8_t reader_id; // 1 or 2
  char uid[kUidMaxLen];
};

struct UartCmd {
  enum class Type : uint8_t {
    Feedback = 0,
    Ping = 1
  };
  Type type;
  uint8_t reader_id;
  uint8_t allowed;
};

enum class LogicRequestType : uint8_t {
  GetUsers,
  AddUser,
  DeleteUser,
  GetLogs,
  ClearLogsRam,
  ClearLogsAll,
  GetLastRfid,
  ReloadUsers,
  TriggerRelay,
  SetRelayState
};

struct LogicRequest {
  LogicRequestType type;
  QueueHandle_t reply_queue;
  union {
    struct {
      char uid[kUidMaxLen];
      char name[kNameMaxLen];
      uint8_t relay1;
      uint8_t relay2;
    } add_user;
    struct {
      char uid[kUidMaxLen];
    } del_user;
    struct {
      uint8_t relay_id;
      uint32_t duration_ms;
    } trigger_relay;
    struct {
      uint8_t relay_id;
      uint8_t enabled;
    } relay_state;
  } payload;
};

struct LogicResponse {
  uint8_t ok;
  char json[kLogicResponseMax];
};

} // namespace app
