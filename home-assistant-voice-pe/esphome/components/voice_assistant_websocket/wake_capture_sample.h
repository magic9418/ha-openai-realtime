#pragma once
#include <algorithm>
#include <cstdint>
namespace esphome::voice_assistant_websocket {
// Match MicrophoneSource's Q31 -> Q25 gain/clamp -> PCM16 conversion.
inline int16_t wake_capture_sample(int32_t sample, uint8_t gain) {
  int32_t q25 = (sample >> 6) * gain;
  q25 = std::clamp<int32_t>(q25, -33554432, 33554431);
  return static_cast<int16_t>((q25 * 64) >> 16);
}
}
