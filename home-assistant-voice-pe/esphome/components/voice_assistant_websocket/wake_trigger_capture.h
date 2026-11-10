#pragma once
// Wake-trigger capture: the 2.5 s of audio THE WAKE-WORD DETECTOR heard around when it fired:
// WT_PRE_MS (2000) before the detection instant plus WT_POST_MS (500) after it. The pre part is the
// window the server-side verifier was trained on; the post part is extra context (what came right
// after the trigger). The firmware reports `post_trigger_ms` so the server knows where the
// detection instant sits: it is the last `post_trigger_ms` of the clip's end.
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
// The snapshot ENDS at a caller-chosen byte counter (detection instant + WT_POST_BYTES), which is
// normally a few ms behind the producer; that lag also has to fit inside WT_SLACK (checked).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "wake_capture_sample.h"

namespace esphome::voice_assistant_websocket {

constexpr uint32_t WT_SAMPLE_RATE = 16000;
constexpr uint32_t WT_PRE_MS = 2000;                           // before the detection instant
constexpr uint32_t WT_POST_MS = 500;                           // after the detection instant
constexpr size_t WT_PRE_BYTES = size_t(WT_PRE_MS) * 2 * WT_SAMPLE_RATE / 1000;    // 64,000 B
constexpr size_t WT_POST_BYTES = size_t(WT_POST_MS) * 2 * WT_SAMPLE_RATE / 1000;  // 16,000 B
constexpr size_t WT_BYTES = WT_PRE_BYTES + WT_POST_BYTES;      // 80,000 B = 2500 ms mono PCM16
constexpr size_t WT_SAMPLES = WT_BYTES / 2;
// Producer headroom + snapshot lag budget, see CONCURRENCY. 16 KiB = 512 ms, so the main loop may
// run ~400 ms late reaching the snapshot point before it is rejected.
constexpr size_t WT_SLACK = 16384;
constexpr size_t WT_RING_BYTES = WT_BYTES + WT_SLACK;
constexpr size_t WT_PUSH_PIECE_FRAMES = 512;                   // pos published every 1 KiB
constexpr size_t WT_MAX_INFLIGHT = WT_PUSH_PIECE_FRAMES * 2;   // unpublished bytes ahead of pos
constexpr size_t WT_CHUNK_BYTES = 4096;
constexpr size_t WT_B64_CHUNK_MAX = ((WT_CHUNK_BYTES + 2) / 3) * 4;  // 5464
constexpr size_t WT_JSON_BYTES = WT_B64_CHUNK_MAX + 96;              // chunk JSON / start frame scratch
constexpr size_t WT_ALLOC_BYTES = WT_RING_BYTES + WT_BYTES + WT_JSON_BYTES;
static_assert(WT_RING_BYTES % 2 == 0 && WT_SLACK > WT_MAX_INFLIGHT * 2, "slack must cover in-flight pushes");
// The byte counter wraps at WT_TOTAL_MOD, the largest multiple of WT_RING_BYTES that fits uint32, so
// `total % WT_RING_BYTES` is always the ring write offset (no phase error at the 2^32 wrap).
constexpr uint32_t WT_TOTAL_MOD = uint32_t(0xFFFFFFFFu / WT_RING_BYTES) * uint32_t(WT_RING_BYTES);
inline uint32_t wt_total_add(uint32_t t, uint32_t n) { return uint32_t((uint64_t(t) + n) % WT_TOTAL_MOD); }
inline uint32_t wt_total_diff(uint32_t a, uint32_t b) {  // a - b, modulo the counter
  return uint32_t((uint64_t(a) + WT_TOTAL_MOD - b) % WT_TOTAL_MOD);
}

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
    total_.store(0);
    filled_.store(0);
    return true;
  }
  bool attached() const { return ring_ != nullptr; }
  const uint8_t *snapshot_data() const { return snap_; }
  char *json_buf() const { return json_; }
  uint32_t filled_bytes() const { return filled_.load(std::memory_order_relaxed); }
  // Bytes ever pushed, modulo WT_TOTAL_MOD. A detection instant is remembered as this value.
  uint32_t total_bytes() const { return total_.load(std::memory_order_acquire); }
  // True once the producer has advanced to (or past) `end_total` (within half the counter range).
  bool reached(uint32_t end_total) const {
    return wt_total_diff(total_bytes(), end_total) < WT_TOTAL_MOD / 2;
  }

  // PRODUCER (mic task). Convert `frames` interleaved 32-bit stereo frames (the same 16 kHz source
  // micro_wake_word consumes) to the detector's mono PCM16 — channel `ch`, Q31->Q25 gain/clamp via
  // wake_capture_sample, identical to the enrollment wake_tap path — straight into the ring.
  void push_stereo32(const int32_t *stereo, size_t frames, uint8_t ch, uint8_t gain) {
    if (ring_ == nullptr) return;
    constexpr size_t cap_s = WT_RING_BYTES / 2;
    int16_t *r = reinterpret_cast<int16_t *>(ring_);
    size_t idx = (total_.load(std::memory_order_relaxed) % WT_RING_BYTES) / 2;
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
      total_.store(wt_total_add(total_.load(std::memory_order_relaxed), uint32_t(n * 2)), std::memory_order_release);
    }
  }

  // CONSUMER (main loop). Copy the WT_BYTES that END at counter value `end_total`, oldest-first, into
  // the snapshot buffer. False if the ring has not yet held that much, `end_total` is not yet reached
  // or is too far behind the producer, or the producer outran the slack mid-copy.
  // NEO capture-v3: `dest` (WT_BYTES) lets capture-only jobs snapshot into their own buffers; default = snap_.
  bool snapshot_ending_at(uint32_t end_total, uint8_t *dest = nullptr) {
    uint8_t *out = dest != nullptr ? dest : this->snap_;
    if (ring_ == nullptr) return false;
    const uint32_t t1 = total_.load(std::memory_order_acquire);
    const uint32_t lag = wt_total_diff(t1, end_total);  // bytes the producer is ahead of the end point
    if (lag >= WT_TOTAL_MOD / 2) return false;          // end point still in the future
    if (filled_.load(std::memory_order_relaxed) < WT_BYTES + lag) return false;
    if (lag + WT_MAX_INFLIGHT > WT_SLACK) return false;  // oldest wanted bytes may already be overwritten
    const size_t start = (size_t(end_total % WT_RING_BYTES) + WT_RING_BYTES - WT_BYTES) % WT_RING_BYTES;
    const size_t first = std::min(WT_BYTES, WT_RING_BYTES - start);
    std::memcpy(out, ring_ + start, first);
    if (first < WT_BYTES) std::memcpy(out + first, ring_, WT_BYTES - first);
    const uint32_t t2 = total_.load(std::memory_order_acquire);
    const uint32_t advanced = wt_total_diff(t2, t1);
    return uint64_t(lag) + advanced + WT_MAX_INFLIGHT <= WT_SLACK;
  }
  // Newest WT_BYTES ending now (no post-trigger wait): used by tests and as a plain "freeze".
  bool snapshot() { return snapshot_ending_at(total_.load(std::memory_order_acquire)); }

 private:
  uint8_t *ring_{nullptr};
  uint8_t *snap_{nullptr};
  char *json_{nullptr};
  std::atomic<uint32_t> total_{0};   // bytes ever pushed mod WT_TOTAL_MOD; ring offset = total % ring size
  std::atomic<uint32_t> filled_{0};  // bytes ever pushed, saturating at WT_RING_BYTES
};

