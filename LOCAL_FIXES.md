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

## Known TODO (out of scope here — need hardware / deeper work)

- **Upstream WebSocket reconnect / backoff** — reconnect logic to the OpenAI Realtime relay is
  fragile; needs proper exponential backoff + jitter.
- **Satellite reconnect on server restart** — the ESP32 satellite does not reliably re-establish
  its session when the add-on/relay server restarts.
- **micro_wake_word re-arm after auto-stop** — wake word detection does not always re-arm after
  an auto-stop, leaving the device unresponsive until reboot.
- **Context restore on reconnect** — conversation context is lost across a reconnect; should be
  restored so a dropped session resumes gracefully.
