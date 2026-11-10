// Host test: g++ -std=c++17 -Iesphome/components/micro_wake_word tests/test_wake_stage2.cpp -o /tmp/t && /tmp/t
// Checks the C++ self-test vector generator against the Python one (FNV-1a hashes from
// `tools/gen_stage2_selftest.py --check`) and the feature-history window arithmetic.
#include <cstdio>
#include <cstring>
#include <vector>
#include "wake_stage2_selftest.h"
#include "wake_features.h"
using namespace esphome::micro_wake_word;

static uint32_t fnv(const int8_t *a, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) h = (h ^ uint8_t(a[i])) * 0x01000193u;
  return h;
}
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
  std::vector<int8_t> v(4000);
  for (int k = 0; k < 3; k++) {
    stage2_selftest_vector(k, v.data(), v.size());
    printf("vector %d: fnv1a32 0x%08x\n", k, fnv(v.data(), v.size()));
  }
  // --- feature history window math (stride 3) ---
  static uint8_t mem[FeatureHistory::BYTES];
  FeatureHistory h;
  CHECK(h.attach(mem));
  int8_t f[40];
  for (uint32_t n = 0; n < 600; n++) {  // frame n has every byte == (n & 0x7f) - 64; prob = n&0xff
    memset(f, int((n & 0x7f)) - 64, 40);
    h.push(f, uint8_t(n & 0xff), uint8_t((n * 2) & 0xff));
  }
  CHECK(h.frames_written() == 600);
  // stage-2 window: 100 frames ending at frame 599
  std::vector<int8_t> w(4000);
  CHECK(h.copy_last(599, 100, w.data()));
  CHECK(w[0] == int((500 & 0x7f)) - 64 && w[3999] == int((599 & 0x7f)) - 64);
  CHECK(!h.copy_last(300, 100, w.data()));  // too old: overwritten
  // upload: fire frame 599 (last frame of an invocation), 20 post frames available
  for (uint32_t n = 600; n < 620; n++) {
    memset(f, int((n & 0x7f)) - 64, 40);
    h.push(f, uint8_t(n & 0xff), uint8_t((n * 2) & 0xff));
  }
  std::vector<int8_t> up(FEATURE_UPLOAD_MAX_FRAMES * 40);
  FeatureWindow fw;
  CHECK(h.copy_upload(599, 3, 50, up.data(), &fw));
  CHECK(fw.pre_frames == 198 && fw.post_frames == 20 && fw.frames == 218 && fw.fire_idx == 197);
  CHECK(up[0] == int(((599 - 197) & 0x7f)) - 64);
  CHECK(fw.n_probs == 218 / 3);
  CHECK(fw.probs[0] == uint8_t((599 - 197 + 2) & 0xff));   // output as of frame start+2
  CHECK(fw.vad[1] == uint8_t(((599 - 197 + 5) * 2) & 0xff));
  CHECK(fw.start_frame_is_invocation_start);
  // stale: fire frame too old (>250 back)
  for (uint32_t n = 620; n < 900; n++) h.push(f, 0, 0);
  CHECK(!h.copy_upload(599, 3, 50, up.data(), &fw));
  printf(fails ? "FAILED\n" : "ok\n");
  return fails ? 1 : 0;
}
