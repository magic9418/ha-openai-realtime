# Option 2 — route the Voice PE through `session-server` (the target architecture)

**Decision (2026-07-10):** the Voice PE must run through the custom **`session-server`** bench, *not*
the bundled `openai_realtime_voice_agent` add-on. The add-on path is a throwaway test of the "Neo"
wake word only. This doc is the switch-over plan.

## Why
`session-server` (the "Jarvis/Neo" bench, on the GPU box `192.168.5.101:8090`) is the reusable core that
also drives the browser Talk page. Routing the Voice PE through it gives the device everything the add-on
path does **not**:

- **Neo persona** (`docs/system_prompt.md`)
- **Daily budget cap** + per-turn cost accounting
- **Per-turn JSONL logging** + the **Console** (live transcript, tool calls, latency)
- **Curated tool surface** via `mcp-proxy` (HA MCP now; Hermes/Google later)

The add-on runs its own OpenAI Realtime session with none of the above.

## Why it's easy: the protocols are the same
Both the ESP32 firmware (`voice_assistant_websocket`) and `session-server`'s `/ws/audio` speak the
**identical** wire protocol:

| Direction | Binary frames | Text frames |
|-----------|---------------|-------------|
| device → server | raw **PCM16, 24 kHz, mono** mic audio | JSON control, e.g. `{"type":"interrupt"}` |
| server → device | raw **PCM16, 24 kHz, mono** playback audio | JSON control |

The add-on is just a Pipecat wrapper (`WebsocketServerTransport` + a `RawAudioSerializer` that emits raw
PCM) around exactly this. So **no firmware rewrite and no protocol bridge are needed** — only a repoint.

## The switch (one line)
In `home-assistant-voice-pe/secrets.yaml`, change `server_url` from the add-on to session-server:

```yaml
# was (add-on path): server_url: "ws://192.168.5.15:8080"
server_url: "ws://192.168.5.101:8090/ws/audio"
```

Re-flash (or OTA) and you're on the real architecture. The wake word stays "Neo" either way.

## Server-side work (already done)
`session-server` `/ws/audio` already accepts raw PCM16 24 kHz binary + JSON control. The one gap —
the firmware's explicit barge-in `{"type":"interrupt"}` — was added (jonnyAI repo commit `8c2e068`:
`{"type":"interrupt"}` → `response.cancel`). Nothing else was required for a first connection.

## Open items / validate on hardware
- **Needs a real device pointed at `:8090`** to confirm end-to-end (verified by protocol inspection only).
- Far-field **server-VAD tuning** (turn detection thresholds) once we hear it in the room.
- **Session lifecycle**: each wake opens a fresh WS → fresh Realtime session (matches the add-on's model).
- Optional later: a server→device `disconnect_client` control message (add-on has a tool for it; not
  required for basic operation).
- The firmware's deferred hard fixes still apply — see `LOCAL_FIXES.md` (WS reconnect/backoff, satellite
  reconnect, micro_wake_word re-arm, context restore).
