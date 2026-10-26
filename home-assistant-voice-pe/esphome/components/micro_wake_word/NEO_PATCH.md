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
