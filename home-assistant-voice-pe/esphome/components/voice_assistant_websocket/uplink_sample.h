#pragma once
#include <cstdint>
namespace esphome::voice_assistant_websocket {
// Uplink (command) path: 32-bit mic sample -> int16 with a fixed integer gain applied on the 32-bit
// value (full resolution), saturating. gain == 1 is EXACTLY the old `static_cast<int16_t>(sample >> 16)`
// for every int32 input (arithmetic shift), so the default changes nothing.
inline int16_t uplink_sample(int32_t sample, uint16_t gain) {
  const int64_t v = (static_cast<int64_t>(sample) * gain) >> 16;
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}
}  // namespace esphome::voice_assistant_websocket
