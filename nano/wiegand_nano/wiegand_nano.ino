// Wiegand bridge for two readers (Nano -> ESP32 UART)
// Output format: "1,UID" or "2,UID" (one line per read)

#include <Arduino.h>

namespace {
constexpr uint32_t kBaud = 115200;
constexpr uint32_t kWiegandTimeoutUs = 25000;

constexpr uint8_t kR1D0 = 2;
constexpr uint8_t kR1D1 = 3;
constexpr uint8_t kR2D0 = 4;
constexpr uint8_t kR2D1 = 5;

constexpr uint8_t kR1Led = 6;
constexpr uint8_t kR1Beep = 7;
constexpr uint8_t kR2Led = 8;
constexpr uint8_t kR2Beep = 9;
constexpr uint16_t kLedOnMs = 300;
constexpr uint16_t kBeepOnMs = 80;
constexpr uint16_t kBeepOffMs = 80;

struct ReaderState {
  volatile uint8_t bits;
  volatile uint64_t data;
  volatile uint32_t last_us;
};

struct FeedbackState {
  bool active = false;
  bool allowed = false;
  uint8_t phase = 0;
  uint32_t next_ms = 0;
};

ReaderState g_r1{0, 0, 0};
ReaderState g_r2{0, 0, 0};
volatile uint8_t g_last_portd = 0;
FeedbackState g_fb1{};
FeedbackState g_fb2{};

void handle_bit(ReaderState& r, uint8_t bit) {
  if (r.bits >= 64) {
    r.bits = 0;
    r.data = 0;
  }
  r.data = (r.data << 1) | (bit & 0x1);
  r.bits++;
  r.last_us = micros();
}

void isr_r1_d0() {
  handle_bit(g_r1, 0);
}

void isr_r1_d1() {
  handle_bit(g_r1, 1);
}

ISR(PCINT2_vect) {
  uint8_t current = PIND;
  uint8_t changed = current ^ g_last_portd;
  g_last_portd = current;
  if (changed & (1 << 4)) {
    if (!(current & (1 << 4))) {
      handle_bit(g_r2, 0);
    }
  }
  if (changed & (1 << 5)) {
    if (!(current & (1 << 5))) {
      handle_bit(g_r2, 1);
    }
  }
}

void set_line_active(uint8_t pin, bool active) {
  if (active) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  } else {
    pinMode(pin, INPUT);
  }
}

void feedback_start(FeedbackState& fb, bool allowed) {
  fb.active = true;
  fb.allowed = allowed;
  fb.phase = 0;
  fb.next_ms = millis();
}

void feedback_tick(uint8_t led_pin, uint8_t beep_pin, FeedbackState& fb) {
  if (!fb.active) {
    return;
  }
  uint32_t now = millis();
  if (now < fb.next_ms) {
    return;
  }
  if (fb.allowed) {
    if (fb.phase == 0) {
      set_line_active(led_pin, true);
      set_line_active(beep_pin, true);
      fb.next_ms = now + kBeepOnMs;
      fb.phase = 1;
      return;
    }
    if (fb.phase == 1) {
      set_line_active(beep_pin, false);
      fb.next_ms = now + (kLedOnMs > kBeepOnMs ? (kLedOnMs - kBeepOnMs) : 0);
      fb.phase = 2;
      return;
    }
    set_line_active(led_pin, false);
    fb.active = false;
    return;
  }

  if (fb.phase == 0) {
    set_line_active(led_pin, true);
    set_line_active(beep_pin, true);
    fb.next_ms = now + kBeepOnMs;
    fb.phase = 1;
    return;
  }
  if (fb.phase == 1) {
    set_line_active(beep_pin, false);
    set_line_active(led_pin, false);
    fb.next_ms = now + kBeepOffMs;
    fb.phase = 2;
    return;
  }
  if (fb.phase == 2) {
    set_line_active(led_pin, true);
    set_line_active(beep_pin, true);
    fb.next_ms = now + kBeepOnMs;
    fb.phase = 3;
    return;
  }
  set_line_active(beep_pin, false);
  set_line_active(led_pin, false);
  fb.active = false;
}

