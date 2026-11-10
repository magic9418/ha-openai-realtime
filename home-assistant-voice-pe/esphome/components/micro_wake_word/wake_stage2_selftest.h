#pragma once
// Stage-2 boot self-test inputs. GENERATED INPUTS, NOT DATA: the three vectors are produced by the
// code below and by the identical Python generator in tools/gen_stage2_selftest.py (which also
// computes the expected outputs with the reference TFLite interpreter and rewrites
// wake_stage2_expected.h). Keep both generators in lockstep: tools/gen_stage2_selftest.py --check
// hashes the Python vectors and tests/test_wake_stage2.cpp hashes these; the FNV-1a values must match.
#include <cstddef>
#include <cstdint>

namespace esphome::micro_wake_word {

constexpr int STAGE2_SELFTEST_VECTORS = 3;

// k = 0: constant -128 (all-minimum). k = 1: sawtooth, (i%32)*8-128. k = 2: 32-bit LCG (Numerical Recipes),
// (top byte >> 2) - 90. (All-zero / full-range patterns saturate the sigmoid at the same score for this model,
// which would make the test blind; these three give clearly different outputs: scores ~89, ~33, ~57.)
inline void stage2_selftest_vector(int k, int8_t *out, size_t n) {
  uint32_t x = 12345u;
  for (size_t i = 0; i < n; i++) {
    int v = -128;
    if (k == 1) {
      v = static_cast<int>(i % 32) * 8 - 128;
    } else if (k == 2) {
      x = x * 1664525u + 1013904223u;
      v = static_cast<int>((x >> 24) >> 2) - 90;
    }
    out[i] = static_cast<int8_t>(v);
  }
}

}  // namespace esphome::micro_wake_word
