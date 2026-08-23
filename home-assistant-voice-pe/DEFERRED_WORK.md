# Voice PE — deferred work & the wake-word blocker (2026-07-17)

## ⚠️ PENDING FLASH (2026-08-23): code ahead of the device

Everything below is committed to the PE firmware but **NOT yet flashed** to the office PE
(192.168.5.29). The office PE has been offline; this branch brings it to **Sat1 parity** so both
rooms run the same voice behaviour.

Ported from `satellite1-neo` on 2026-08-23 (`voice_assistant_websocket` is a shared lineage, so
these are the Sat1 commits applied to the PE copy):
- **Session auto-close with no reply** (Sat1 `221ad39`) — `running_since_` is the auto-stop
  reference until the bot speaks. Before: a bare wake with no reply left the session open forever,
  which then wedged the next wake ("WebSocket client already exists").
- **Barge-in no longer auto-stops the session** (Sat1 `3b0d381`) — the interrupt path re-bases
  `running_since_`, so saying "Neo" over a reply on a session older than `auto_stop_inactivity_ms`
  no longer closes it the moment you stop talking.
- **Mic look-back / pre-roll** (Sat1 `3e31cbd` + `1861537`) — a 1 s circular PSRAM ring filled
  continuously while idle, flushed oldest-first at the wake, announced to the server as
  `{"type":"preroll","ms":N}`. Fixes the run-together "Neo play some alternative music" losing
  "play some" to micro_wake_word's detection latency. The server (jonnyAI `1aecbc6`) already
  measures the wake instant on the audio timeline and needs this frame to place it.
- **Server `{"type":"reboot"}` handler** (Sat1 `ff17435`) — present but **deliberately disabled**,
  exactly as on the Sat1, so the Console's Reboot button is a no-op until that build is re-tested.

Config-side parity, same date:
- **`power_save_mode: none`** — the Sendspin PONG-deadline dropout fix (Sat1 `ce2a721`). The PE now
  plays music through Sendspin, so it is exposed to the same ~15-min "player unavailable".
- **Wake cutoff keyed on session OR playback** (Sat1 `f3caf6d`) — one `update_wake_sensitivity`
  script replaces the inline per-hook lambdas, so the strict idle cutoff no longer applies while
  music is playing with no session open.
- **Build label** `project.version: neo-2026.08.23-sat1parity` — so "what is flashed?" is a log line.

Still pending from 2026-08-02 (unchanged, also not flashed):
- **Barge-in reply pitch fix** — `process_received_audio_` re-asserts `set_audio_stream_info(24kHz)`
  when restarting the speaker after a barge-in.
- **Silent wake** — the wake chime is gone; Neo wakes silent on all hardware.
- **Sendspin / `speaker_source` migration** and the session-scoped wake cutoff.
Already flashed earlier (2026-08-02): server-driven `auto_stop_inactivity_ms` from the hello frame.
Sat1-only fixes (mic uplink 16k, red-LED no-HA check) do NOT apply to the PE.

**Not compiled or flashed yet** — this port was written off-device. Build before flashing:
`poetry run esphome compile voice_pe_config.yaml`, then
`poetry run esphome upload voice_pe_config.yaml` (OTA to .29).

---


We hit a hard blocker debugging the PE and are rolling back to a known-good firmware to get a
working device, then re-applying the work below methodically. This file is the memory of *what*
we were fixing and *what we learned*, so nothing is lost.

## THE BLOCKER (solve this first — it gates everything else)

`micro_wake_word` fails at boot: **`Failed to allocate tensors for the streaming model`**, for
**every** model tried — custom Neo (v2), stock `okay_nabu` (v2), and stock `okay_nabu` **v1**.
So it is **NOT the model** and **NOT our app code** (it fails early in boot, before the
`voice_assistant_websocket` component even sets up, and after we reverted all our audio edits).

Facts from the device log (`esphome logs voice_pe_config.yaml --device 192.168.5.29`):
- ESPHome **2025.11.4**, ESP-IDF **5.5.1**, ESP32-S3, PSRAM 8 MB available.
- **Free heap ~106 KB** at boot (low — something consumes a lot of internal RAM).
- mww: STARTING → DETECTING_WAKE_WORD → error on first inference → STOPPED → "Failed to allocate
  tensors." A wake-word watchdog re-arms it every ~15 s; it fails again identically.
