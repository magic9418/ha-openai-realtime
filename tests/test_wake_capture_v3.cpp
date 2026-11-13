// Host test: g++ -std=c++17 -I. -Iesphome/components tests/test_wake_capture_v3.cpp -o /tmp/t3 && /tmp/t3 > /tmp/t3.out
// Prints frames as JSON lines; tests/check_wake_capture_v3.py validates them with Python (zlib CRC, base64, JSON).
#include <cstdio>
#include <cstring>
#include <vector>
#include "voice_assistant_websocket/wake_capture_v3.h"
using namespace esphome::voice_assistant_websocket;
using namespace esphome::micro_wake_word;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
  std::vector<int8_t> feats(250 * 40);
  uint32_t x = 7;
  for (auto &b : feats) { x = x * 1664525u + 1013904223u; b = int8_t(x >> 24); }
  FeatureWindow w; w.frames = 248; w.pre_frames = 198; w.post_frames = 50; w.fire_idx = 197; w.stride = 3; w.n_probs = 82;
  for (int i = 0; i < 82; i++) { w.probs[i] = uint8_t(i * 3); w.vad[i] = uint8_t(255 - i); }
  std::vector<char> buf(5600);
  WakeS2Meta s2; s2.present = true; s2.follow_features = true; s2.mode = 1; s2.score = 77; s2.threshold = 15; s2.infer_ms = 12;
  snprintf(s2.model, sizeof s2.model, "penny_stage2_v1");
  size_t n = wf_build_trigger_start(buf.data(), buf.size(), false, true, 16000, 40000, 20, 1, 6, 235, 5, "idle",
                                    "penny_v5_oct1.json", "fw-x", 500, 2000, 200, 210, s2);
  CHECK(n > 0); printf("%s\n", buf.data());
  s2.present = false; s2.follow_features = false;
  n = wf_build_trigger_start(buf.data(), buf.size(), true, false, 16000, 40000, 20, 1, 6, 235, 5, "session", "m", "f", 500, 2000, -1, -1, s2);
  CHECK(n > 0); printf("%s\n", buf.data());
  s2.present = true; s2.follow_features = false; snprintf(s2.features_skip, sizeof s2.features_skip, "window");
  n = wf_build_trigger_start(buf.data(), buf.size(), false, false, 16000, 40000, 20, 1, 6, 235, 5, "idle", "m", "f", 500, 2000, 1, 1, s2);
  CHECK(n > 0 && strstr(buf.data(), "\"features_skip\":\"window\"") && !strstr(buf.data(), "features_follow"));
  const size_t frames = wf_feature_frame_count(w);
  CHECK(frames == 10 + 2);  // 9920 B -> 10 chunks
  size_t maxlen = 0;
  for (size_t f = 0; f < frames; f++) {
    n = wf_build_feature_frame(f, feats.data(), w, buf.data(), buf.size());
    CHECK(n > 0 && n < 1500); maxlen = std::max(maxlen, n);
    printf("%s\n", buf.data());
  }
  CHECK(wf_build_feature_frame(frames, feats.data(), w, buf.data(), buf.size()) == 0);
  fprintf(stderr, "max feature frame %zu B\n", maxlen);
  uint8_t m, t;
  CHECK(wf_parse_wake_stage2("{\"type\":\"hello\",\"wake_stage2\": {\"mode\": \"enforce\", \"threshold\": 140}}", m, t) && m == 2 && t == 140);
  CHECK(wf_parse_wake_stage2("{\"wake_stage2\":{\"threshold\":0,\"mode\":\"shadow\"}}", m, t) && m == 1 && t == 0);
  CHECK(!wf_parse_wake_stage2("{\"wake_stage2\":{\"mode\":\"bogus\",\"threshold\":3}}", m, t));
  CHECK(!wf_parse_wake_stage2("{\"wake_stage2\":{\"mode\":\"off\",\"threshold\":300}}", m, t));
  CHECK(!wf_parse_wake_stage2("{\"type\":\"hello\"}", m, t));
  bool to = false;
  CHECK(wf_parse_talk_over("{\"type\":\"hello\",\"talk_over\": true}", to) && to);
  CHECK(wf_parse_talk_over("{\"talk_over\":false,\"wake_open_delay_ms\":200}", to) && !to);
  CHECK(!wf_parse_talk_over("{\"type\":\"hello\"}", to));
  CHECK(!wf_parse_talk_over("{\"talk_over\":\"true\"}", to));
  CHECK(!wf_parse_talk_over("{\"talk_over\":1}", to));
  CHECK(wf_hello_is_capture_only("{\"type\":\"hello\",\"capture_only\":true}"));
  CHECK(!wf_hello_is_capture_only("{\"type\":\"hello\",\"wake_open_delay_ms\":200}"));
  bool ok = false;
  CHECK(wf_parse_capture_ack("{\"type\":\"capture_ack\",\"ok\":true,\"stored\":true}", ok) && ok);
  CHECK(wf_parse_capture_ack("{\"type\":\"capture_ack\",\"ok\":false,\"reason\":\"rate_limited\"}", ok) && !ok);
  CHECK(!wf_parse_capture_ack("{\"type\":\"hello\"}", ok));
  CHECK(wf_crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xCBF43926u);
  fprintf(stderr, fails ? "FAILED\n" : "ok\n");
  return fails ? 1 : 0;
}
