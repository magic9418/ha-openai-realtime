#pragma once
// Wake-trigger capture: the ~2 s of audio THE WAKE-WORD DETECTOR heard when it fired.
//
// Every false-wake clip saved so far is post-wake COMMAND-path audio (channel 0); this is the audio
// that actually fired micro_wake_word (its own channel + gain, 16 kHz mono PCM16). Pure logic with
// no ESPHome/IDF dependency so it is host-testable (tests/test_wake_trigger_capture.cpp).
//
// Wire contract (all JSON text frames, PCM16 LE mono, base64 standard alphabet + padding):
//   {"type":"wake_trigger_start", ...meta...}  then N x {"type":"wake_trigger_chunk","seq":i,
//   "pcm_b64":"..."} (each <= WT_CHUNK_BYTES raw)  then {"type":"wake_trigger_end","samples":n}
//
// MEMORY: the ring + snapshot + JSON scratch are ONE PSRAM allocation made once at setup by the
// component (never internal RAM; on failure the feature is disabled). This header only manages the
// bytes it is handed.
//
// CONCURRENCY: single producer (the i2s mic task) / single consumer (the main loop), lock-free.
// The ring is WT_SLACK bytes larger than a snapshot; the producer only ever overwrites the OLDEST
// slack bytes, which a snapshot never reads, so a snapshot copied while the mic task keeps pushing
// is intact as long as the producer advanced < WT_SLACK during the copy (checked; else rejected).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "wake_capture_sample.h"

namespace esphome::voice_assistant_websocket {

constexpr uint32_t WT_SAMPLE_RATE = 16000;
constexpr size_t WT_BYTES = 2000 * 2 * WT_SAMPLE_RATE / 1000;  // 64,000 B = 2000 ms mono PCM16
constexpr size_t WT_SAMPLES = WT_BYTES / 2;
constexpr size_t WT_SLACK = 8192;                              // producer headroom, see CONCURRENCY
constexpr size_t WT_RING_BYTES = WT_BYTES + WT_SLACK;
constexpr size_t WT_PUSH_PIECE_FRAMES = 512;                   // pos published every 1 KiB
constexpr size_t WT_MAX_INFLIGHT = WT_PUSH_PIECE_FRAMES * 2;   // unpublished bytes ahead of pos
constexpr size_t WT_CHUNK_BYTES = 4096;
constexpr size_t WT_B64_CHUNK_MAX = ((WT_CHUNK_BYTES + 2) / 3) * 4;  // 5464
constexpr size_t WT_JSON_BYTES = WT_B64_CHUNK_MAX + 96;              // chunk JSON / start frame scratch
constexpr size_t WT_ALLOC_BYTES = WT_RING_BYTES + WT_BYTES + WT_JSON_BYTES;
static_assert(WT_RING_BYTES % 2 == 0 && WT_SLACK > WT_MAX_INFLIGHT * 2, "slack must cover in-flight pushes");

inline size_t wt_chunk_count(size_t bytes) { return (bytes + WT_CHUNK_BYTES - 1) / WT_CHUNK_BYTES; }
inline size_t wt_b64_len(size_t n) { return ((n + 2) / 3) * 4; }

// Standard-alphabet base64 with '=' padding. `out` needs wt_b64_len(n) bytes (no NUL written).
inline size_t wt_b64_encode(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63];
    out[o++] = T[v & 63];
  }
  if (n - i == 1) {
    const uint32_t v = uint32_t(in[i]) << 16;
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (n - i == 2) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63];
    out[o++] = '=';
  }
  return o;
}

