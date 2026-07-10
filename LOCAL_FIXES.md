# Local fixes (fork: magic9418/ha-openai-realtime, branch `local-fixes`)

Personal fork of [`fjfricke/ha-openai-realtime`](https://github.com/fjfricke/ha-openai-realtime)
carrying low-risk fixes for the "Jarvis" local-first voice assistant. Upstream was validated
on real HA Voice Preview Edition hardware but has known issues; this branch fixes the
compile/config-ergonomics ones that need no hardware.

## Applied fixes

1. **Stale micro_wake_word model URL** — `voice_pe_config.yaml` used an old
   `github.com/kahrendt/microWakeWord/releases/download/...json` URL that ESPHome 2025.11
   rejects. Replaced with the microWakeWord v2 github shorthand so it compiles out of the box.

2. **Configurable wake word** — the primary wake word model is now a
   `substitutions: wake_word_model` variable (default `okay_nabu`), referenced by the
   `micro_wake_word:` block. Change it in one place. Accepted v2 model names documented in a
   YAML comment (`okay_nabu`, `hey_jarvis`, `hey_mycroft`, `hey_rhasspy`).

3. **`auto_stop_inactivity_ms` is now a config option** — was a hard-coded
   `AUTO_STOP_INACTIVITY_MS` constant in `voice_assistant_websocket.h`. Now a proper ESPHome
   component schema option (`positive_time_period_milliseconds`, default `120s`) wired through
   codegen to a `set_auto_stop_inactivity_ms()` setter.

4. **Custom "Neo" wake word model** — added `home-assistant-voice-pe/wake_words/neo.{tflite,json}`,
   a microWakeWord v2 int8 streaming model trained 2026-07-10 for the single word **"Neo"**
   (7,480 piper-TTS positives + microWakeWord negative sets). `wake_word_model` now defaults to
   `wake_words/neo.json`, and the `hey_jarvis`/`hey_mycroft` extra triggers are commented out so
   the device wakes only to "Neo". Quality (quantized streaming): at cutoff 0.98 ≈ 92% detection /
   ~0.45 false-accepts per hour. **Not yet hardware-validated** — trained on synthetic audio; expect
   a tune/retrain pass after a real flash-test, and bump `tensor_arena_size` if ESPHome errors.
   To revert to a stock trigger, set `wake_word_model: okay_nabu`.

## Known TODO (out of scope here — need hardware / deeper work)

- **WebSocket drop / server-restart recovery** — ✅ FIXED (5). Reworked for the session-server
  architecture: instead of a fragile reconnect-and-resume, any unexpected drop or connect error
  now cleanly returns the device to IDLE so the wake word re-arms ("Neo" is the retry). Disabled
  the ESP-IDF client's built-in auto-reconnect (it fought this and left the device "connecting");
  route DISCONNECTED/ERROR through the loop's `pending_disconnect_ → IDLE` path. This covers both
  the old "upstream reconnect/backoff" and "reconnect on server restart" items: sessions are short
  and stateless (a fresh OpenAI session per wake), so resume has no benefit; never-stuck does.
- **micro_wake_word re-arm after auto-stop** — resolved in practice; wake-from-red works reliably
  after the `on_wake_word_detected` rework + on_disconnected re-arm.
- **Context restore on reconnect** — N/A under the session-server model (one stateless session per
  wake). Revisit only if long-lived sessions are added.
