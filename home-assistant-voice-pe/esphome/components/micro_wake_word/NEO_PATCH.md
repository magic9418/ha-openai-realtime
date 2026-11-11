# micro_wake_word held-duck patch

Voice PE uses ESPHome 2026.6.5's built-in micro_wake_word. This directory is an UNMODIFIED copy of that
component (from ghcr.io/esphome/esphome:2026.6.5, /usr/src/esphome/esphome/components/micro_wake_word)
vendored only so the patch below can exist; a local external component of the same name overrides the
built-in. To drop the fork: delete this directory (and the `capture_wake_trigger` score arguments).
One addition on top of the upstream copy:
`MicroWakeWord::get_last_detection_average()` / `get_last_detection_max()` (uint8, 0-255 on the cutoff
scale), set from the `DetectionEvent` immediately before `wake_word_detected_trigger_.trigger()`.

Used by `config/common/neo_voice.yaml` (`on_wake_word_detected`) -> `va_ws.capture_wake_trigger(...)`
-> `wake_trigger_start.detector_score` / `detector_score_max`. No behaviour change to detection:
three assignments + two getters + two members. `average` is the sliding-window mean already computed
by `WakeWordModel::determine_detected()` (integer sum / window, so 1/255 resolution).

# Stage 2 + feature history (2026-11-10, branch feat/wake-stage2)

Same additions as the Satellite1 copy (see its NEO_PATCH.md for the full description): feature history ring,
second-stage runner (`wake_stage2.{h,cpp}`, `wake_features.h`, self-test headers), `on_wake_word_rejected`
trigger, runtime mode/threshold/regime setters, inference task stack 3072 -> 4096. All new memory is PSRAM
(10,500 B history + 40,000 B arena). Fail-open. The PE copy keeps its pre-existing differences from the Sat1
copy (no atomic cutoff queue, blocking detection-queue send, no [neo-raw]/[neo-mic] diagnostics).

Self-test (v3): expected outputs are the TFLM reference values (-39,-95,-69); tolerance +-12 per vector, plus "not all three identical".
ESP-NN's optimised kernels on the ESP32-S3 differ from the reference by a few LSB on these extreme synthetic inputs (measured
on the Voice PE: got -32,-92,-69, dev 7), so the test only catches gross failure. Real-data parity is checked offline from the
uploaded features (device score vs offline score on the same features). Log: `selftest=PASS got a,b,c dev=N`.
