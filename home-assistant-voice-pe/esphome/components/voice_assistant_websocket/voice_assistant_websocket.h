#pragma once

#include "esphome.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#ifdef USE_ESP_IDF
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // audio_ring_lock_ mutex (serialize ring drain vs push/clear/stop)
#include "esp_system.h"
#include "esp_heap_caps.h"   // heap_caps_malloc for the PSRAM audio ring buffer
#endif
#include <string>
#include <vector>
#include <queue>

// Barge-in policy toggle (see on_microphone_data_):
//   0 = DEFAULT half-duplex — mic muted while Neo speaks; barge-in is via the wake word ("Neo").
//       Robust in noisy / multi-person rooms (no cross-talk phantom turns).
//   1 = FULL-DUPLEX — stream mic continuously so you can talk over Neo to interrupt. Only for a
//       quiet, single-person room (relies on XMOS AEC to reject Neo's own playback).
#ifndef NEO_FULL_DUPLEX_BARGEIN
#define NEO_FULL_DUPLEX_BARGEIN 0
#endif

namespace esphome {
namespace voice_assistant_websocket {

enum VoiceAssistantWebSocketState {
  VOICE_ASSISTANT_WEBSOCKET_IDLE = 0,
  VOICE_ASSISTANT_WEBSOCKET_STARTING,
  VOICE_ASSISTANT_WEBSOCKET_RUNNING,
  VOICE_ASSISTANT_WEBSOCKET_STOPPING,
  VOICE_ASSISTANT_WEBSOCKET_ERROR,
  VOICE_ASSISTANT_WEBSOCKET_DISCONNECTED
};

class VoiceAssistantWebSocket : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // INVARIANT: the uplink sender task must never outlive the component it sends for.
  // App.safe_reboot() (and ESPHome shutdown) call this before the chip resets, so this is the one
  // place the task is asked to exit and is joined.
  void on_shutdown() override;

  void set_server_url(const std::string &url) { this->server_url_ = url; }
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
  void set_speaker(speaker::Speaker *spkr) { this->speaker_ = spkr; }
  // Auto-stop the session after this many ms of speaker (bot) inactivity.
  // Configurable via the `auto_stop_inactivity_ms` component option (see __init__.py).
  void set_auto_stop_inactivity_ms(uint32_t ms) { this->auto_stop_inactivity_ms_ = ms; }
  
  void start();
  void stop();
  void request_start();
  void interrupt();  // Send interrupt message to server and stop speaker

  // Wake-boundary / flywheel control messages (see docs/firmware_guards_plan.md &
  // docs/wakeword_flywheel_plan.md). Each sends a JSON text frame to the server;
  // old servers ignore unknown types. Contract is pinned to the session-server.
  void send_false_flag();     // button double-press: "that was a false trigger", any time
  void send_button_cancel();  // fast single-press cancel shortly after a wake w/ no reply audio yet

  // Server-driven chime->start delay (from the {"type":"hello"} frame). Defaults to
  // the YAML substitution value if the server never sends a hello.
  uint32_t get_wake_open_delay_ms() const { return this->wake_open_delay_ms_; }

  bool is_running() const { return this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING; }
  bool is_connected() const { return this->websocket_client_ != nullptr && esp_websocket_client_is_connected(this->websocket_client_); }
  bool is_bot_speaking() const;  // Check if bot is currently speaking (within 500ms of last audio)
  // True while a turn is still "open with no reply audio yet" — i.e. we woke and started a
  // session but the bot hasn't produced any speaker audio. Gates button_cancel (a fast
  // cancel here means the wake was unwanted); once reply audio has played it's a real turn.
  //
  // INVARIANT: this asks "has this SESSION ever produced reply audio?", which is NOT the same
  // question as the auto-stop inactivity reference. It used to read last_speaker_audio_time_ == 0,
  // but interrupt() deliberately zeroes that timestamp (to drop the half-duplex guard and re-base
  // the inactivity clock for the follow-up turn). So after ANY barge-in a genuine turn looked like
  // "no reply audio yet", and a button press mislabelled it a false wake - poisoning the wake-word
  // training flywheel with false negatives. reply_audio_seen_this_session_ is latched independently
  // and cleared only at session start.
  bool turn_has_no_reply_audio() const {
    return this->is_running() && !this->reply_audio_seen_this_session_;
  }
  
