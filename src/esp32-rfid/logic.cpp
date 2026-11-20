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
#include "validation.h"
#include "reader_uart.h"
#include <atomic>

namespace app {

namespace {
std::atomic<uint32_t> heartbeat{0}, max_delay{0}, processed{0}, expired{0};
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

void send_response(const LogicRequest& req, bool ok, const String& json) {
  if (req.reply_queue == nullptr) {
    return;
  }
  static LogicResponse resp;
  memset(&resp, 0, sizeof(resp));
  resp.request_id = req.request_id;
  resp.ok = ok ? 1 : 0;
  if (json.length() >= sizeof(resp.json)) {
    resp.ok = false;
    strcpy(resp.json, "{\"ok\":false,\"error\":\"response_too_large\"}");
  } else
    strncpy(resp.json, json.c_str(), sizeof(resp.json) - 1);
  resp.json[sizeof(resp.json) - 1] = '\0';
  xQueueOverwrite(req.reply_queue, &resp);
}

void send_response_cstr(const LogicRequest& req, bool ok, const char* json) {
  if (req.reply_queue == nullptr) {
    return;
  }
  static LogicResponse resp;
  memset(&resp, 0, sizeof(resp));
  resp.request_id = req.request_id;
  resp.ok = ok ? 1 : 0;
  if (json) {
    strncpy(resp.json, json, sizeof(resp.json) - 1);
    resp.json[sizeof(resp.json) - 1] = '\0';
  } else {
    resp.json[0] = '\0';
  }
  xQueueOverwrite(req.reply_queue, &resp);
}

void send_uart_feedback(AppQueues* queues, uint8_t reader_id, bool allowed) {
  if (!queues || !queues->uart_cmd_queue) {
    return;
  }
  UartCmd cmd{};
  cmd.type = UartCmd::Type::Feedback;
  cmd.reader_id = reader_id;
  cmd.allowed = allowed ? 1 : 0;
  if (xQueueSend(queues->uart_cmd_queue, &cmd, 0) != pdTRUE) uart_note_cmd_drop();
}

}  // namespace

LogicHealth logic_health() {
  return {heartbeat.load(), max_delay.load(), processed.load(), expired.load()};
}
void logic_task(void* param) {
  auto* queues = static_cast<AppQueues*>(param);

  auto& users = users_database();
  static LogBuffer logs;
  static LastRfidState last_rfid;

  users.init();
  logs.init();
  users.load();
  auto settings = settings_get();
  relay_set_state(1, settings.relay1_state);
  relay_set_state(2, settings.relay2_state);
  if (logs.load())
    logs.start_writer();
  else
    Serial.println("Log startup failed; persistent logging disabled, RAM logging remains active.");

  QueueSetHandle_t set = queues->queue_set;
  xEventGroupSetBits(queues->events, kAppReady);

  for (;;) {
    heartbeat = millis();
    if (app_maintenance())
      xEventGroupSetBits(queues->events, kLogicIdle);
    else
      xEventGroupClearBits(queues->events, kLogicIdle);
    QueueSetMemberHandle_t active = xQueueSelectFromSet(set, pdMS_TO_TICKS(200));
    if (active == nullptr) {
      continue;
    }

    if (active == queues->rfid_queue) {
      RfidEvent event{};
      if (xQueueReceive(queues->rfid_queue, &event, 0) == pdTRUE) {
        uint32_t delay = millis() - event.received_ms;
        if (delay > max_delay) max_delay = delay;
        if (app_maintenance() || delay >= 2000) {
          ++expired;
          continue;
        }
        ++processed;
        const uint8_t relay_id = event.reader_id;
        bool allowed = relay_ready() && users.authorized(event.uid, relay_id);
        UserRecord user{};
        bool has_user = users.get_user(event.uid, &user);

        last_rfid.reader_id = relay_id;
        strncpy(last_rfid.uid, event.uid, sizeof(last_rfid.uid) - 1);
        last_rfid.uid[sizeof(last_rfid.uid) - 1] = '\0';
        last_rfid.allowed = allowed;
        last_rfid.ts_ms = millis();

        const auto current_settings = settings_get();
        if (allowed) {
          allowed = relay_activate(relay_id, current_settings.relay_pulse_ms);
          last_rfid.allowed = allowed;
        }
        // Readers already beep when a card is read. Preserve that single beep
        // on success; only denied/failed access needs an extra warning.
        if (!allowed) {
          send_uart_feedback(queues, relay_id, false);
        }

        const char* relay_name =
            (relay_id == 1) ? current_settings.relay1_name : current_settings.relay2_name;
        char relay_field[32];
        char uid_field[24];
        char name_field[40];
        sanitize_csv_field(relay_name, relay_field, sizeof(relay_field));
        sanitize_csv_field(event.uid, uid_field, sizeof(uid_field));
        sanitize_csv_field(user.name, name_field, sizeof(name_field));

        const char* status = allowed ? "granted" : "denied";
        char base_msg[120];
        if (allowed && has_user) {
          snprintf(base_msg, sizeof(base_msg), "%s,%s,%s,%s", relay_field, status, uid_field,
                   name_field);
        } else {
          snprintf(base_msg, sizeof(base_msg), "%s,%s,%s,", relay_field, status, uid_field);
        }

        char log_msg[160];
        RtcDateTime dt{};
        if (rtc_get_datetime(&dt)) {
          snprintf(log_msg, sizeof(log_msg), "%02u/%02u/%04u,%02u:%02u:%02u,%s", dt.day, dt.month,
                   dt.year, dt.hour, dt.minute, dt.second, base_msg);
        } else {
          snprintf(log_msg, sizeof(log_msg), "%s", base_msg);
        }
        logs.add(log_msg, last_rfid.ts_ms);
      }
      continue;
    }

    if (active == queues->logic_queue) {
      LogicRequest req{};
      if (xQueueReceive(queues->logic_queue, &req, 0) != pdTRUE) {
        continue;
      }

      if (app_maintenance() || int32_t(millis() - req.deadline_ms) >= 0) {
        send_response_cstr(req, false, "{\"ok\":false,\"error\":\"maintenance\"}");
        continue;
      }
      switch (req.type) {
        case LogicRequestType::GetLogs: {
          String json = logs.to_json();
          send_response(req, true, json);
          break;
        }
        case LogicRequestType::ClearLogsRam: {
          logs.clear_ram();
          send_response_cstr(req, true, "{\"ok\":true}");
          break;
        }
        case LogicRequestType::ClearLogsAll: {
          bool ok = logs.clear_all();
          send_response_cstr(req, ok,
                             ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"log_queue_full\"}");
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
          send_response(req, true, json);
          break;
        }
        case LogicRequestType::TriggerRelay: {
          uint8_t relay_id = req.payload.trigger_relay.relay_id;
          if (relay_id == 1 || relay_id == 2) {
            uint32_t duration = req.payload.trigger_relay.duration_ms;
            if (duration == 0) {
              duration = settings_get().relay_pulse_ms;
            }
            bool ok = relay_activate(relay_id, duration);
            send_response_cstr(req, ok, ok ? "{\"ok\":true}" : "{\"ok\":false}");
          } else {
            send_response_cstr(req, false, "{\"ok\":false,\"error\":\"invalid_relay\"}");
          }
          break;
        }
        case LogicRequestType::SetRelayState: {
          uint8_t relay_id = req.payload.relay_state.relay_id;
          bool enabled = req.payload.relay_state.enabled != 0;
          if (relay_id == 1 || relay_id == 2) {
            bool ok = relay_set_state(relay_id, enabled);
            send_response_cstr(req, ok, ok ? "{\"ok\":true}" : "{\"ok\":false}");
          } else {
            send_response_cstr(req, false, "{\"ok\":false,\"error\":\"invalid_relay\"}");
          }
          break;
        }
        default:
          send_response_cstr(req, false, "{\"ok\":false}");
          break;
      }
    }
  }
}

}  // namespace app
