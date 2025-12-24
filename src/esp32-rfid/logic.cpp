#include "logic.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "log.h"
#include "messages.h"
#include "relay.h"
#include "settings.h"
#include "rtc.h"
#include "users.h"

namespace app {

namespace {
constexpr uint32_t kRelayPulseMs = 600;

struct LastRfidState {
  uint8_t reader_id = 0;
  char uid[kUidMaxLen] = {0};
  bool allowed = false;
  uint32_t ts_ms = 0;
};

void sanitize_csv_field(const char* src, char* dest, size_t dest_len) {
  if (!dest || dest_len == 0) {
    return;
  }
  if (!src) {
    dest[0] = '\0';
    return;
  }
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < dest_len; ++i) {
    char c = src[i];
    if (c == ',' || c == '\r' || c == '\n') {
      c = ' ';
    }
    dest[out++] = c;
  }
  dest[out] = '\0';
}

void send_response(QueueHandle_t reply, bool ok, const String& json) {
  if (reply == nullptr) {
    return;
  }
  static LogicResponse resp;
  memset(&resp, 0, sizeof(resp));
  resp.ok = ok ? 1 : 0;
  strncpy(resp.json, json.c_str(), sizeof(resp.json) - 1);
  resp.json[sizeof(resp.json) - 1] = '\0';
  xQueueSend(reply, &resp, pdMS_TO_TICKS(100));
}

void send_response_cstr(QueueHandle_t reply, bool ok, const char* json) {
  if (reply == nullptr) {
    return;
  }
  static LogicResponse resp;
  memset(&resp, 0, sizeof(resp));
  resp.ok = ok ? 1 : 0;
  if (json) {
    strncpy(resp.json, json, sizeof(resp.json) - 1);
    resp.json[sizeof(resp.json) - 1] = '\0';
  } else {
    resp.json[0] = '\0';
  }
  xQueueSend(reply, &resp, pdMS_TO_TICKS(100));
}

void send_uart_feedback(AppQueues* queues, uint8_t reader_id, bool allowed) {
  if (!queues || !queues->uart_cmd_queue) {
    return;
  }
  UartCmd cmd{};
  cmd.type = UartCmd::Type::Feedback;
  cmd.reader_id = reader_id;
  cmd.allowed = allowed ? 1 : 0;
  xQueueSend(queues->uart_cmd_queue, &cmd, 0);
}

} // namespace

