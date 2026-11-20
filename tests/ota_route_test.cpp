// Exercises the production /firmware callbacks using simulated HTTP and flash APIs.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

constexpr int HTTP_POST = 1, UPDATE_SIZE_UNKNOWN = -1, U_FLASH = 0;
enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status = 0;
  std::string filename = "firmware.bin";
  uint8_t buf[4]{};
  size_t currentSize = 4, totalSize = 4;
};
struct Server {
  std::function<void()> complete, receive;
  HTTPUpload data;
  std::string response;
  int status = 0;
  void on(const char*, int, std::function<void()> a, std::function<void()> b) {
    complete = a;
    receive = b;
  }
  void on(const char*, int, std::function<void()> handler) { complete = handler; }
  HTTPUpload& upload() { return data; }
  void send(int code, const char*, const char* body) {
    status = code;
    response = body;
  }
};
struct FakeUpdate {
  bool running = false, error = false;
  bool begin_ok = true, write_ok = true, end_ok = true;
  int end_calls = 0, abort_calls = 0, target = -1;
  bool begin(int, int command) {
    target = command;
    error = !begin_ok;
    running = begin_ok;
    return begin_ok;
  }
  bool isRunning() { return running; }
  bool hasError() { return error; }
  size_t write(uint8_t*, size_t size) {
    if (!write_ok) {
      return 0;
    }
    return size;
  }
  bool end(bool) {
    ++end_calls;
    running = false;
    error = !end_ok;
    return end_ok;
  }
  void abort() {
    ++abort_calls;
    running = false;
    error = true;
  }
} Update;
struct {
  int restarts = 0;
  void restart() { ++restarts; }
} ESP;
bool format_ok = true, defaults_ok = true;
int formats = 0;
bool storage_format() {
  ++formats;
  return format_ok;
}
void settings_init() {}
bool settings_save() { return defaults_ok; }
void delay(int) {}
bool authorized = true, maintenance_ok = true, maintenance = false;
bool check_auth(Server&) { return authorized; }
bool require_auth(Server& s) { return check_auth(s); }
bool app_maintenance() { return maintenance; }
void app_set_maintenance(bool value) { maintenance = value; }
bool app_prepare_maintenance() {
  if (maintenance) return false;
  maintenance = maintenance_ok;
  return maintenance_ok;
}
void result(Server& s, bool ok, const char*) {
  s.send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}
void send_unauthorized(Server& s, const char* type, const char* body) { s.send(401, type, body); }
struct Fixture {
  Server server;
  bool ota_started = false, ota_completed = false, ota_failed = false;
  unsigned ota_parts = 0;
  Fixture() {
    Update = FakeUpdate{};
    ESP.restarts = 0;
    authorized = true;
    maintenance_ok = true;
    maintenance = false;
#include "firmware_route.inc"
  }
  void event(int status) {
    server.data.status = status;
    server.receive();
  }
  void finish() { server.complete(); }
  void expect_failure() {
    finish();
    assert(server.response == "{\"ok\":false}");
    assert(ESP.restarts == 0);
    assert(!Update.running);
  }
  void upload_success() {
    event(UPLOAD_FILE_START);
    event(UPLOAD_FILE_WRITE);
    event(UPLOAD_FILE_END);
  }
};

