#pragma once
// Wake capture v3 (core/docs/wake_capture_v3_protocol.md): pure helpers, no ESPHome/IDF dependency so
// they are host-testable (tests/test_wake_capture_v3.cpp). JSON is built with snprintf into the caller's
// scratch buffer; nothing here allocates.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esphome/components/micro_wake_word/wake_features.h"
#include "wake_trigger_capture.h"

namespace esphome::voice_assistant_websocket {

constexpr size_t WF_CHUNK_BYTES = 1024;                                        // raw bytes per feature chunk
constexpr size_t WF_B64_CHUNK_MAX = ((WF_CHUNK_BYTES + 2) / 3) * 4;            // 1368
constexpr size_t WF_FEATURE_BYTES_MAX = micro_wake_word::FEATURE_UPLOAD_MAX_FRAMES * micro_wake_word::FEATURE_CHANNELS;

inline size_t wf_chunk_count(size_t bytes) { return (bytes + WF_CHUNK_BYTES - 1) / WF_CHUNK_BYTES; }

// CRC-32 (IEEE 802.3, the zlib polynomial), nibble-table implementation (no 1 KB table in RAM).
inline uint32_t wf_crc32(const uint8_t *d, size_t n, uint32_t crc = 0) {
  static const uint32_t T[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                 0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    crc = (crc >> 4) ^ T[crc & 0x0f];
    crc = (crc >> 4) ^ T[crc & 0x0f];
  }
  return ~crc;
}

// Stage-2 facts attached to a captured detection (copied from the detector at capture time).
struct WakeS2Meta {
  bool present{false};            // emit the "stage2" object
  bool follow_features{false};    // emit features_follow:true
  uint8_t mode{0};                // 0 off, 1 shadow, 2 enforce
  uint8_t score{0};
  uint8_t threshold{0};
  bool reject{false};
  uint16_t infer_ms{0};
  char status[16]{"ok"};          // ok | selftest_fail | nomem | nopsram | nomodel | notconfigured
  char model[40]{"s2"};
  // Why no feature upload (only set when features_follow is false): nodetector | nobuf | window | disabled.
  char features_skip[12]{""};
};

inline const char *wf_mode_name(uint8_t m) { return m == 2 ? "enforce" : (m == 1 ? "shadow" : "off"); }

// wake_trigger_start with the v2 fields unchanged plus the v3 additions. Returns the length or 0 on overflow.
inline size_t wf_build_trigger_start(char *buf, size_t cap, bool bargein, bool capture_only, uint32_t sample_rate,
                                     uint32_t samples, uint32_t chunks, uint8_t channel, uint8_t gain, uint8_t cutoff,
                                     uint8_t window, const char *regime, const char *model, const char *fw,
                                     uint32_t post_ms, uint32_t pre_ms, int score_avg, int score_max,
                                     const WakeS2Meta &s2) {
  int n = snprintf(buf, cap,
                   "{\"type\":\"wake_trigger_start\",\"kind\":\"%s\",\"sample_rate\":%u,\"samples\":%u,\"chunks\":%u,"
                   "\"channel\":%u,\"gain_factor\":%u,\"cutoff\":%u,\"window\":%u,\"regime\":\"%s\",\"model\":\"%s\","
                   "\"fw\":\"%s\",\"post_trigger_ms\":%u,\"pre_trigger_ms\":%u,\"capture_version\":3",
                   bargein ? "bargein" : "cold", (unsigned) sample_rate, (unsigned) samples, (unsigned) chunks,
                   (unsigned) channel, (unsigned) gain, (unsigned) cutoff, (unsigned) window, regime, model, fw,
                   (unsigned) post_ms, (unsigned) pre_ms);
  if (n <= 0 || size_t(n) >= cap) return 0;
  size_t len = size_t(n);
  auto app = [&](const char *fmt, auto... a) -> bool {
    const int k = snprintf(buf + len, cap - len, fmt, a...);
    if (k <= 0 || len + size_t(k) >= cap) return false;
    len += size_t(k);
    return true;
  };
  if (score_avg >= 0 && !app(",\"detector_score\":%d,\"detector_score_max\":%d", score_avg, score_max)) return 0;
  if (s2.present) {
    if (!app(",\"stage2\":{\"model\":\"%s\",\"mode\":\"%s\",\"score\":%u,\"threshold\":%u,\"verdict\":\"%s\","
             "\"infer_ms\":%u},\"stage2_status\":\"%s\"",
             s2.model, wf_mode_name(s2.mode), (unsigned) s2.score, (unsigned) s2.threshold,
             s2.reject ? "reject" : "pass", (unsigned) s2.infer_ms, s2.status))
      return 0;
  }
  if (capture_only && !app("%s", ",\"capture_only\":true")) return 0;
  if (s2.follow_features && !app("%s", ",\"features_follow\":true")) return 0;
  if (!s2.follow_features && s2.features_skip[0] != '\0' && !app(",\"features_skip\":\"%s\"", s2.features_skip)) return 0;
  if (len + 2 > cap) return 0;
  buf[len++] = '}';
  buf[len] = '\0';
  return len;
}

// Feature upload frames. Frame index f: 0 = wake_features_start, 1..K = chunks, K+1 = wake_features_end,
// K = wf_chunk_count(frames*40). Returns the frame length or 0.
inline size_t wf_feature_frame_count(const micro_wake_word::FeatureWindow &w) {
  return wf_chunk_count(size_t(w.frames) * micro_wake_word::FEATURE_CHANNELS) + 2;
}

inline size_t wf_build_feature_frame(size_t f, const int8_t *feats, const micro_wake_word::FeatureWindow &w, char *buf,
                                     size_t cap) {
  const size_t bytes = size_t(w.frames) * micro_wake_word::FEATURE_CHANNELS;
  const size_t k = wf_chunk_count(bytes);
  if (f == 0) {
    int n = snprintf(buf, cap, "{\"type\":\"wake_features_start\",\"frames\":%u,\"channels\":40,\"post_frames\":%u,"
                               "\"step_ms\":10,\"dtype\":\"int8\",\"probs\":[",
                     (unsigned) w.frames, (unsigned) w.post_frames);
    if (n <= 0 || size_t(n) >= cap) return 0;
    size_t len = size_t(n);
    for (int pass = 0; pass < 2; pass++) {
      const uint8_t *a = pass == 0 ? w.probs : w.vad;
      for (size_t j = 0; j < w.n_probs; j++) {
        n = snprintf(buf + len, cap - len, j ? ",%u" : "%u", (unsigned) a[j]);
        if (n <= 0 || len + size_t(n) >= cap) return 0;
        len += size_t(n);
      }
      n = snprintf(buf + len, cap - len, pass == 0 ? "],\"vad_probs\":[" : "],\"fire_frame\":%u}", (unsigned) w.fire_idx);
      if (n <= 0 || len + size_t(n) >= cap) return 0;
      len += size_t(n);
    }
    return len;
  }
  if (f <= k) {
    const size_t off = (f - 1) * WF_CHUNK_BYTES;
    const size_t m = std::min(WF_CHUNK_BYTES, bytes - off);
    const int h = snprintf(buf, cap, "{\"type\":\"wake_features_chunk\",\"seq\":%u,\"b64\":\"", (unsigned) (f - 1));
    if (h <= 0 || size_t(h) + wt_b64_len(m) + 3 > cap) return 0;
    size_t len = size_t(h) + wt_b64_encode(reinterpret_cast<const uint8_t *>(feats) + off, m, buf + h);
    buf[len++] = '"';
    buf[len++] = '}';
    buf[len] = '\0';
    return len;
  }
  if (f == k + 1) {
    const uint32_t crc = wf_crc32(reinterpret_cast<const uint8_t *>(feats), bytes);
    const int n = snprintf(buf, cap, "{\"type\":\"wake_features_end\",\"bytes\":%u,\"crc32\":%u}", (unsigned) bytes,
                           (unsigned) crc);
    return (n > 0 && size_t(n) < cap) ? size_t(n) : 0;
  }
  return 0;
}

// ---- Parsers (tolerate whitespace, any key order) ----------------------------------------------------
inline size_t wf_find_value(const std::string &m, const char *key, size_t from = 0) {
  const size_t k = m.find(key, from);
  if (k == std::string::npos) return std::string::npos;
  size_t p = k + std::strlen(key);
  while (p < m.size() && (m[p] == ' ' || m[p] == '\t')) ++p;
  if (p >= m.size() || m[p] != ':') return std::string::npos;
  ++p;
  while (p < m.size() && (m[p] == ' ' || m[p] == '\t')) ++p;
  return p;
}

// hello: "wake_stage2":{"mode":"off|shadow|enforce","threshold":0-255}. All-or-nothing.
inline bool wf_parse_wake_stage2(const std::string &m, uint8_t &mode, uint8_t &threshold) {
  const size_t s = wf_find_value(m, "\"wake_stage2\"");
  if (s == std::string::npos || s >= m.size() || m[s] != '{') return false;
  const size_t e = m.find('}', s);
  if (e == std::string::npos) return false;
  const std::string obj = m.substr(s, e - s + 1);
  const size_t mv = wf_find_value(obj, "\"mode\"");
  const size_t tv = wf_find_value(obj, "\"threshold\"");
  if (mv == std::string::npos || tv == std::string::npos) return false;
  uint8_t md;
  if (obj.compare(mv, 5, "\"off\"") == 0) md = 0;
  else if (obj.compare(mv, 8, "\"shadow\"") == 0) md = 1;
  else if (obj.compare(mv, 9, "\"enforce\"") == 0) md = 2;
  else return false;
  if (tv >= obj.size() || obj[tv] < '0' || obj[tv] > '9') return false;
  char *end = nullptr;
  const long t = std::strtol(obj.c_str() + tv, &end, 10);
  if (t < 0 || t > 255) return false;
  mode = md;
  threshold = uint8_t(t);
  return true;
}

inline bool wf_hello_is_capture_only(const std::string &m) {
  const size_t v = wf_find_value(m, "\"capture_only\"");
  return v != std::string::npos && m.compare(v, 4, "true") == 0;
}

inline bool wf_is_type(const std::string &m, const char *type) {
  const size_t v = wf_find_value(m, "\"type\"");
  const std::string want = std::string("\"") + type + "\"";
  return v != std::string::npos && m.compare(v, want.size(), want) == 0;
}

// capture_ack: ok true/false. `rate_limited` when ok:false and reason is rate_limited.
inline bool wf_parse_capture_ack(const std::string &m, bool &ok) {
  if (!wf_is_type(m, "capture_ack")) return false;
  const size_t v = wf_find_value(m, "\"ok\"");
  if (v == std::string::npos) return false;
  ok = m.compare(v, 4, "true") == 0;
  return true;
}

}  // namespace esphome::voice_assistant_websocket
