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
  bool turn_has_no_reply_audio() const {
    return this->is_running() && this->last_speaker_audio_time_ == 0;
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
  void send_text_frame_(const char *json);  // Send a JSON control text frame (shared by wake/flush/etc.)
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
  void audio_ring_init_();
  void audio_ring_push_(const uint8_t *data, size_t len);
  void audio_ring_drain_();
  void audio_ring_clear_();
  
  // Timing
  uint32_t last_audio_send_{0};
  uint32_t last_audio_receive_{0};
  static const uint32_t AUDIO_SEND_INTERVAL_MS = 100;  // Send 100ms chunks
  static const uint32_t MICROPHONE_SAMPLE_RATE = 16000;  // 16kHz from microphone (required by micro_wake_word)
  static const uint32_t INPUT_SAMPLE_RATE = 24000;       // 24kHz for OpenAI input (non-beta API requirement)
  static const uint32_t OUTPUT_SAMPLE_RATE = 24000;    // 24kHz for OpenAI output
  static const uint32_t BYTES_PER_SAMPLE = 2;          // 16-bit = 2 bytes
  static const uint32_t INPUT_BUFFER_SIZE = (INPUT_SAMPLE_RATE * BYTES_PER_SAMPLE * AUDIO_SEND_INTERVAL_MS) / 1000;
  
  // Auto-stop tracking
  uint32_t last_speaker_audio_time_{0};  // Last time we received audio from speaker
  // Stop after N ms of speaker (bot) inactivity. Configurable via the
  // `auto_stop_inactivity_ms` YAML option; default set here matches the schema default.
  uint32_t auto_stop_inactivity_ms_{120000};
  
  // Audio conversion buffers
  std::vector<int16_t> mono_buffer_;  // For stereo to mono conversion (input)
  std::vector<int16_t> resampled_buffer_;  // For 16kHz -> 24kHz resampling (1.5x upsampling)
  std::vector<uint8_t> output_stereo_buffer_;  // For output processing (24kHz mono -> 48kHz stereo, 16-bit)
  
  bool pending_start_{false};
  bool pending_disconnect_{false};  // Flag to disconnect in loop() (cannot be called from websocket task)
  bool reconnect_pending_{false};
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

