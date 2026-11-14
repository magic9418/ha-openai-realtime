#pragma once
#include <cstdint>
#include <cstdio>
namespace esphome::voice_assistant_websocket {
// Heap-diagnostic frame (development): all integers, built into a caller-provided stack buffer.
// ev: "start" | "wake" | "audio" | "tick" | "end". wh/uh/sh/mh/lh: stack high-water marks (bytes never used) of the
// websocket client / uplink / i2s speaker / i2s mic / main loop task; -1 = unavailable. Returns snprintf's result.
inline int heap_diag_format(char *buf, size_t n, const char *ev, uint32_t up, int rr, uint32_t if_, uint32_t im,
                            uint32_t ib, uint32_t pf, uint32_t pm, int ok, uint32_t us, int wh = -1, int uh = -1, int sh = -1, int mh = -1, int lh = -1) {
  return snprintf(buf, n,
                  "{\"type\":\"diag\",\"ev\":\"%s\",\"up\":%u,\"rr\":%d,\"if\":%u,\"im\":%u,\"ib\":%u,"
                  "\"pf\":%u,\"pm\":%u,\"ok\":%d,\"us\":%u,\"wh\":%d,\"uh\":%d,\"sh\":%d,\"mh\":%d,\"lh\":%d}",
                  ev, (unsigned) up, rr, (unsigned) if_, (unsigned) im, (unsigned) ib, (unsigned) pf, (unsigned) pm,
                  ok, (unsigned) us, wh, uh, sh, mh, lh);
}
}  // namespace esphome::voice_assistant_websocket