  void set_state_callback(std::function<void(VoiceAssistantWebSocketState)> &&callback) {
    this->state_callback_ = std::move(callback);
  }
  
  // Automation triggers
  Trigger<> *get_connected_trigger() { return &this->connected_trigger_; }
  Trigger<> *get_disconnected_trigger() { return &this->disconnected_trigger_; }
  Trigger<> *get_error_trigger() { return &this->error_trigger_; }
  Trigger<> *get_stopped_trigger() { return &this->stopped_trigger_; }
  // Enrollment mode enter/exit — the YAML wires these to micro_wake_word.stop/start
  // (disarm the wake + stop models so guided reps don't self-trigger) while the C++
  // owns the streaming/mic-pin state + the 15-min safety cap and WS-drop restore.
  Trigger<> *get_enroll_start_trigger() { return &this->enroll_start_trigger_; }
  Trigger<> *get_enroll_stop_trigger() { return &this->enroll_stop_trigger_; }
  bool is_enrolling() const { return this->enrolling_; }

 protected:
  void connect_websocket_();
  void disconnect_websocket_();
  // Send a JSON control text frame (shared by wake/flush/etc.).
  // INVARIANT: a frame sent from the MAIN LOOP or the MIC TASK must pass a bounded ticks_to_wait.
  // portMAX_DELAY on the main task parks every other ESPHome component (LEDs, mixer, watchdog)
  // behind one TCP write; on the mic task it starves micro_wake_word's producer ring.
  void send_text_frame_(const char *json, TickType_t ticks_to_wait = portMAX_DELAY);
  void enter_enrollment_();                 // pin mic open, disarm wake+stop models
  void exit_enrollment_();                  // restore normal wake/stop-model operation
  void send_audio_chunk_(const uint8_t *data, size_t len);
  void process_received_audio_(const uint8_t *data, size_t len);
  void on_microphone_data_(const std::vector<uint8_t> &data);
  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  
  std::string server_url_;
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  
#ifdef USE_ESP_IDF
  esp_websocket_client_handle_t websocket_client_{nullptr};
#else
  void *websocket_client_{nullptr};
#endif
  VoiceAssistantWebSocketState state_{VOICE_ASSISTANT_WEBSOCKET_IDLE};
  
  std::function<void(VoiceAssistantWebSocketState)> state_callback_;
  
  // Automation triggers
  Trigger<> connected_trigger_{};
  Trigger<> disconnected_trigger_{};
  Trigger<> error_trigger_{};
  Trigger<> stopped_trigger_{};
  Trigger<> enroll_start_trigger_{};
  Trigger<> enroll_stop_trigger_{};