- The user confirms the **same Neo model worked before** → this is a build/toolchain regression,
  not the model.

### ROOT CAUSE FOUND (2026-07-17): ESPHome version downgrade.
The known-good bin (device log, 192.168.5.29) was built with **ESPHome 2026.6.5 / ESP-IDF 5.5.4**
(compiled 2026-07-12 21:23) — Neo v2 loads and allocates tensors fine there, on even LESS free
heap (~84 KB). Our failing builds run **ESPHome 2025.11.4** (ESP-IDF 5.5.1), which has the v2
tensor-arena bug (esphome#15603/#7242). The `pyproject.toml` pin `esphome = "^2025.11.4"` caps us
below 2026.x, so `poetry install` resolved DOWN to the buggy 2025.11.4. Bumping `tensor_arena_size`
and the v1-model workaround did nothing because the bug is in the 2025.11.4 toolchain, not the model.

### THE FIX
Pin **`esphome = "2026.6.5"`** in `pyproject.toml`, `poetry lock && poetry install`, rebuild.
That's the exact toolchain proven working on the device now. Then re-apply the deferred fixes below
on top of it (they were never the problem).

## DEFERRED FIXES WE BUILT (re-apply after the PE has a working wake word)

### Firmware (branch `feat/wake-guards-flywheel`; commits 657a3f3, 400bf7c + uncommitted)
- **PSRAM ring buffer** for the audio backlog — replaces the per-chunk `std::vector` queue that
  fragmented internal DRAM and **OOM-crashed the PE on long playback** ("read a book"). Structural,
  good. (657a3f3)
- **micro_wake_word.start on boot** — HA-independent wake start (these PEs are divorced from HA, so
  the old `api.on_client_connected` gate never fired). **Confirmed working** — mww *starts* now;
  it's only the tensor-alloc bug that stops it. (400bf7c)
- **Enrollment mic-streaming** — `on_microphone_data_` streams during enrollment so the recorder
  captures reps (fixes empty enrollment WAVs). (657a3f3)
- **Wake-word barge-in / half-duplex** + `NEO_FULL_DUPLEX_BARGEIN` compile toggle — say "Neo" to
  interrupt; no cross-talk break-ins. Reverted Matt's full-duplex experiment. (uncommitted/657a3f3)
- **Reverted** Matt's barge-in audio diff (full-duplex, **AGC/NS removal**, speaker buffer bumps
  100→300/500→2000 ms) — it caused cross-talk break-ins AND the buffer bumps were (wrongly)
  suspected in the mww OOM. Back to 7/12 known-good audio config.

### Server (repo jonnyAI — DONE + DEPLOYED to the box)
- **Enrollment coach fixes** (commit a3a86a7): coach audio was inaudible (raw blob overflowed the
  PE speaker queue) → routed through the paced `play_pcm`; model muted during enrollment; TTS
  failures now logged. Recorder records at true 24 kHz.
- **Long-form reader / "read a book"** (commit 8c4c84d): `reader.py` chunks text → prefetch TTS →
  paced, back-pressured playback; `read_aloud` neo-tool; "Neo" stops it; model muted while reading.
  Needs the firmware PSRAM ring buffer (done) for hours-long reads without OOM.

## PROBLEMS THESE ADDRESSED (the original goals)
1. Wake-word flywheel (validated end-to-end server-side; retrain harness fixed).
2. Empty enrollment WAVs / "teach me my voice" not working.
3. Silent enrollment coach.
4. Cross-talk break-ins when others talk nearby.
5. PE crash on very long replies (pe-long-tts-crash).
6. "Neo, read me a whole book."
7. Wake word not starting without Home Assistant.

## STATE
- **Server-side work: done + deployed** (box). Safe to keep.
- **Firmware work: blocked** on the wake-word tensor-allocation toolchain bug. Get a working PE
  base first (flash a known-good old bin, or rebuild a clean pre-flywheel commit with a pinned
  toolchain + reduced RAM pressure), THEN re-apply the firmware fixes above one at a time,
  flashing + checking the device log between each.