void logic_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);

  static UsersDb users;
  static LogBuffer logs;
  static LastRfidState last_rfid;

  users.init();
  logs.init();
  users.load();
  logs.load();
  relay_init();
  auto settings = settings_get();
  relay_set_state(1, settings.relay1_state);
  relay_set_state(2, settings.relay2_state);

  QueueSetHandle_t set = xQueueCreateSet(16);
  xQueueAddToSet(queues->rfid_queue, set);
  xQueueAddToSet(queues->logic_queue, set);

  for (;;) {
    QueueSetMemberHandle_t active = xQueueSelectFromSet(set, pdMS_TO_TICKS(200));
    if (active == nullptr) {
      continue;
    }

    if (active == queues->rfid_queue) {
      RfidEvent event{};
      if (xQueueReceive(queues->rfid_queue, &event, 0) == pdTRUE) {
        const uint8_t relay_id = event.reader_id;
        bool allowed = users.authorized(event.uid, relay_id);
        UserRecord user{};
        bool has_user = users.get_user(event.uid, &user);

        last_rfid.reader_id = relay_id;
        strncpy(last_rfid.uid, event.uid, sizeof(last_rfid.uid) - 1);
        last_rfid.uid[sizeof(last_rfid.uid) - 1] = '\0';
        last_rfid.allowed = allowed;
        last_rfid.ts_ms = millis();

        const char* relay_name = (relay_id == 1) ? settings_get().relay1_name : settings_get().relay2_name;
        char relay_field[32];
        char uid_field[24];
        char name_field[40];
        sanitize_csv_field(relay_name, relay_field, sizeof(relay_field));
        sanitize_csv_field(event.uid, uid_field, sizeof(uid_field));
        sanitize_csv_field(user.name, name_field, sizeof(name_field));

        const char* status = allowed ? "granted" : "denied";
        char base_msg[120];
        if (allowed && has_user) {
          snprintf(base_msg, sizeof(base_msg), "%s,%s,%s,%s",
                   relay_field, status, uid_field, name_field);
        } else {
          snprintf(base_msg, sizeof(base_msg), "%s,%s,%s,",
                   relay_field, status, uid_field);
        }

        char log_msg[160];
        if (rtc_has_valid_time()) {
          RtcDateTime dt{};
          if (rtc_get_datetime(&dt)) {
            snprintf(log_msg, sizeof(log_msg), "%02u/%02u/%04u,%02u:%02u:%02u,%s",
                     dt.day, dt.month, dt.year, dt.hour, dt.minute, dt.second, base_msg);
          } else {
            snprintf(log_msg, sizeof(log_msg), "%s", base_msg);
          }
        } else {
          snprintf(log_msg, sizeof(log_msg), "%s", base_msg);
        }
        logs.add(log_msg, last_rfid.ts_ms);
        
        if (!allowed) {
          send_uart_feedback(queues, relay_id, false);
        }

        if (allowed) {
          relay_activate(relay_id, kRelayPulseMs);
        }
      }
      continue;
    }

    if (active == queues->logic_queue) {
      LogicRequest req{};
      if (xQueueReceive(queues->logic_queue, &req, 0) != pdTRUE) {
        continue;
      }

      switch (req.type) {
        case LogicRequestType::GetUsers: {
          String json = users.to_json();
          send_response(req.reply_queue, true, json);
          break;
        }
        case LogicRequestType::AddUser: {
          UserRecord existing{};
          bool exists = users.get_user(req.payload.add_user.uid, &existing);
          if (exists) {
            send_response_cstr(req.reply_queue, false, "{\"ok\":false,\"error\":\"uid_exists\"}");
            break;
          }
          bool ok = users.add_user(req.payload.add_user.uid,
                                   req.payload.add_user.name,
                                   req.payload.add_user.relay1 != 0,
                                   req.payload.add_user.relay2 != 0);
          send_response_cstr(req.reply_queue, ok, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"save_failed\"}");
          break;
        }
        case LogicRequestType::DeleteUser: {
          bool ok = users.remove(req.payload.del_user.uid);
          send_response_cstr(req.reply_queue, ok, ok ? "{\"ok\":true}" : "{\"ok\":false}");
          break;
        }
        case LogicRequestType::GetLogs: {
          String json = logs.to_json();
          send_response(req.reply_queue, true, json);
          break;
        }
        case LogicRequestType::ReloadUsers: {
          bool ok = users.load();
          send_response_cstr(req.reply_queue, ok, ok ? "{\"ok\":true}" : "{\"ok\":false}");
          break;
        }
        case LogicRequestType::ClearLogsRam: {
          logs.clear_ram();
          send_response_cstr(req.reply_queue, true, "{\"ok\":true}");
          break;
        }
        case LogicRequestType::ClearLogsAll: {
          logs.clear_all();
          send_response_cstr(req.reply_queue, true, "{\"ok\":true}");
          break;
        }
        case LogicRequestType::GetLastRfid: {
          String json = "{\"rfid\":{\"reader\":";
          json += last_rfid.reader_id;
          json += ",\"uid\":\"";
          json += last_rfid.uid;
          json += "\",\"allowed\":";
          json += (last_rfid.allowed ? "true" : "false");
          json += ",\"ts\":";
          json += last_rfid.ts_ms;
          json += "}}";
          send_response(req.reply_queue, true, json);
          break;
        }
        case LogicRequestType::TriggerRelay: {
          uint8_t relay_id = req.payload.trigger_relay.relay_id;
          if (relay_id == 1 || relay_id == 2) {
            uint32_t duration = req.payload.trigger_relay.duration_ms;
            if (duration == 0) {
              duration = kRelayPulseMs;
            }
            relay_activate(relay_id, duration);
            send_response_cstr(req.reply_queue, true, "{\"ok\":true}");
          } else {
            send_response_cstr(req.reply_queue, false, "{\"ok\":false,\"error\":\"invalid_relay\"}");
          }
          break;
        }
        case LogicRequestType::SetRelayState: {
          uint8_t relay_id = req.payload.relay_state.relay_id;
          bool enabled = req.payload.relay_state.enabled != 0;
          if (relay_id == 1 || relay_id == 2) {
            relay_set_state(relay_id, enabled);
            settings_set_relay_state(relay_id, enabled);
            send_response_cstr(req.reply_queue, true, "{\"ok\":true}");
          } else {
            send_response_cstr(req.reply_queue, false, "{\"ok\":false,\"error\":\"invalid_relay\"}");
          }
          break;
        }
        default:
          send_response_cstr(req.reply_queue, false, "{\"ok\":false}");
          break;
      }
    }
  }
}

} // namespace app