  // ---- Deferred YAML-automation dispatch ---------------------------------------------------
  // INVARIANT: a Trigger<> (and state_callback_) is only ever fired from loop(), i.e. the MAIN
  // task. ESP-IDF delivers websocket events on the client's own 8 KB task with the client lock
  // held, and the YAML wired to these triggers calls main-loop-only APIs - mixer_speaker
  // .apply_ducking (non-atomic gain writes racing the mixer task), the LED ring's perform() via
  // control_leds, media_player.stop, micro_wake_word.stop/start. Firing them from the websocket
  // event handler risked a stack overflow and held the client lock across arbitrary YAML - which
  // is also the lock the microphone uplink needs. stopped_trigger_ was already dispatched this
  // way; this generalises the same pattern to the other five and to state_callback_.
  //
  // Ordering is preserved: a FIFO, drained oldest-first, nothing coalesced. EV_STATE_CALLBACK
  // carries the state observed WHEN THE EVENT HAPPENED, not the state at dispatch time.
  enum class DeferredEvent : uint8_t {
    EV_STATE_CALLBACK,
    EV_CONNECTED,
    EV_DISCONNECTED,
    EV_ERROR,
    EV_ENROLL_START,
    EV_ENROLL_STOP,
    EV_STOPPED,
  };
  struct DeferredEntry {
    DeferredEvent event;
    VoiceAssistantWebSocketState state;  // payload; only meaningful for EV_STATE_CALLBACK
  };
  static const size_t DEFERRED_QUEUE_LEN = 16;
  DeferredEntry deferred_[DEFERRED_QUEUE_LEN]{};
  size_t deferred_head_{0};   // next write slot
  size_t deferred_tail_{0};   // next read slot
  size_t deferred_count_{0};  // entries held
  uint32_t deferred_overflow_{0};
  SemaphoreHandle_t deferred_lock_{nullptr};
  // Producers: websocket task and main task. Consumer: main task only.
  void defer_event_(DeferredEvent event, VoiceAssistantWebSocketState state = VOICE_ASSISTANT_WEBSOCKET_IDLE);
  void dispatch_deferred_();  // main task only; never holds deferred_lock_ across a trigger

  
  // Audio buffers
  std::vector<uint8_t> input_buffer_;
  std::vector<uint8_t> output_buffer_;
  
  // Assistant-audio backlog (audio arriving faster than the speaker drains it). Held in a fixed
  // PSRAM RING BUFFER, not a std::queue<vector>: the old per-chunk vector queue alloc/free'd on the
  // scarce ~300 KB internal DRAM, which fragmented it and OOM-crashed the PE on long playback
  // (reading a whole book). Pre-allocated ONCE from the 8 MB PSRAM → no per-chunk allocation, no
  // fragmentation, safe for unbounded-length reads. The server paces ~real-time, so fill stays low.
  static const size_t AUDIO_RING_CAPACITY = 96 * 1024;   // ~2s @24kHz mono16 of jitter headroom (kept modest so it never competes with micro_wake_word's tensor arena for memory)
  uint8_t *audio_ring_{nullptr};
  size_t audio_ring_read_{0};   // read cursor
  size_t audio_ring_fill_{0};   // bytes currently buffered
  // The ring is touched from TWO tasks: the main loop() drains it into the speaker, while the
  // websocket task pushes new audio AND (on barge-in / server interrupt) clears it + stops the
  // speaker. Unserialized, a clear that zeroes fill mid-drain made `fill -= written` underflow a
  // size_t → the drain looped on stale bytes forever ("loop audio" after a break-in) and the free
  // report ran away past capacity. This mutex serializes every ring mutation and the speaker
  // stop/clear against the drain's speaker_->play(). play() is non-blocking, so holding it is cheap.
  SemaphoreHandle_t audio_ring_lock_{nullptr};
  void audio_ring_init_();
  void audio_ring_push_(const uint8_t *data, size_t len);
  void audio_ring_drain_();
  void audio_ring_clear_();
  void audio_ring_flush_and_stop_speaker_();   // barge-in: stop speaker + empty ring, atomically

  // Closed-loop audio flow control (docs/long_reply_flow_control_plan.md). The server pre-fills only
  // into advertised ring headroom, so the ring never overflows (no dropped/garbled audio) and
  // buffering is bounded no matter how long the reply. Enabled only when the server's hello carries
  // "flow_control":"credit" (old servers → stays open-loop, server time-paces as before).
  bool flow_control_enabled_{false};
  size_t audio_free_reported_fill_{0};   // ring fill at the last report (hysteresis reference)
  static const size_t AUDIO_FREE_REPORT_QUANTUM = AUDIO_RING_CAPACITY / 4;  // report every ~0.5s drained/filled
  void report_audio_free_(bool force);   // send {"type":"audio_free","bytes":<capacity-fill>}

