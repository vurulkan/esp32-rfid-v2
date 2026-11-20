// Real service implementations run against POSIX temporary files and fake RTOS/GPIO.
#include <Arduino.h>
#include <LittleFS.h>
#include "freertos/queue.h"
#include <cassert>
#include <fstream>
#include <thread>
namespace host {
uint32_t now = 100;
int pins[64]{}, modes[64]{};
uint32_t now_us = 0, stop_at = UINT32_MAX;
int atomic_depth = 0;
bool require_atomic = false;
std::function<void()> wait_hook;
bool task_ok = true, stop_empty = false;
std::vector<std::pair<TaskFunction_t, void*>> tasks;
}  // namespace host
FakeSerial Serial, Serial2;
FakeFS LittleFS;

bool fail_sync = false, fail_rename = false, skip_sync = false;
int rename_attempt = 0, fail_rename_at = 0;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) { return fail_sync ? -1 : (skip_sync ? 0 : __real_fsync(fd)); }
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* a, const char* b) {
  ++rename_attempt;
  return fail_rename || (fail_rename_at && rename_attempt == fail_rename_at) ? -1
                                                                             : __real_rename(a, b);
}
#include "storage.cpp"
#include "settings.cpp"
#include "users.cpp"
#include "relay.cpp"
#include "log.cpp"
#include "reader_uart.cpp"
#include "wifi.cpp"
#include "logic.cpp"
HostWiFi WiFi;
#include <map>
#include <random>
struct WebServer {
  std::map<std::string, String> headers, args;
  String cookie;
  String header(const char* key) { return headers[key]; }
  bool hasArg(const char* key) { return args.count(key); }
  String arg(const char* key) { return args[key]; }
  void sendHeader(const char*, const String& value) { cookie = value; }
};
uint64_t clock64 = 1000;
int64_t esp_timer_get_time() { return clock64 * 1000; }
uint32_t esp_random() {
  static std::mt19937 gen(1);
  return gen();
}
namespace app {
static AppQueues g_queues{};
static std::atomic<bool> g_maintenance{false};
#include "maintenance_policy.inc"
bool rtc_init(bool) { return true; }
bool verify_relay_before_rtc = false;
unsigned rtc_reads = 0;
bool rtc_get_datetime(RtcDateTime*) {
  ++rtc_reads;
  if (verify_relay_before_rtc) assert(host::pins[16] == HIGH);
  return false;
}
#include "auth_policy.inc"
}  // namespace app
using namespace app;
void seed(const char* path, const std::string& value) {
  std::ofstream f(std::string(RFID_FS_ROOT) + path, std::ios::binary);
  f << value;
}
std::string read(const char* path) {
  std::ifstream f(std::string(RFID_FS_ROOT) + path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void atomic_storage_preserves_old_data() {
  assert(storage_init());
  seed("/users.txt", "old\n");
  assert(!storage_atomic_write("/users.txt", [](FILE* f) {
    fputs("bad", f);
    return false;
  }));
  assert(read("/users.txt") == "old\n");
  fail_sync = true;
  assert(!storage_write_text("/users.txt", "new"));
  assert(read("/users.txt") == "old\n");
  fail_sync = false;
  fail_rename = true;
  assert(!storage_write_text("/users.txt", "new"));
  assert(read("/users.txt") == "old\n");
  fail_rename = false;
  assert(storage_write_text("/users.txt", "new\n"));
  assert(read("/users.txt") == "new\n");
  assert(!LittleFS.exists("/users.txt.tmp"));
  assert(LittleFS.begins == 1);
}
void settings_validate_and_rollback() {
  assert(storage_init());
  settings_init();
  assert(settings_load());
  assert(settings_set_wifi(true, "Test", "  p|ass  "));
  auto before = settings_get();
  auto old = read("/settings.txt");
  fail_sync = true;
  assert(!settings_set_relay_pulse_ms(999));
  assert(settings_get().relay_pulse_ms == before.relay_pulse_ms);
  assert(read("/settings.txt") == old);
  fail_sync = false;
  assert(!settings_set_auth(true, "", "", "KEY"));
  assert(!settings_get().auth_enabled);
  assert(!settings_set_relay_pulse_ms(49));
  assert(!settings_set_relay_pulse_ms(10001));
  assert(settings_set_relay_pulse_ms(50));
  assert(settings_set_relay_pulse_ms(10000));
  settings_init();
  assert(settings_load());
  assert(strcmp(settings_get().wifi_pass, "  p|ass  ") == 0);
  assert(settings_set_relay_names("First", nullptr));
  assert(strcmp(settings_get().relay2_name, "Relay 2") == 0);
  Settings candidate = settings_get();
  assert(!settings_parse_text("auth_enabled=1\n", &candidate));
  candidate = settings_get();
  assert(!settings_parse_text("rtc=1\nrtc=0\n", &candidate));
  assert(!settings_set_wifi_static(true, "bad", "1.2.3.1", "255.255.255.0"));
  assert(!settings_set_wifi_static(true, "1.2.3.4", "1.2.3.1", "255.0.255.0"));
  assert(LittleFS.begins == 1);
}
void corrupt_settings_fail_closed() {
  assert(storage_init());
  settings_init();
  seed("/settings.txt", "rtc=1\n");
  assert(!settings_load());
  assert(!settings_ready());
  assert(settings_get().auth_enabled);
}
void users_restore_replaces_and_validates() {
  assert(storage_init());
  UsersDb db;
  db.init();
  assert(db.load());
  assert(db.add_user("abcdef", "Old", true, false));
  assert(db.add_user("123456", "Removed", true, true));
  seed("/restore", "ABCDEF|Changed|0|1\n");
  assert(db.restore_file("/restore"));
  assert(!db.authorized("ABCDEF", 1));
  assert(db.authorized("abcdef", 2));
  assert(!db.authorized("123456", 1));
  auto old = read("/users.txt");
  seed("/restore", "ABCDEF|Bad|1|1|1\n");
  assert(!db.restore_file("/restore"));
  assert(read("/users.txt") == old);
  assert(db.authorized("ABCDEF", 2));
  seed("/restore", "ABCDEF|A|0|1\nabcdef|B|1|0\n");
  assert(!db.restore_file("/restore"));
  std::string malformed = "ABCDEF|A|0|1";
  malformed.push_back(0);
  malformed += "junk\n";
  seed("/restore", malformed);
  assert(!db.restore_file("/restore"));
  seed("/restore", "");
  assert(db.restore_file("/restore"));
  assert(!db.authorized("ABCDEF", 2));
  seed("/restore", "#RFID_BACKUP\n[users]\nABCDEF|Again|1|0\n[/users]\n");
  assert(db.restore_file("/restore"));
  assert(db.authorized("ABCDEF", 1));
}
void users_failed_save_preserves_permissions() {
  assert(storage_init());
  UsersDb db;
  db.init();
  assert(db.load());
  assert(db.add_user("ABCDEF", "Original", true, false));
  auto old = read("/users.txt");
  fail_sync = true;
  assert(!db.remove("ABCDEF"));
  assert(db.authorized("ABCDEF", 1));
  assert(!db.add_user("123456", "New", true, true));
  assert(!db.authorized("123456", 1));
  assert(read("/users.txt") == old);
  fail_sync = false;
  assert(!db.add_user("123456", "Ali|1|1", false, false));
  assert(!db.add_user("xyz", "Bad", true, true));
  assert(!db.add_user("12345678901234567890", "Long", true, true));
  assert(db.remove("ABCDEF"));
  assert(!db.authorized("ABCDEF", 1));
}
void users_pages_escape_and_fit() {
  assert(storage_init());
  UsersDb db;
  db.init();
  seed("/restore", "");
  std::ofstream f(std::string(RFID_FS_ROOT) + "/restore");
  for (int i = 0; i < 130; ++i) {
    char uid[16];
    snprintf(uid, sizeof(uid), "%08X", i);
    f << uid << "|Name \\\"" << std::string(20, 'X') << "|0|0\n";
  }
  f.close();
  assert(db.restore_file("/restore"));
  size_t total = 0, offset = 0;
  while (offset < 130) {
    String json = db.to_json_page(offset, 100, total);
    assert(total == 130);
    assert(json.length() < kLogicResponseMax);
    assert(json.indexOf("\\\"") >= 0);
    assert(json.endsWith("}"));
    puts(json.c_str());
    size_t n = 0;
    int pos = 0;
    while ((pos = json.indexOf("{\"uid\":", pos)) >= 0) {
      ++n;
      ++pos;
    }
    assert(n > 0);
    offset += n;
  }
}
void relay_pulse_manual_and_wrap() {
  assert(relay_init());
  assert(relay_activate(1, 100));
  assert(host::pins[16] == HIGH);
  update_outputs(199);
  assert(host::pins[16] == HIGH);
  update_outputs(200);
  assert(host::pins[16] == LOW);
  host::now = 300;
  assert(relay_activate(1, 100));
  assert(relay_set_state(1, true));
  update_outputs(1000);
  assert(host::pins[16] == HIGH);
  assert(relay_set_state(1, false));
  assert(host::pins[16] == LOW);
  assert(!relay_activate(1, 49));
  assert(!relay_activate(3, 100));
  host::now = 0xfffffff0u;
  assert(relay_activate(2, 80));
  update_outputs(63);
  assert(host::pins[17] == HIGH);
  update_outputs(64);
  assert(host::pins[17] == LOW);
}
void relay_startup_failure_stays_off() {
  host::task_ok = false;
  assert(!relay_init());
  assert(!relay_activate(1, 100));
  assert(host::pins[16] == LOW && host::pins[17] == LOW);
}
void log_rotation_preserves_archive_and_reserve() {
  assert(storage_init());
  seed("/logs.txt", "1,Relay,granted,ABCDEF,Legacy\n");
  LogBuffer logs;
  logs.init();
  assert(logs.load());
  assert(read("/logs-legacy.txt").find("Legacy") != std::string::npos);
  Command c{};
  c.type = Command::Append;
  strcpy(c.entry.msg, "Relay,granted,ABCDEF,Name");
  skip_sync = true;
  for (int i = 0; i < 15000; ++i) {
    c.entry.ts_ms = i;
    assert(append(c));
  }
  assert(LittleFS.totalBytes() - LittleFS.usedBytes() >= kReserve);
  assert(LittleFS.exists("/logs-legacy.txt"));
  assert(segments.size() < 33);
  auto before = read("/logs-legacy.txt");
  LittleFS.capacity = kReserve + before.length();
  assert(!append(c));
  assert(read("/logs-legacy.txt") == before);
}
void log_queue_orders_clear_and_append() {
  assert(storage_init());
  LogBuffer logs;
  logs.init();
  assert(logs.load());
  assert(logs.start_writer());
  logs.add("before", 1);
  assert(logs.clear_all());
  logs.add("after", 2);
  host::stop_empty = true;
  try {
    host::tasks.back().first(host::tasks.back().second);
  } catch (host::Stop&) {
  }
  host::stop_empty = false;
  assert(read("/logs.txt") == "2,after\n");
  assert(log_write_errors() == 0);
}
void uart_rejects_malformed_frames() {
  RfidEvent event{};
  assert(parse_line("1,abcdef", &event));
  assert(strcmp(event.uid, "ABCDEF") == 0 && event.reader_id == 1);
  assert(event.received_ms == host::now);
  assert(parse_line("2,001234AB", &event));
  for (const char* bad : {"1ABCDEF", "3,ABCDEF", "999999999999999999999,ABCDEF", "1,ABCDEX",
                          "1,12345", "1,123456789", "1,ABCDEF trailing", ""})
    assert(!parse_line(bad, &event));
  assert(!parse_line("PONG", &event));
  assert(uart_last_pong_ms() == host::now);
}
void restore_bundle_rolls_back_both_files() {
  assert(storage_init());
  settings_init();
  assert(settings_load());
  assert(settings_set_relay_names("Original", nullptr));
  UsersDb db;
  db.init();
  assert(db.load());
  assert(db.add_user("ABCDEF", "Original", true, false));
  auto old_users = read("/users.txt"), old_settings = read("/settings.txt");
  Settings candidate = settings_get();
  strcpy(candidate.relay1_name, "Restored");
  seed("/restore", "123456|New|0|1\n");
  // Fail the second installed file, after both originals were moved to rollback copies.
  rename_attempt = 0;
  fail_rename_at = 7;
  assert(!db.restore_file("/restore", &candidate));
  fail_rename_at = 0;
  assert(read("/users.txt") == old_users && read("/settings.txt") == old_settings);
  assert(db.authorized("ABCDEF", 1) && !db.authorized("123456", 2));
  assert(strcmp(settings_get().relay1_name, "Original") == 0);
  assert(db.restore_file("/restore", &candidate));
  assert(!db.authorized("ABCDEF", 1) && db.authorized("123456", 2));
  assert(strcmp(settings_get().relay1_name, "Restored") == 0);
  assert(!LittleFS.exists("/restore.pending"));
  Settings from_backup = settings_get();
  strcpy(from_backup.relay1_name, "Multipart");
  seed("/restore", std::string("#RFID_BACKUP\n[settings]\n") +
                       settings_encode(from_backup).c_str() +
                       "[/settings]\n[users]\n654321|Multipart user|1|0\n[/users]\n");
  assert(db.restore_file("/restore"));
  assert(db.authorized("654321", 1));
  assert(!db.authorized("123456", 2));
  assert(strcmp(settings_get().relay1_name, "Multipart") == 0);
}
void interrupted_bundle_recovers_on_boot() {
  assert(storage_init());
  seed("/users.txt", "NEW");
  seed("/settings.txt", "NEWSET");
  seed("/users.rollback", "OLD");
  seed("/settings.rollback", "OLDSET");
  seed("/restore.pending", "1 1 1");
  assert(storage_init());
  assert(read("/users.txt") == "OLD" && read("/settings.txt") == "OLDSET");
  assert(!LittleFS.exists("/restore.pending"));
}
void auth_rejects_bad_config_and_expires_sessions() {
  assert(storage_init());
  settings_init();
  assert(settings_load());
  WebServer s;
  assert(check_auth(s));
  assert(settings_set_auth(true, "admin", "secret", "KEY"));
  assert(!check_auth(s));
  s.headers["X-API-Key"] = "WRONG";
  assert(!check_auth(s));
  s.headers["X-API-Key"] = "KEY";
  assert(check_auth(s));
  s.headers.clear();
  clock64 = 0xfffffff0ULL;
  issue_session(s);
  s.headers["Cookie"] = s.cookie;
  assert(check_session(s, false));
  clock64 += 299999;
  assert(check_session(s, false));
  ++clock64;
  assert(!check_session(s, false));
  issue_session(s);
  s.headers["Cookie"] = s.cookie;
  assert(check_session(s, false));
  assert(settings_set_auth(true, "admin", "changed", "KEY"));
  assert(!check_session(s, false));
  Settings bad = settings_get();
  bad.auth_pass[0] = 0;
  settings_publish_committed(bad);
  s.headers["X-API-Key"] = "KEY";
  assert(!check_auth(s));
  settings_init();
  assert(!check_auth(s));
}
void maintenance_requires_fresh_idle_ack() {
  g_queues.events = new HostEvents{kLogicIdle};
  host::wait_hook = {};
  assert(!app_prepare_maintenance(1));
  assert(!app_maintenance());
  host::wait_hook = []() { g_queues.events->bits |= kLogicIdle; };
  assert(app_prepare_maintenance(1));
  assert(app_maintenance());
  assert(!app_prepare_maintenance(1));
  assert(app_maintenance());
  app_set_maintenance(false);
  assert(!(g_queues.events->bits & kLogicIdle));
}
void wifi_starts_ap_and_recovers_missing_station() {
  assert(storage_init());
  settings_init();
  assert(settings_load());
  host::stop_at = 2000;
  try {
    wifi_task(nullptr);
  } catch (host::Stop&) {
  }
  assert(WiFi.mode_ == WIFI_AP && WiFi.ap_calls == 1 && wifi_is_ap_ready());
  assert(settings_set_wifi(true, "Missing", "password"));
  host::now = 0;
  host::stop_at = 35000;
  try {
    wifi_task(nullptr);
  } catch (host::Stop&) {
  }
  assert(WiFi.begin_calls == 1 && WiFi.reconnect && WiFi.mode_ == WIFI_AP_STA &&
         WiFi.ap_calls == 2);
  WiFi.callback(ARDUINO_EVENT_WIFI_STA_GOT_IP);
  assert(wifi_is_sta_ready());
  WiFi.callback(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  assert(!wifi_is_sta_ready());
}
void setup_decision_queues() {
  assert(storage_init());
  settings_init();
  assert(settings_load());
  seed("/users.txt", "ABCDEF|Allowed|1|0\n");
  assert(relay_init());
  g_queues.events = new HostEvents;
  g_queues.rfid_queue = xQueueCreate(8, sizeof(RfidEvent));
  g_queues.logic_queue = xQueueCreate(8, sizeof(LogicRequest));
  g_queues.uart_cmd_queue = xQueueCreate(8, sizeof(UartCmd));
  g_queues.queue_set = xQueueCreateSet(16);
  assert(xQueueAddToSet(g_queues.rfid_queue, g_queues.queue_set));
  assert(xQueueAddToSet(g_queues.logic_queue, g_queues.queue_set));
  host::stop_empty = true;
}
void run_decision_until_empty() {
  try {
    logic_task(&g_queues);
  } catch (host::Stop&) {
  }
  host::stop_empty = false;
}
void decision_relay_precedes_rtc_without_extra_beep() {
  setup_decision_queues();
  host::now = 3000;
  RfidEvent event{};
  event.reader_id = 1;
  strcpy(event.uid, "ABCDEF");
  event.received_ms = 1001;  // One millisecond inside the accepted age boundary.
  assert(xQueueSend(g_queues.rfid_queue, &event, 0));
  verify_relay_before_rtc = true;
  run_decision_until_empty();
  assert(g_queues.events->bits & kAppReady);
  assert(relay_output_state(1));
  assert(!relay_output_state(2));
  assert(rtc_reads == 1);
  assert(!uxQueueMessagesWaiting(g_queues.uart_cmd_queue));
  assert(logic_health().processed == 1 && logic_health().expired == 0);
}
void decision_denied_card_keeps_warning_feedback() {
  setup_decision_queues();
  RfidEvent event{};
  event.reader_id = 1;
  strcpy(event.uid, "123456");
  event.received_ms = host::now;
  assert(xQueueSend(g_queues.rfid_queue, &event, 0));
  run_decision_until_empty();
  assert(!relay_output_state(1) && !relay_output_state(2));
  UartCmd feedback{};
  assert(xQueueReceive(g_queues.uart_cmd_queue, &feedback, 0));
  assert(feedback.type == UartCmd::Type::Feedback && feedback.reader_id == 1 && !feedback.allowed);
  assert(!uxQueueMessagesWaiting(g_queues.uart_cmd_queue));
}
void decision_discards_old_cards_and_requests() {
  setup_decision_queues();
  host::now = 3000;
  RfidEvent event{};
  event.reader_id = 1;
  strcpy(event.uid, "ABCDEF");
  event.received_ms = 1000;  // Exactly the maximum accepted age is rejected.
  assert(xQueueSend(g_queues.rfid_queue, &event, 0));
  LogicRequest req{};
  req.type = LogicRequestType::TriggerRelay;
  req.deadline_ms = host::now;
  req.reply_queue = xQueueCreate(1, sizeof(LogicResponse));
  req.payload.trigger_relay.relay_id = 1;
  req.payload.trigger_relay.duration_ms = 600;
  assert(xQueueSend(g_queues.logic_queue, &req, 0));
  run_decision_until_empty();
  assert(!relay_output_state(1) && !relay_output_state(2));
  assert(rtc_reads == 0 && !uxQueueMessagesWaiting(g_queues.uart_cmd_queue));
  assert(logic_health().processed == 0 && logic_health().expired == 1);
  static LogicResponse resp;
  assert(xQueueReceive(req.reply_queue, &resp, 0) && !resp.ok);
  vQueueDelete(req.reply_queue);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  std::string name = argv[1];
#define RUN(f)      \
  if (name == #f) { \
    f();            \
    return 0;       \
  }
  RUN(maintenance_requires_fresh_idle_ack)
  RUN(decision_relay_precedes_rtc_without_extra_beep)
  RUN(decision_denied_card_keeps_warning_feedback)
  RUN(decision_discards_old_cards_and_requests)
  RUN(wifi_starts_ap_and_recovers_missing_station)
  RUN(restore_bundle_rolls_back_both_files)
  RUN(interrupted_bundle_recovers_on_boot)
  RUN(auth_rejects_bad_config_and_expires_sessions) RUN(atomic_storage_preserves_old_data)
      RUN(settings_validate_and_rollback) RUN(corrupt_settings_fail_closed)
          RUN(users_restore_replaces_and_validates) RUN(users_failed_save_preserves_permissions)
              RUN(users_pages_escape_and_fit) RUN(relay_pulse_manual_and_wrap)
                  RUN(relay_startup_failure_stays_off)
                      RUN(log_rotation_preserves_archive_and_reserve)
                          RUN(log_queue_orders_clear_and_append)
                              RUN(uart_rejects_malformed_frames) return 2;
}