void to_hex(uint64_t value, uint8_t width, char* out, size_t out_len) {
  static const char* kHex = "0123456789ABCDEF";
  if (!out || out_len == 0) {
    return;
  }
  if (width + 1 > out_len) {
    width = static_cast<uint8_t>(out_len - 1);
  }
  for (int i = width - 1; i >= 0; --i) {
    out[i] = kHex[value & 0xF];
    value >>= 4;
  }
  out[width] = '\0';
}

void emit_uid(uint8_t reader_id, uint8_t bits, uint64_t data) {
  if (bits != 26 && bits != 34) {
    return;
  }
  uint8_t payload_bits = static_cast<uint8_t>(bits - 2);
  uint64_t payload = (data >> 1) & ((payload_bits == 64) ? ~0ULL : ((1ULL << payload_bits) - 1));
  uint8_t width = static_cast<uint8_t>((payload_bits + 3) / 4);
  char uid[17];
  to_hex(payload, width, uid, sizeof(uid));
  Serial.print(reader_id);
  Serial.print(',');
  Serial.println(uid);
}

void handle_uart() {
  static char buf[16];
  static uint8_t idx = 0;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (idx == 0) {
        continue;
      }
      buf[idx] = '\0';
      idx = 0;
      if ((buf[0] == 'P' || buf[0] == 'p') &&
          (buf[1] == 'I' || buf[1] == 'i') &&
          (buf[2] == 'N' || buf[2] == 'n') &&
          (buf[3] == 'G' || buf[3] == 'g') &&
          buf[4] == '\0') {
        Serial.println("PONG");
        continue;
      }
      char action = buf[0];
      bool is_allow = (action == 'A' || action == 'a');
      bool is_deny = (action == 'D' || action == 'd');
      if (!is_allow && !is_deny) {
        continue;
      }
      const char* p = buf + 1;
      if (*p == ',' || *p == ':') {
        ++p;
      }
      int reader = atoi(p);
      if (reader == 1) {
        feedback_start(g_fb1, is_allow);
      } else if (reader == 2) {
        feedback_start(g_fb2, is_allow);
      }
      continue;
    }
    if (idx + 1 < sizeof(buf)) {
      buf[idx++] = c;
    } else {
      idx = 0;
    }
  }
}

bool try_flush_reader(uint8_t reader_id, ReaderState& r) {
  uint32_t now = micros();
  if (r.bits == 0) {
    return false;
  }
  if (now - r.last_us < kWiegandTimeoutUs) {
    return false;
  }
  noInterrupts();
  uint8_t bits = r.bits;
  uint64_t data = r.data;
  r.bits = 0;
  r.data = 0;
  interrupts();
  emit_uid(reader_id, bits, data);
  return true;
}

} // namespace

void setup() {
  Serial.begin(kBaud);

  pinMode(kR1D0, INPUT);
  pinMode(kR1D1, INPUT);
  pinMode(kR2D0, INPUT);
  pinMode(kR2D1, INPUT);

  set_line_active(kR1Led, false);
  set_line_active(kR1Beep, false);
  set_line_active(kR2Led, false);
  set_line_active(kR2Beep, false);

  attachInterrupt(digitalPinToInterrupt(kR1D0), isr_r1_d0, FALLING);
  attachInterrupt(digitalPinToInterrupt(kR1D1), isr_r1_d1, FALLING);

  g_last_portd = PIND;
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT20);
  PCMSK2 |= (1 << PCINT21);
}

void loop() {
  handle_uart();
  try_flush_reader(1, g_r1);
  try_flush_reader(2, g_r2);
  feedback_tick(kR1Led, kR1Beep, g_fb1);
  feedback_tick(kR2Led, kR2Beep, g_fb2);
  delay(2);
}