  // Timing
  uint32_t last_audio_send_{0};
  uint32_t last_audio_receive_{0};
  static const uint32_t AUDIO_SEND_INTERVAL_MS = 100;  // Send 100ms chunks
  static const uint32_t MICROPHONE_SAMPLE_RATE = 16000;  // 16kHz from microphone (required by micro_wake_word)
  static const uint32_t INPUT_SAMPLE_RATE = 24000;       // 24kHz for OpenAI input (non-beta API requirement)
  static const uint32_t OUTPUT_SAMPLE_RATE = 24000;    // 24kHz for OpenAI output
  static const uint32_t BYTES_PER_SAMPLE = 2;          // 16-bit = 2 bytes
  static const uint32_t INPUT_BUFFER_SIZE = (INPUT_SAMPLE_RATE * BYTES_PER_SAMPLE * AUDIO_SEND_INTERVAL_MS) / 1000;

  // Mic LOOK-BACK — the uplink counterpart of the assistant-audio ring above (ported from Sat1,
  // 2026-08-21).
  //
  // A buffer armed by start() cannot fix the front-of-command clip: micro_wake_word can only report
  // "Neo" once the whole word has passed through its sliding window, so start() runs a few hundred ms
  // AFTER the user finished saying it. In a run-together "Neo play some alternative music" the words
  // "play some" are spoken INSIDE that detection latency — before start(). Nor can wake_open_delay_ms
  // help: every one of these gates sits downstream of a wake that has, by construction, already
  // happened.
  //
  // So this is a CIRCULAR look-back that fills continuously while idle (the mic callback is
  // registered unconditionally in setup() and micro_wake_word keeps the mic running, so the frames
  // are already arriving — we simply used to discard them). At the wake we already hold the preceding
  // PREROLL_MS, wake word and all, and flush it oldest-first ahead of the live stream.
  //
  // INVARIANT: the ring holds only CONTIGUOUS, recently-heard room audio. Every path that drops a
  // frame (bot speaking, mic gate) resets it, so a flush can never prepend Neo's own reply echo or
  // a stale fragment from minutes ago onto the next command.
  static const uint32_t PREROLL_MS = 1000;                     // look-back depth: wake word + detection latency
  static const uint32_t BYTES_PER_MS_24K = (INPUT_SAMPLE_RATE * BYTES_PER_SAMPLE) / 1000;  // 48 B/ms
  static const size_t PREROLL_CAPACITY = PREROLL_MS * BYTES_PER_MS_24K;                    // 48 KB
  static const size_t PREROLL_CHUNK = INPUT_BUFFER_SIZE;       // flush in normal-sized frames
  uint8_t *preroll_{nullptr};
  size_t preroll_fill_{0};            // bytes held (saturates at PREROLL_CAPACITY)
  size_t preroll_head_{0};            // write cursor; once full this is also the OLDEST byte
  // preroll_head_/preroll_fill_ are written by the MIC task (push/flush) and reset from the MAIN
  // task (init) and the WEBSOCKET task (stop()). Unserialized, a reset landing between the head
  // update and the fill update in preroll_push_ resurrects a stale fill over a rewound head, and a
  // flush would then send bytes that were never contiguous - breaking the ring's one invariant.
  // A DEDICATED mutex, NOT audio_ring_lock_: that lock is deliberately held across the whole
  // speaker drain, and making the 16 kHz mic callback queue behind it would recreate the very
  // mic-task stall the uplink sender task exists to remove.
  // INVARIANT: this lock is never held across a network send. Flush snapshots under the lock,
  // releases, and only then hands bytes to the uplink ring.
  SemaphoreHandle_t preroll_lock_{nullptr};
  void preroll_init_();
  void preroll_push_(const uint8_t *data, size_t len);
  void preroll_flush_();
  void preroll_reset_();