// ---- Server verdict on a cold wake -------------------------------------------------------------
// {"type":"wake_verdict","accepted":true|false}: the server's second-stage check of the trigger clip.
// accepted:true -> duck the music now (as before); false -> do not duck (the session is about to be
// closed). Tolerates JSON whitespace and key order. Returns false for any other frame / bad value.
inline bool wt_parse_verdict(const std::string &message, bool &accepted) {
  auto find_key = [&](const char *key) -> size_t {
    const size_t k = message.find(key);
    if (k == std::string::npos) return std::string::npos;
    size_t p = k + std::strlen(key);
    while (p < message.size() && (message[p] == ' ' || message[p] == '\t')) ++p;
    if (p >= message.size() || message[p] != ':') return std::string::npos;
    ++p;
    while (p < message.size() && (message[p] == ' ' || message[p] == '\t')) ++p;
    return p;
  };
  const size_t t = find_key("\"type\"");
  if (t == std::string::npos || message.compare(t, 14, "\"wake_verdict\"") != 0) return false;
  const size_t a = find_key("\"accepted\"");
  if (a == std::string::npos) return false;
  if (message.compare(a, 4, "true") == 0) {
    accepted = true;
    return true;
  }
  if (message.compare(a, 5, "false") == 0) {
    accepted = false;
    return true;
  }
  return false;
}

}  // namespace esphome::voice_assistant_websocket
