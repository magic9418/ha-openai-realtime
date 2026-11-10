#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace voice_assistant_websocket {

// Called under the history mutex. The wake marker counts actual retained PCM,
// not wall-clock connection time. Once marked, never overwrite command onset.
struct WakeHistoryCursor {
  size_t head{0};
  size_t fill{0};
  size_t prewake_bytes{0};
  bool marked{false};

  void mark_wake() {
    prewake_bytes = fill;
    marked = true;
  }
  bool append(size_t bytes, size_t capacity, size_t idle_limit) {
    if (bytes > capacity || (marked && bytes > capacity - fill)) return false;
    head = (head + bytes) % capacity;
    fill += bytes;
    const size_t limit = marked ? capacity : idle_limit;
    if (fill > limit) fill = limit;
    return true;
  }
  void reset() { *this = WakeHistoryCursor{}; }
};

// Accept an integer JSON field, including zero; reject partial conversions,
// negative/fractional values and overflow. A delay above five seconds is invalid.
inline bool parse_wake_delay(const std::string &message, uint32_t &result) {
  const std::string key = "\"wake_open_delay_ms\"";
  size_t p = message.find(key);
  if (p == std::string::npos) return false;
  p += key.size();
  auto space = [&]() {
    while (p < message.size() && (message[p] == ' ' || message[p] == '\t' ||
           message[p] == '\r' || message[p] == '\n')) ++p;
  };
  space();
  if (p == message.size() || message[p++] != ':') return false;
  space();
  if (p == message.size() || message[p] < '0' || message[p] > '9') return false;
  uint32_t value = 0;
  const size_t first = p;
  while (p < message.size() && message[p] >= '0' && message[p] <= '9') {
    value = value * 10 + (message[p++] - '0');
    if (value > 5000) return false;
  }
  if (p - first > 1 && message[first] == '0') return false;
  space();
  if (p == message.size() || (message[p] != ',' && message[p] != '}')) return false;
  result = value;
  return true;
}

}  // namespace voice_assistant_websocket
}  // namespace esphome
