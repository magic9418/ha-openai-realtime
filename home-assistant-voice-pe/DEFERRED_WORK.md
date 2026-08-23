# Voice PE — deferred work & the wake-word blocker (2026-07-17)

## ⚠️ PENDING FLASH (2026-08-23): code ahead of the device

Five commits are on `feat/wake-guards-flywheel` but **NOT yet flashed** to the office PE
(192.168.5.29), which is still on the 2026-08-02 build.

**Web-flash image built 2026-08-23** (flash at offset `0x0`; md5 `449cea5e7e2bcd214f0bb9a70a8e478e`):
`neo-voice-pe-20260823-sat1-parity.factory.bin`, 1,936,560 B. Rebuild with
`poetry run esphome compile voice_pe_config.yaml`; the factory bin lands at
`.esphome/build/ha-voice-openai/.pioenvs/ha-voice-openai/firmware.factory.bin`.
(OTA — `poetry run esphome upload voice_pe_config.yaml` — also works, but the PE is web-flashed
by preference.)

### 2026-08-23 — parity with the current Sat1 build (satellite1-neo @ 1861537)
- **Continuous mic look-back ring** (`preroll_*` + the `{"type":"preroll","ms":N}` frame). Fixes the
  front-of-command clip. A buffer armed at the wake **cannot** fix this: micro_wake_word only reports
  "Neo" after the whole word has passed its window, so in a run-together "Neo play some alt music"
  the words "play some" are spoken inside the detection latency, before `start()` ever runs — hence a
  ring that fills continuously while idle. The server already handles its absence (`preroll_ms = 0`),
  so this is additive: the PE is not broken without it, just clipping.
- **`running_since_` as the auto-stop inactivity fallback** (Sat1 `221ad39`, never ported until now).
  The timer only armed after the first reply chunk, so a bare wake with no reply left the session
  open forever and wedged the next wake ("WebSocket client already exists").
- **`running_since_` re-base on barge-in.** Must ship WITH the item above, never after it:
  `interrupt()` zeroes `last_speaker_audio_time_`, which now falls through to `running_since_` =
  session start, so any session older than the timeout would auto-stop the instant you barge in.
- **Server `{"type":"reboot"}` handler — present but DISABLED**, matching the Sat1, so the Console's
  Reboot button is a no-op on both devices rather than one of each.
- **`wifi: power_save_mode: none`.** ESPHome's `LIGHT` default parks the radio between beacons and
  the Sendspin heartbeat then misses its 15 s PONG deadline — that is what dropped Sat1 music every
  ~15 min. The PE gained Sendspin in `2e23b38`, so it has the same exposure.

### Earlier, still unflashed (2026-08-02)
- **Barge-in reply pitch fix** — `process_received_audio_` now re-asserts `set_audio_stream_info(24kHz)`
  when restarting the speaker after a barge-in (else the follow-up reply plays 2× / high-pitched).
- **Silent wake** — removed the wake chime (`play_sound(wake_word_triggered_sound)`); Neo wakes silent
  on all HW per Matt's preference.
- **Sendspin / `speaker_source` migration** (Stage B) and a **session-scoped wake cutoff** so
  barge-in works again.
- Already flashed 2026-08-02: server-driven `auto_stop_inactivity_ms` from the hello frame. Sat1-only
  fixes (mic uplink 16k, red-LED no-HA check) do NOT apply to the PE.

**Compiles clean** on esphome 2026.6.5 / ESP-IDF 5.5.4 — verify the version before every build
(see the toolchain trap at the bottom of this file).

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