// Build chunk `seq` of a `total`-byte snapshot as one NUL-terminated JSON frame into buf (cap
// WT_JSON_BYTES). Returns the frame length, or 0 if seq is out of range.
inline size_t wt_build_chunk_json(const uint8_t *snap, size_t total, size_t seq, char *buf, size_t cap) {
  const size_t off = seq * WT_CHUNK_BYTES;
  if (off >= total) return 0;
  const size_t n = std::min(WT_CHUNK_BYTES, total - off);
  const int h = snprintf(buf, cap, "{\"type\":\"wake_trigger_chunk\",\"seq\":%u,\"pcm_b64\":\"", (unsigned) seq);
  if (h < 0 || size_t(h) + wt_b64_len(n) + 3 > cap) return 0;
  size_t len = size_t(h) + wt_b64_encode(snap + off, n, buf + h);
  buf[len++] = '"';
  buf[len++] = '}';
  buf[len] = '\0';
  return len;
}

class WakeTriggerRing {
 public:
  // `mem` must hold WT_ALLOC_BYTES; the three regions are carved from it.
  bool attach(uint8_t *mem) {
    if (mem == nullptr) return false;
    ring_ = mem;
    snap_ = mem + WT_RING_BYTES;
    json_ = reinterpret_cast<char *>(mem + WT_RING_BYTES + WT_BYTES);
    pos_.store(0);
    filled_.store(0);
    return true;
  }
  bool attached() const { return ring_ != nullptr; }
  const uint8_t *snapshot_data() const { return snap_; }
  char *json_buf() const { return json_; }
  uint32_t filled_bytes() const { return filled_.load(std::memory_order_relaxed); }

  // PRODUCER (mic task). Convert `frames` interleaved 32-bit stereo frames (the same 16 kHz source
  // micro_wake_word consumes) to the detector's mono PCM16 — channel `ch`, Q31->Q25 gain/clamp via
  // wake_capture_sample, identical to the enrollment wake_tap path — straight into the ring.
  void push_stereo32(const int32_t *stereo, size_t frames, uint8_t ch, uint8_t gain) {
    if (ring_ == nullptr) return;
    constexpr size_t cap_s = WT_RING_BYTES / 2;
    int16_t *r = reinterpret_cast<int16_t *>(ring_);
    size_t idx = pos_.load(std::memory_order_relaxed) / 2;
    size_t done = 0;
    while (done < frames) {
      const size_t n = std::min(WT_PUSH_PIECE_FRAMES, frames - done);
      for (size_t i = 0; i < n; i++) {
        r[idx] = wake_capture_sample(stereo[(done + i) * 2 + ch], gain);
        if (++idx == cap_s) idx = 0;
      }
      done += n;
      const uint32_t f = filled_.load(std::memory_order_relaxed);
      filled_.store(std::min<uint32_t>(f + uint32_t(n * 2), uint32_t(WT_RING_BYTES)), std::memory_order_relaxed);
      pos_.store(uint32_t(idx * 2), std::memory_order_release);
    }
  }

  // CONSUMER (main loop). Copy the newest WT_BYTES, oldest-first, into the snapshot buffer.
  // False if the ring has not yet accumulated WT_BYTES or the producer outran the slack mid-copy.
  bool snapshot() {
    if (ring_ == nullptr || filled_.load(std::memory_order_relaxed) < WT_BYTES) return false;
    const uint32_t p = pos_.load(std::memory_order_acquire);
    const size_t start = (p + WT_RING_BYTES - WT_BYTES) % WT_RING_BYTES;
    const size_t first = std::min(WT_BYTES, WT_RING_BYTES - start);
    std::memcpy(snap_, ring_ + start, first);
    if (first < WT_BYTES) std::memcpy(snap_ + first, ring_, WT_BYTES - first);
    const uint32_t p2 = pos_.load(std::memory_order_acquire);
    const size_t advanced = (p2 + WT_RING_BYTES - p) % WT_RING_BYTES;
    return advanced + WT_MAX_INFLIGHT <= WT_SLACK;
  }

 private:
  uint8_t *ring_{nullptr};
  uint8_t *snap_{nullptr};
  char *json_{nullptr};
  std::atomic<uint32_t> pos_{0};     // next write byte offset in the ring (published per piece)
  std::atomic<uint32_t> filled_{0};  // bytes ever pushed, saturating at WT_RING_BYTES
};

}  // namespace esphome::voice_assistant_websocket