  // ---- Uplink ring + sender task -----------------------------------------------------------
  // INVARIANT: the microphone callback NEVER blocks on the network. It runs on the shared,
  // high-priority i2s mic task that also feeds micro_wake_word's ~120 ms producer ring; every
  // millisecond spent inside esp_websocket_client_send_bin(..., portMAX_DELAY) is a millisecond
  // mww is not being fed, and once that ring overflows mww RESETS it - the device goes deaf. That
  // happened on every connect (preroll_flush_ pushed up to 48 KB in a loop) and on every session
  // end (the client lock is held for up to 1 s inside close()); a WiFi stall made it unbounded.
  //
  // So the mic callback only memcpys into this PSRAM ring (never blocking, drop-OLDEST when full)
  // and one dedicated low-priority task drains it with a BOUNDED send timeout.
  //
  // Capacity must comfortably exceed PREROLL_CAPACITY (48 KB), or a look-back flush could drop its
  // own oldest bytes - which are exactly the wake word and the front of the command the look-back
  // exists to rescue. 96 KB is ~2 s @ 24 kHz mono16 (the required minimum is ~500 ms).
  static const size_t UPLINK_RING_CAPACITY = 96 * 1024;
  static const size_t UPLINK_SEND_CHUNK = INPUT_BUFFER_SIZE;   // 100 ms per websocket frame
  static const uint32_t UPLINK_SEND_TIMEOUT_MS = 200;          // bounded; never portMAX_DELAY
  static const uint32_t UPLINK_DROP_LOG_INTERVAL_MS = 5000;    // rate-limit the drop report
  uint8_t *uplink_ring_{nullptr};      // PSRAM
  uint8_t *uplink_stage_{nullptr};     // internal-RAM copy-out, so the send holds no lock at all
  size_t uplink_read_{0};
  size_t uplink_fill_{0};
  SemaphoreHandle_t uplink_lock_{nullptr};  // guards uplink_read_/uplink_fill_ only (tiny sections)
  SemaphoreHandle_t uplink_wake_{nullptr};  // "data available" / "please exit" signal
  TaskHandle_t uplink_task_{nullptr};
  volatile bool uplink_task_exit_{false};
  volatile bool uplink_task_running_{false};
  volatile bool uplink_paused_{false};  // set while disconnect_websocket_() tears the client down
  uint32_t uplink_dropped_bytes_{0};
  uint32_t uplink_drop_events_{0};
  uint32_t uplink_last_drop_log_{0};
  void uplink_init_();                                    // main task, from setup(); creates once
  void uplink_enqueue_(const uint8_t *data, size_t len);  // mic task; never blocks
  void uplink_reset_();                                   // drop a dead session's queued audio
  void uplink_shutdown_();                                // main task; asks the sender to exit
  void uplink_run_();                                     // sender task body
  static void uplink_task_fn_(void *param);

  // Serializes the uplink sender's use of websocket_client_ against disconnect_websocket_()'s
  // destroy of it. Taken with a BOUNDED timeout on both sides: the main loop must never sit behind
  // a stalled TCP write. The WEBSOCKET task must never take this lock at all - it would deadlock
  // against esp_websocket_client_close(), which joins that very task.
  SemaphoreHandle_t ws_client_lock_{nullptr};

  // Auto-stop tracking
  uint32_t last_speaker_audio_time_{0};  // Last time we received audio from speaker
  // "Did THIS session ever produce reply audio?" - latched true on the first speaker chunk and
  // cleared only in start(). Deliberately NOT derived from last_speaker_audio_time_, which
  // interrupt() zeroes on every barge-in (see turn_has_no_reply_audio()).
  bool reply_audio_seen_this_session_{false};
  uint32_t running_since_{0};  // millis() the session went RUNNING; auto-stop reference when the bot
                               // hasn't spoken yet, so a bare wake with no reply still auto-closes.
  // Stop after N ms of speaker (bot) inactivity. Configurable via the
  // `auto_stop_inactivity_ms` YAML option; default set here matches the schema default.
  uint32_t auto_stop_inactivity_ms_{120000};
  