void rejects_incomplete_uploads() {
  {
    Fixture f;
    f.expect_failure();
  }
  {
    Fixture f;
    f.event(UPLOAD_FILE_START);
    f.expect_failure();
  }
  {
    Fixture f;
    f.server.data.totalSize = 0;
    f.event(UPLOAD_FILE_START);
    f.event(UPLOAD_FILE_END);
    f.expect_failure();
    assert(Update.end_calls == 0);
  }
  {
    Fixture f;
    Update.begin_ok = false;
    f.upload_success();
    f.expect_failure();
  }
  {
    Fixture f;
    Update.write_ok = false;
    f.upload_success();
    f.expect_failure();
  }
  {
    Fixture f;
    Update.end_ok = false;
    f.upload_success();
    f.expect_failure();
  }
  {
    Fixture f;
    f.event(UPLOAD_FILE_START);
    f.event(UPLOAD_FILE_WRITE);
    f.event(UPLOAD_FILE_ABORTED);
    f.expect_failure();
    assert(Update.abort_calls == 1);
  }
}
void reboots_after_finalized_upload() {
  Fixture f;
  f.upload_success();
  assert(ESP.restarts == 0);
  assert(Update.end_calls == 0);
  assert(Update.target == U_FLASH);
  f.finish();
  assert(Update.end_calls == 1);
  assert(f.server.response == "{\"ok\":true}");
  assert(ESP.restarts == 1);
  // A following empty POST must not inherit the previous upload's success.
  f.finish();
  assert(f.server.response == "{\"ok\":false}");
  assert(ESP.restarts == 1);
}
void recovers_after_failed_upload() {
  Fixture f;
  f.event(UPLOAD_FILE_START);
  f.event(UPLOAD_FILE_ABORTED);
  f.expect_failure();
  f.upload_success();
  f.finish();
  assert(f.server.response == "{\"ok\":true}");
  assert(ESP.restarts == 1);
}
void rejects_multiple_files_before_commit() {
  Fixture f;
  f.upload_success();
  assert(Update.end_calls == 0);
  f.event(UPLOAD_FILE_START);
  f.expect_failure();
  assert(Update.end_calls == 0);
}
void rejects_abort_after_file_end() {
  Fixture f;
  f.upload_success();
  f.event(UPLOAD_FILE_ABORTED);
  f.expect_failure();
  assert(Update.end_calls == 0);
}
void rejects_unauthorized_or_maintenance_failure() {
  {
    Fixture f;
    authorized = false;
    f.event(UPLOAD_FILE_START);
    f.finish();
    assert(Update.end_calls == 0 && ESP.restarts == 0);
    assert(f.server.status == 401);
  }
  {
    Fixture f;
    f.upload_success();
    maintenance_ok = false;
    f.expect_failure();
    assert(Update.end_calls == 0);
  }
}
void preserves_another_maintenance_owner() {
  Fixture f;
  f.upload_success();
  maintenance = true;
  f.expect_failure();
  assert(maintenance);
  assert(Update.end_calls == 0);
}
struct FormatFixture {
  Server server;
  bool reject_maintenance() { return maintenance; }
  FormatFixture() {
    authorized = true;
    maintenance = false;
    maintenance_ok = true;
    format_ok = defaults_ok = true;
    formats = 0;
    ESP.restarts = 0;
#include "format_route.inc"
  }
};
void format_reboots_after_erasure_even_if_defaults_fail() {
  {
    FormatFixture f;
    f.server.complete();
    assert(formats == 1 && ESP.restarts == 1);
  }
  {
    FormatFixture f;
    format_ok = false;
    f.server.complete();
    assert(formats == 1 && ESP.restarts == 1);
  }
  {
    FormatFixture f;
    defaults_ok = false;
    f.server.complete();
    assert(formats == 1 && ESP.restarts == 1);
  }
  {
    FormatFixture f;
    maintenance_ok = false;
    f.server.complete();
    assert(formats == 0 && ESP.restarts == 0);
  }
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string name = argv[1];
  if (name == "rejects_incomplete_uploads")
    rejects_incomplete_uploads();
  else if (name == "reboots_after_finalized_upload")
    reboots_after_finalized_upload();
  else if (name == "recovers_after_failed_upload")
    recovers_after_failed_upload();
  else if (name == "rejects_multiple_files_before_commit")
    rejects_multiple_files_before_commit();
  else if (name == "rejects_abort_after_file_end")
    rejects_abort_after_file_end();
  else if (name == "rejects_unauthorized_or_maintenance_failure")
    rejects_unauthorized_or_maintenance_failure();
  else if (name == "format_reboots_after_erasure_even_if_defaults_fail")
    format_reboots_after_erasure_even_if_defaults_fail();
  else if (name == "preserves_another_maintenance_owner")
    preserves_another_maintenance_owner();
  else
    return 2;
}
