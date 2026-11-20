#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <deque>
#include <functional>
#include <algorithm>
class String {
 public:
  std::string value;
  String() = default;
  String(const char* s) : value(s ? s : "") {}
  String(char c) : value(1, c) {}
  String(const std::string& s) : value(s) {}
  template <class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T n) : value(std::to_string(n)) {}
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  bool reserve(size_t n) {
    value.reserve(n);
    return true;
  }
  bool concat(const char* p, size_t n) {
    value.append(p, n);
    return true;
  }
  bool startsWith(const String& s) const { return value.rfind(s.value, 0) == 0; }
  bool endsWith(const String& s) const {
    return value.size() >= s.value.size() &&
           value.compare(value.size() - s.value.size(), s.value.size(), s.value) == 0;
  }
  int indexOf(char c, size_t start = 0) const {
    auto n = value.find(c, start);
    return n == std::string::npos ? -1 : int(n);
  }
  int indexOf(const String& s, size_t start = 0) const {
    auto n = value.find(s.value, start);
    return n == std::string::npos ? -1 : int(n);
  }
  String substring(size_t a, size_t b = std::string::npos) const {
    return a > value.size() ? String()
                            : String(value.substr(a, b == std::string::npos ? b : b - a));
  }
  void remove(size_t p, size_t n = std::string::npos) { value.erase(p, n); }
  void toCharArray(char* out, size_t n) const {
    if (n) {
      strncpy(out, c_str(), n - 1);
      out[n - 1] = 0;
    }
  }
  void trim() {
    auto a = value.find_first_not_of(" \t\r\n");
    auto b = value.find_last_not_of(" \t\r\n");
    value = a == std::string::npos ? "" : value.substr(a, b - a + 1);
  }
  long toInt() const { return strtol(c_str(), nullptr, 10); }
  char operator[](size_t i) const { return value[i]; }
  String& operator+=(const String& s) {
    value += s.value;
    return *this;
  }
  template <class T,
            typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, char>::value,
                                    int>::type = 0>
  String& operator+=(T value) {
    this->value += std::to_string(value);
    return *this;
  }
  String& operator+=(char c) {
    value += c;
    return *this;
  }
  friend String operator+(String a, const String& b) {
    a += b;
    return a;
  }
  friend bool operator==(const String& a, const String& b) { return a.value == b.value; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
  friend bool operator<(const String& a, const String& b) { return a.value < b.value; }
};
namespace host {
extern uint32_t now;
extern int pins[64];
extern int modes[64];
extern uint32_t now_us;
extern int atomic_depth;
extern bool require_atomic;
}  // namespace host
inline uint32_t millis() { return host::now; }
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT = 0, FALLING = 2, SERIAL_8N1 = 0;
inline uint32_t micros() {
  if (host::require_atomic && host::atomic_depth <= 0) std::abort();
  return host::now_us;
}
inline void delay(uint32_t n) {
  host::now += n;
  host::now_us += n * 1000;
}
inline uint8_t digitalPinToInterrupt(uint8_t p) { return p; }
inline void attachInterrupt(uint8_t, void (*)(), int) {}
extern volatile uint8_t PIND, PCICR, PCMSK2;
#define ISR(name) void name()
#define PCIE2 2
#define PCINT20 4
#define PCINT21 5

inline void pinMode(uint8_t p, int mode) { host::modes[p] = mode; }
inline int digitalRead(uint8_t p) { return host::pins[p]; }
inline void digitalWrite(uint8_t p, int v) { host::pins[p] = v; }
enum hardwareSerial_error_t { UART_FIFO_OVF_ERROR };
struct FakeSerial {
  std::deque<char> input;
  std::string output;
  void begin(uint32_t, int = 0, int = 0, int = 0) {}
  void setRxBufferSize(size_t) {}
  void onReceiveError(std::function<void(hardwareSerial_error_t)>) {}
  int available() { return input.size(); }
  int read() {
    if (input.empty()) return -1;
    char c = input.front();
    input.pop_front();
    return c;
  }
  template <class T>
  void print(T t) {
    output += String(t).value;
  }
  template <class T>
  void println(T t) {
    print(t);
    output += '\n';
  }
  template <class... A>
  void printf(const char*, A...) {}
};
extern FakeSerial Serial, Serial2;