  // Audio conversion buffers
  std::vector<int16_t> mono_buffer_;  // For stereo to mono conversion (input)
  std::vector<int16_t> resampled_buffer_;  // For 16kHz -> 24kHz resampling (1.5x upsampling)
  std::vector<uint8_t> output_stereo_buffer_;  // For output processing (24kHz mono -> 48kHz stereo, 16-bit)
  
  bool pending_start_{false};
  bool pending_disconnect_{false};  // Flag to disconnect in loop() (cannot be called from websocket task)
  bool pending_reboot_{false};  // Flag to reboot in loop() (server {"type":"reboot"} frame; cannot reboot from websocket task)
  bool reconnect_pending_{false};
  // Set alongside reconnect_pending_ when connect_websocket_() has to tear a stale client down
  // before it can honour a FRESH WAKE. It has to be a separate flag because both websocket event
  // handlers deliberately clear reconnect_pending_ on any drop/error ("no auto-reconnect on a
  // network blip") - and the drop they clear it during is the very teardown the wake is queued
  // behind. Cleared only by loop(), when it actually promotes the queued wake to a reconnect.
  bool wake_reconnect_queued_{false};
  bool explicit_disconnect_{false};  // Flag to prevent reconnection after explicit disconnect
  uint32_t reconnect_attempts_{0};
  static const uint32_t MAX_RECONNECT_ATTEMPTS = 5;
  static const uint32_t RECONNECT_DELAY_MS = 5000;
  uint32_t last_reconnect_attempt_{0};
  uint32_t interrupt_time_{0};  // Time when interrupt was sent (to ignore audio for a short period)
  static const uint32_t INTERRUPT_IGNORE_AUDIO_MS = 250;  // Ignore mic for 250ms after interrupt (was 500 — too long, ate the front of the user's command). Band-aid for echo; can go lower once AEC/AGC fix lands.

  // Wake-boundary / server-driven chime->start delay. Overwritten by the {"type":"hello",
  // "wake_open_delay_ms":N} frame the server sends on WS open; falls back to this default
  // (matches the YAML ${wake_open_delay_ms} substitution) if no hello arrives.
  uint32_t wake_open_delay_ms_{320};

  // Mic-forward gate: drop uplink frames until this millis() timestamp. Set on every wake
  // (start() for a cold wake, interrupt() for a barge-in re-wake) to wake_open_delay_ms_ from now,
  // so the wake chime doesn't bleed into ASR — while the WS connect runs *underneath* it instead
  // of after it. Lets the mic open ~wake_open_delay_ms after wake regardless of connect latency,
  // instead of chime + delay + connect (which ate the front of the user's command). 0 = no gate.
  uint32_t mic_gate_until_{0};

  // Enrollment mode: mic pinned open + streaming, wake/stop models disarmed by YAML.
  // Auto-restores on {"enroll","stop"}, WS drop, or the 15-min safety cap.
  bool enrolling_{false};
  uint32_t enroll_start_time_{0};
  static const uint32_t ENROLL_MAX_MS = 15 * 60 * 1000;  // 15-min hard safety cap
};

// Action classes for automations (defined outside the main class)
template<typename... Ts> class VoiceAssistantWebSocketStartAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketStartAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketStopAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketStopAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

// Condition classes for automations (defined outside the main class)
template<typename... Ts> class VoiceAssistantWebSocketIsRunningCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsRunningCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketIsConnectedCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsConnectedCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketIsBotSpeakingCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsBotSpeakingCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_bot_speaking(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketInterruptAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketInterruptAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->interrupt(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketFalseFlagAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketFalseFlagAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->send_false_flag(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketButtonCancelAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketButtonCancelAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->send_button_cancel(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

// Condition: turn is open with no reply audio yet (gates the button_cancel path in YAML).
template<typename... Ts> class VoiceAssistantWebSocketTurnHasNoReplyAudioCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketTurnHasNoReplyAudioCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->turn_has_no_reply_audio(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

}  // namespace voice_assistant_websocket
}  // namespace esphome

