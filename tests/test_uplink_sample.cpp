// Host test: g++ -O2 -std=c++17 -I<components dir> tests/test_uplink_sample.cpp -o /tmp/tu && /tmp/tu
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "voice_assistant_websocket/uplink_sample.h"
#include "voice_assistant_websocket/heap_diag.h"
using namespace esphome::voice_assistant_websocket;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
  // gain 1 == the old conversion, for EVERY int32 input.
  uint64_t bad = 0;
  for (int64_t s = INT32_MIN; s <= INT32_MAX; s++) {
    if (uplink_sample((int32_t) s, 1) != static_cast<int16_t>(((int32_t) s) >> 16)) bad++;
  }
  CHECK(bad == 0);
  CHECK(uplink_sample(0x00010000, 64) == 64);
  CHECK(uplink_sample(-0x00010000, 64) == -64);
  CHECK(uplink_sample(0, 512) == 0);
  // finer than 16-bit resolution survives: 0x400 * 64 >> 16 = 1
  CHECK(uplink_sample(0x400, 64) == 1);
  // saturation
  CHECK(uplink_sample(INT32_MAX, 2) == 32767);
  CHECK(uplink_sample(INT32_MIN, 2) == -32768);
  CHECK(uplink_sample(INT32_MAX, 512) == 32767);
  CHECK(uplink_sample(INT32_MIN, 512) == -32768);
  CHECK(uplink_sample(0x01000000, 64) == 16384);
  CHECK(uplink_sample(0x01000000, 128) == 32767);
  CHECK(uplink_sample(-0x01000000, 128) == -32768);
  CHECK(uplink_sample(-0x01000000, 64) == -16384);
  // diag frame
  char b[200];
  int n = heap_diag_format(b, sizeof b, "start", 12, 3, 40000, 30000, 20000, 7000000, 6000000, 1, 123);
  CHECK(n > 0 && n < (int) sizeof b);
  CHECK(strcmp(b, "{\"type\":\"diag\",\"ev\":\"start\",\"up\":12,\"rr\":3,\"if\":40000,\"im\":30000,\"ib\":20000,\"pf\":7000000,\"pm\":6000000,\"ok\":1,\"us\":123}") == 0);
  // worst-case widths still fit in the 200-byte stack buffer
  n = heap_diag_format(b, sizeof b, "audio", 4294967295u, 15, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 0, 4294967295u);
  CHECK(n > 0 && n < 200);
  printf("%s (worst-case frame %d bytes)\n", fails ? "FAILED" : "ALL PASS", n);
  return fails;
}
