#include "voice_assistant_websocket.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include <cstring>
#include <algorithm>
#include <queue>

#ifdef USE_ESP_IDF
#include "esp_system.h"
#endif

static const char *TAG = "voice_assistant_websocket";

namespace esphome {
namespace voice_assistant_websocket {

void VoiceAssistantWebSocket::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Assistant WebSocket...");
  this->input_buffer_.reserve(INPUT_BUFFER_SIZE);
  this->output_buffer_.reserve(4096);  // Reserve space for output buffer
  this->mono_buffer_.reserve(INPUT_BUFFER_SIZE / 2);  // Reserve for mono conversion (input)
  this->resampled_buffer_.reserve(INPUT_BUFFER_SIZE * 3 / 2); // 1.5x upsampling for 16kHz -> 24kHz
  this->output_stereo_buffer_.reserve(4096 * 2);  // Reserve for output processing (24kHz mono -> 48kHz stereo)
  this->audio_ring_init_();  // pre-allocate the PSRAM audio backlog ring (see header)
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
  
  // Register microphone data callback
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data);
    });
  }
}

void VoiceAssistantWebSocket::loop() {
  // Handle pending disconnect (must be done in main task, not websocket task)
  if (this->pending_disconnect_) {
    this->pending_disconnect_ = false;
    this->disconnect_websocket_();
    // After disconnect, continue with stop() cleanup
    // Clear buffers
    this->input_buffer_.clear();
    this->output_buffer_.clear();
    
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
    this->reconnect_attempts_ = 0;
    this->reconnect_pending_ = false;
    
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
    
    // Trigger stopped automation
    this->stopped_trigger_.trigger();
    
    ESP_LOGI(TAG, "Voice Assistant WebSocket stopped");
    return;  // Skip other loop operations after disconnect
  }
  
  // Drain the assistant-audio backlog (PSRAM ring) into the speaker as it frees up.
  if (this->speaker_ != nullptr && this->speaker_->is_running() && this->audio_ring_fill_ > 0) {
    this->audio_ring_drain_();
  }
  
  // Handle pending start request
  if (this->pending_start_ && this->state_ == VOICE_ASSISTANT_WEBSOCKET_IDLE) {
    this->pending_start_ = false;
    this->start();
  }
  
  // Handle reconnection (only if not pending disconnect and websocket client is cleaned up)
  if (this->reconnect_pending_ && 
      !this->pending_disconnect_ &&
      this->websocket_client_ == nullptr &&
      (millis() - this->last_reconnect_attempt_) > RECONNECT_DELAY_MS &&
      this->reconnect_attempts_ < MAX_RECONNECT_ATTEMPTS) {
    this->reconnect_pending_ = false;
    this->last_reconnect_attempt_ = millis();
    this->reconnect_attempts_++;
    ESP_LOGW(TAG, "Attempting to reconnect (attempt %u/%u)...", this->reconnect_attempts_, MAX_RECONNECT_ATTEMPTS);
    this->connect_websocket_();
  }
  
  // Auto-stop: Check if we should stop after inactivity
  // Stop if: speaker hasn't spoken for 5 seconds
  // Note: We only check speaker audio, not microphone audio, because:
  // - Microphone always sends audio (background noise, silence, etc.)
  // - OpenAI's server_vad handles voice activity detection
  // - If user speaks, OpenAI will generate new audio, which resets the timer
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    uint32_t current_time = millis();
    uint32_t time_since_speaker_audio = current_time - this->last_speaker_audio_time_;
    
    // Only check if we've received at least one audio chunk (to avoid stopping immediately)
    if (this->last_speaker_audio_time_ > 0) {
      // Stop if speaker hasn't spoken for 5 seconds
      // If user speaks during this time, OpenAI will generate new audio, resetting the timer
      if (time_since_speaker_audio > this->auto_stop_inactivity_ms_) {
        ESP_LOGI(TAG, "Auto-stopping: Speaker inactive for %u ms (threshold: %u ms)",
                 time_since_speaker_audio, this->auto_stop_inactivity_ms_);
        // Timer/auto-stop close: drop any half-sentence still sitting in the server's
        // input buffer *at the cut-off source* so a later wake can't "complete" it.
        // Only on this timer path — NOT on user-interrupt or wake (per the guards plan;
        // a reactive clear-on-wake disturbs the server VAD). Sent while still connected,
        // before stop() tears the WS down.
        this->send_text_frame_("{\"type\":\"mic_flush\"}");
        this->stop();
      }
    }
  }
  
  // Enrollment safety cap: never leave the mic pinned open / wake disarmed indefinitely.
  // (Normal exit is {"enroll","stop"} or a WS drop; this is the backstop.)
  if (this->enrolling_ && (millis() - this->enroll_start_time_) > ENROLL_MAX_MS) {
    ESP_LOGW(TAG, "Enrollment safety cap (%u ms) hit - exiting enrollment mode", ENROLL_MAX_MS);
    this->exit_enrollment_();
  }

  // Audio input is handled via callback (on_microphone_data_)
  // No need to poll here
  
  // Audio output is handled directly in process_received_audio_()
  // No queue processing needed here
}

void VoiceAssistantWebSocket::dump_config() {
  ESP_LOGCONFIG(TAG, "Voice Assistant WebSocket:");
  ESP_LOGCONFIG(TAG, "  Server URL: %s", this->server_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Microphone Sample Rate: %u Hz", MICROPHONE_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Input Sample Rate (after resampling): %u Hz", INPUT_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Output Sample Rate: %u Hz", OUTPUT_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->microphone_ ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Audio ring buffer: %zu bytes (PSRAM)", AUDIO_RING_CAPACITY);
}

void VoiceAssistantWebSocket::start() {
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    ESP_LOGW(TAG, "Already running");
    return;
  }
  
  ESP_LOGI(TAG, "Starting Voice Assistant WebSocket...");
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_STARTING;

  // Reset auto-stop tracking
  this->last_speaker_audio_time_ = 0;

  // Arm the mic-forward gate for the wake chime. The YAML now calls start() BEFORE the chime, so
  // connect_websocket_() runs underneath the chime instead of after it — the mic opens ~this delay
  // after wake (protecting ASR from the chime) instead of chime + delay + connect.
  this->mic_gate_until_ = millis() + this->wake_open_delay_ms_;
  
  // Reset explicit disconnect flag for new session
  this->explicit_disconnect_ = false;
  
  // Reset interrupt time
  this->interrupt_time_ = 0;
  
  // Start microphone first (if not already running)
  // Note: micro_wake_word also uses this microphone, so it might already be running
  if (this->microphone_ != nullptr) {
    if (this->microphone_->is_stopped()) {
      this->microphone_->start();
    } else {
      ESP_LOGD(TAG, "Microphone already running (likely used by micro_wake_word)");
    }
  }
  
  // Start speaker - the resampler will handle format conversion
  if (this->speaker_ != nullptr) {
    // IMPORTANT: Set audio stream info BEFORE starting the speaker!
    // The resampler uses audio_stream_info_ to determine the input sample rate.
    // OpenAI sends 24kHz, 16-bit, mono audio - let the resampler convert to 48kHz
    audio::AudioStreamInfo input_stream_info(16, 1, 24000);  // 16-bit, mono, 24kHz (OpenAI output)
    this->speaker_->set_audio_stream_info(input_stream_info);
    
    // Only start speaker if it's not already running
    // For streaming audio, we want continuous playback without restarting
    if (this->speaker_->is_stopped()) {
      this->speaker_->start();
    }
  }
  
  if (this->state_callback_) {
    this->state_callback_(this->state_);
  }
  
  this->connect_websocket_();
}

void VoiceAssistantWebSocket::stop() {
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_IDLE) {
    return;
  }
  
  ESP_LOGI(TAG, "Stopping Voice Assistant WebSocket...");
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_STOPPING;
  
  // Don't stop microphone - micro_wake_word needs it to continue running
  // The microphone can be shared between multiple components in ESPHome
  // micro_wake_word will continue to work even when voice_assistant_websocket is stopped
  ESP_LOGD(TAG, "Keeping microphone running for micro_wake_word");
  // Stop speaker if it's running
  if (this->speaker_ != nullptr) {
    this->speaker_->stop();
  }
  
  // Clear the audio backlog ring
  this->audio_ring_clear_();

  if (this->state_callback_) {
    this->state_callback_(this->state_);
  }
  
  // IMPORTANT: Cannot call disconnect_websocket_() from websocket task/event handler
  // Set flag to disconnect in loop() instead (which runs in main task)
  this->pending_disconnect_ = true;
  
  // Note: Rest of cleanup (buffers, state, triggers) will be done in loop() after disconnect
}

void VoiceAssistantWebSocket::request_start() {
  this->pending_start_ = true;
}

void VoiceAssistantWebSocket::connect_websocket_() {
  if (this->websocket_client_ != nullptr) {
    ESP_LOGW(TAG, "WebSocket client already exists, cleaning up...");
    // Use pending_disconnect_ instead of direct call to avoid blocking
    // Set reconnect_pending_ so we retry after disconnect completes
    this->pending_disconnect_ = true;
    this->reconnect_pending_ = true;
    this->last_reconnect_attempt_ = millis();  // Reset timer so we retry after disconnect
    return;  // Exit early, will retry connection after disconnect completes in loop()
  }
  
  if (this->server_url_.empty()) {
    ESP_LOGE(TAG, "Server URL not set!");
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
    return;
  }
  
  ESP_LOGI(TAG, "Connecting to WebSocket server: %s", this->server_url_.c_str());
  
  esp_websocket_client_config_t websocket_cfg = {};
  websocket_cfg.uri = this->server_url_.c_str();
  websocket_cfg.user_context = this;
  websocket_cfg.buffer_size = 4096;
  websocket_cfg.task_prio = 5;
  websocket_cfg.task_stack = 8192;
  websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;  // Use TCP (not SSL) for ws://
  websocket_cfg.network_timeout_ms = 30000;  // 30 second timeout for network operations
  websocket_cfg.reconnect_timeout_ms = 10000;  // (unused: auto-reconnect disabled below)
  // We handle recovery ourselves: any unexpected drop cleanly returns the device to IDLE
  // (wake word re-arms; "Neo" is the retry). Sessions are short + stateless, so the ESP-IDF
  // client's built-in auto-reconnect just fights that and leaves the device "connecting".
  websocket_cfg.disable_auto_reconnect = true;
  websocket_cfg.ping_interval_sec = 20;  // Send ping every 20 seconds (matches server)
  websocket_cfg.pingpong_timeout_sec = 10;  // 10 second timeout for pong (matches server)
  
  this->websocket_client_ = esp_websocket_client_init(&websocket_cfg);
  if (this->websocket_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
    return;
  }
  
  // Register event handler
  esp_websocket_register_events(this->websocket_client_, 
                                 (esp_websocket_event_id_t) WEBSOCKET_EVENT_ANY,
                                 websocket_event_handler_,
                                 this);
  
  // Start connection
  esp_err_t err = esp_websocket_client_start(this->websocket_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
  }
}

void VoiceAssistantWebSocket::disconnect_websocket_() {
  if (this->websocket_client_ != nullptr) {
    ESP_LOGI(TAG, "Disconnecting WebSocket...");
    
    // Check if client is actually connected before trying graceful close
    bool is_connected = esp_websocket_client_is_connected(this->websocket_client_);
    
    if (is_connected) {
      // Try graceful close first (sends close frame)
      // Use shorter timeout (1 second) to avoid blocking too long
      esp_err_t close_err = esp_websocket_client_close(this->websocket_client_, pdMS_TO_TICKS(1000));
      if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "Graceful close failed (%s), forcing stop", esp_err_to_name(close_err));
        // Fallback to immediate stop if graceful close fails
        esp_websocket_client_stop(this->websocket_client_);
      }
    } else {
      // Client not connected, just stop and destroy immediately
      ESP_LOGD(TAG, "Client not connected, stopping immediately");
      esp_websocket_client_stop(this->websocket_client_);
    }
    
    // Always destroy the client to free resources
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
  }
}

void VoiceAssistantWebSocket::send_audio_chunk_(const uint8_t *data, size_t len) {
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    return;
  }
  
  // Send binary data
  int sent = esp_websocket_client_send_bin(this->websocket_client_, 
                                            (const char *) data, 
                                            len, 
                                            portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send audio chunk");
  }
}

void VoiceAssistantWebSocket::process_received_audio_(const uint8_t *data, size_t len) {
  // Use speaker directly (media_player uses this speaker internally via announcement_pipeline)
  // The media_player is configured in YAML but we access the speaker directly for PCM audio streaming
  if (this->speaker_ == nullptr) {
    ESP_LOGW(TAG, "Speaker is null, cannot play audio");
    return;
  }
  
  // Don't try to play audio if speaker is not ready (still initializing)
  // The speaker will retry automatically, so we just skip audio for now
  if (this->state_ != VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    ESP_LOGD(TAG, "Skipping audio playback - voice assistant not in running state");
    return;
  }
  
  // Ignore audio for a short period after interrupt to allow server to process it
  if (this->interrupt_time_ > 0) {
    uint32_t time_since_interrupt = millis() - this->interrupt_time_;
    if (time_since_interrupt < INTERRUPT_IGNORE_AUDIO_MS) {
      ESP_LOGD(TAG, "Ignoring audio after interrupt (%u ms remaining)", 
               INTERRUPT_IGNORE_AUDIO_MS - time_since_interrupt);
      return;  // Drop audio packets for a short time after interrupt
    } else {
      // Reset interrupt time after ignore period
      this->interrupt_time_ = 0;
      ESP_LOGI(TAG, "Resuming audio processing after interrupt");
    }
  }
  
  // OpenAI sends 24kHz, 16-bit, mono PCM
  // The resampler is configured for 48kHz output and will automatically convert 24kHz -> 48kHz
  // We set audio_stream_info to 24kHz in start(), so the resampler knows the input sample rate
  // ESPHome will then convert 16-bit to 32-bit and mono to stereo for I2S
  
  // Ensure speaker is running before sending audio
  // For streaming audio, we want continuous playback
  if (this->speaker_->is_stopped()) {
    ESP_LOGD(TAG, "Speaker is stopped, starting it");
    this->speaker_->start();
  }
  
  // FIFO through the PSRAM ring: append the new audio, then drain as much as the speaker will
  // accept. Strict order (never plays new bytes ahead of buffered backlog) with ZERO per-chunk
  // heap allocation — this is the fix for the long-playback (read-a-book) OOM crash.
  this->last_speaker_audio_time_ = millis();
  this->audio_ring_push_(data, len);
  this->audio_ring_drain_();
}

// ---- PSRAM audio backlog ring buffer (see header) ----------------------------------------
void VoiceAssistantWebSocket::audio_ring_init_() {
  if (this->audio_ring_ != nullptr) return;
#ifdef USE_ESP_IDF
  this->audio_ring_ = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_RING_CAPACITY, MALLOC_CAP_SPIRAM));
  if (this->audio_ring_ == nullptr) {
    ESP_LOGE(TAG, "PSRAM audio ring alloc failed (%zu bytes); trying internal", AUDIO_RING_CAPACITY);
    this->audio_ring_ = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_RING_CAPACITY, MALLOC_CAP_8BIT));
  }
#else
  this->audio_ring_ = static_cast<uint8_t *>(malloc(AUDIO_RING_CAPACITY));
#endif
  if (this->audio_ring_ == nullptr) {
    ESP_LOGE(TAG, "Audio ring allocation failed — backlogged audio will be dropped");
  }
  this->audio_ring_read_ = 0;
  this->audio_ring_fill_ = 0;
}

void VoiceAssistantWebSocket::audio_ring_push_(const uint8_t *data, size_t len) {
  if (this->audio_ring_ == nullptr || data == nullptr || len == 0) return;
  size_t avail = AUDIO_RING_CAPACITY - this->audio_ring_fill_;
  if (len > avail) {
    // Backlog full. Server paces ~real-time, so this is rare jitter, not steady state — drop the
    // chunk rather than ever growing memory. (Bounded by construction: this is the OOM guard.)
    ESP_LOGW(TAG, "Audio ring full (%zu/%zu), dropping %zu bytes", this->audio_ring_fill_,
             AUDIO_RING_CAPACITY, len);
    return;
  }
  size_t write = (this->audio_ring_read_ + this->audio_ring_fill_) % AUDIO_RING_CAPACITY;
  size_t first = std::min(len, AUDIO_RING_CAPACITY - write);   // bytes until wrap
  memcpy(this->audio_ring_ + write, data, first);
  if (len > first) memcpy(this->audio_ring_, data + first, len - first);
  this->audio_ring_fill_ += len;
}

void VoiceAssistantWebSocket::audio_ring_drain_() {
  if (this->audio_ring_ == nullptr || this->speaker_ == nullptr) return;
  while (this->audio_ring_fill_ > 0) {
    size_t contiguous = std::min(this->audio_ring_fill_, AUDIO_RING_CAPACITY - this->audio_ring_read_);
    size_t written = this->speaker_->play(this->audio_ring_ + this->audio_ring_read_, contiguous);
    if (written == 0) break;   // speaker buffer full — retry next loop()
    this->audio_ring_read_ = (this->audio_ring_read_ + written) % AUDIO_RING_CAPACITY;
    this->audio_ring_fill_ -= written;
  }
}

void VoiceAssistantWebSocket::audio_ring_clear_() {
  this->audio_ring_read_ = 0;
  this->audio_ring_fill_ = 0;
}

void VoiceAssistantWebSocket::on_microphone_data_(const std::vector<uint8_t> &data) {
  // Only process if connected. Normally we stream only during a RUNNING session — BUT during
  // enrollment ("teach me my voice") there is no wake-session: the mic is pinned open with wake
  // disarmed, and the server needs every rep frame to build the training WAV. Without the
  // `enrolling_` exception the enrollment recorder captured ~nothing (empty WAVs).
  if (!this->is_connected() ||
      (this->state_ != VOICE_ASSISTANT_WEBSOCKET_RUNNING && !this->enrolling_)) {
    return;
  }
  
  // Barge-in policy (see NEO_FULL_DUPLEX_BARGEIN in the header):
  //  * DEFAULT (half-duplex): don't stream the mic while Neo is speaking, so room cross-talk /
  //    echo isn't committed as user input. Full-duplex flooded noisy rooms with phantom turns
  //    (a person talking nearby got treated as commands). Barge-in is via the WAKE WORD instead:
  //    micro_wake_word keeps listening locally during the reply, and saying "Neo" fires
  //    voice_assistant_websocket.interrupt (on_wake_word_detected in the YAML).
  //  * FULL-DUPLEX build (quiet single-person room): define NEO_FULL_DUPLEX_BARGEIN 1 to drop the
  //    guard and stream continuously so you can talk over Neo to interrupt (relies on XMOS AEC).
  // Enrollment pins the mic open regardless of this guard.
#if !NEO_FULL_DUPLEX_BARGEIN
  if (this->is_bot_speaking() && !this->enrolling_) {
    return;
  }
#endif

  // Wake-chime gate: drop uplink for the wake-open window after a wake so the chime doesn't bleed
  // into ASR. The WS connect overlaps this window, so the mic opens ~wake_open_delay_ms after wake
  // (not chime + delay + connect). Enrollment pins the mic open regardless.
  if (!this->enrolling_ && this->mic_gate_until_ != 0) {
    if ((int32_t)(millis() - this->mic_gate_until_) < 0) {
      return;
    }
    this->mic_gate_until_ = 0;  // window elapsed — stop paying the compare every frame
  }

  // Microphone is configured for 16kHz, 32-bit, stereo (required by micro_wake_word)
  // OpenAI expects 24kHz, 16-bit, mono (non-beta API requirement)
  // Convert: 32-bit stereo -> 16-bit mono (16kHz) -> resample to 24kHz
  
  size_t stereo_32bit_samples = data.size() / (4 * 2);  // 4 bytes per 32-bit sample, 2 channels
  size_t mono_16khz_samples = stereo_32bit_samples;
  
  if (this->mono_buffer_.size() < mono_16khz_samples) {
    this->mono_buffer_.resize(mono_16khz_samples);
  }
  
  const int32_t *stereo_32bit = reinterpret_cast<const int32_t *>(data.data());
  int16_t *mono_16bit = this->mono_buffer_.data();
  
  for (size_t i = 0; i < stereo_32bit_samples; i++) {
    int32_t left_sample = stereo_32bit[i * 2];
    mono_16bit[i] = static_cast<int16_t>((left_sample >> 16));
  }
  
  // Resample from 16kHz to 24kHz (1.5x upsampling)
  size_t resampled_24khz_samples = (mono_16khz_samples * INPUT_SAMPLE_RATE) / MICROPHONE_SAMPLE_RATE;
  if (this->resampled_buffer_.size() < resampled_24khz_samples) {
    this->resampled_buffer_.resize(resampled_24khz_samples);
  }
  
  int16_t *resampled_24khz = this->resampled_buffer_.data();
  
  // Linear interpolation resampling: 16kHz -> 24kHz
  for (size_t i = 0; i < resampled_24khz_samples; i++) {
    float source_pos = (float)i * (float)MICROPHONE_SAMPLE_RATE / (float)INPUT_SAMPLE_RATE;
    size_t source_idx = (size_t)source_pos;
    float fraction = source_pos - source_idx;
    
    if (source_idx + 1 < mono_16khz_samples) {
      int16_t sample0 = mono_16bit[source_idx];
      int16_t sample1 = mono_16bit[source_idx + 1];
      resampled_24khz[i] = static_cast<int16_t>(sample0 + (sample1 - sample0) * fraction);
    } else if (source_idx < mono_16khz_samples) {
      resampled_24khz[i] = mono_16bit[source_idx];
    } else {
      resampled_24khz[i] = mono_16bit[mono_16khz_samples - 1];
    }
  }
  
  size_t resampled_bytes = resampled_24khz_samples * BYTES_PER_SAMPLE;
  this->send_audio_chunk_(reinterpret_cast<const uint8_t *>(resampled_24khz), resampled_bytes);
}

bool VoiceAssistantWebSocket::is_bot_speaking() const {
  // Bot is considered speaking if we received audio within the last 500ms
  if (this->last_speaker_audio_time_ == 0) {
    return false;  // No audio received yet
  }
  uint32_t time_since_last_audio = millis() - this->last_speaker_audio_time_;
  return time_since_last_audio < 500;  // 500ms threshold
}

void VoiceAssistantWebSocket::send_text_frame_(const char *json) {
  // Shared JSON text-frame sender for all control messages (interrupt / wake / mic_flush /
  // false_flag / button_cancel). No-op (with a warning) if the WS isn't connected.
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send control frame '%s' - not connected", json);
    return;
  }
  int sent = esp_websocket_client_send_text(this->websocket_client_, json, strlen(json), portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send control frame: %s", json);
  } else {
    ESP_LOGI(TAG, "Sent control frame: %s", json);
  }
}

void VoiceAssistantWebSocket::send_false_flag() {
  // Button double-press → "that was a false trigger". Valid any time there's a live WS;
  // the server relabels the newest wake probe as a hard negative for retraining.
  ESP_LOGI(TAG, "false_flag: user marked the last wake as a false trigger");
  this->send_text_frame_("{\"type\":\"false_flag\"}");
}

void VoiceAssistantWebSocket::send_button_cancel() {
  // Fast single-press cancel shortly after a wake with no reply audio yet → treat as an
  // unwanted wake. Gate here too (defense in depth) so a press during a real turn doesn't
  // mislabel it; the YAML also checks turn_has_no_reply_audio before calling this.
  if (!this->turn_has_no_reply_audio()) {
    ESP_LOGD(TAG, "button_cancel ignored: turn already has reply audio (real turn)");
    return;
  }
  ESP_LOGI(TAG, "button_cancel: fast cancel after wake with no reply audio yet");
  this->send_text_frame_("{\"type\":\"button_cancel\"}");
}

void VoiceAssistantWebSocket::enter_enrollment_() {
  if (this->enrolling_) {
    ESP_LOGD(TAG, "Already enrolling");
    return;
  }
  ESP_LOGI(TAG, "Entering enrollment mode: mic pinned open, wake/stop models disarmed");
  this->enrolling_ = true;
  this->enroll_start_time_ = millis();
  // Make sure the mic is actually running so reps stream to the backend. The YAML
  // enroll_start trigger disarms micro_wake_word so the reps don't self-trigger.
  if (this->microphone_ != nullptr && this->microphone_->is_stopped()) {
    this->microphone_->start();
  }
  this->enroll_start_trigger_.trigger();
}

void VoiceAssistantWebSocket::exit_enrollment_() {
  if (!this->enrolling_) {
    return;
  }
  ESP_LOGI(TAG, "Exiting enrollment mode: restoring normal wake/stop-model operation");
  this->enrolling_ = false;
  this->enroll_start_time_ = 0;
  // The YAML enroll_stop trigger re-arms micro_wake_word.
  this->enroll_stop_trigger_.trigger();
}

void VoiceAssistantWebSocket::interrupt() {
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send interrupt - not connected");
    return;
  }

  ESP_LOGI(TAG, "Sending interrupt message to server");

  // Send interrupt message as JSON text frame
  const char *interrupt_msg = "{\"type\":\"interrupt\"}";
  int sent = esp_websocket_client_send_text(this->websocket_client_, interrupt_msg, strlen(interrupt_msg), portMAX_DELAY);

  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send interrupt message");
  } else {
    ESP_LOGI(TAG, "Interrupt message sent successfully");
    // Stop speaker immediately after sending interrupt
    if (this->speaker_ != nullptr) {
      this->speaker_->stop();
    }
    // Drop the audio backlog so buffered speech stops immediately on interrupt
    this->audio_ring_clear_();
    // Open the mic for the follow-up NOW: clear last_speaker_audio_time_ so is_bot_speaking()
    // (the half-duplex guard) reads false immediately instead of staying true ~500ms after the
    // last server frame — that lingering guard ate the front of the barge-in follow-up command.
    // (Consistent with start(), which also zeroes it to re-arm auto-stop for the fresh turn.)
    this->last_speaker_audio_time_ = 0;
    // But still gate the barge-in chime out of ASR for the wake-open window.
    this->mic_gate_until_ = millis() + this->wake_open_delay_ms_;
    // Set interrupt time to ignore incoming audio for a short period
    // This gives the server time to process the interrupt and stop sending audio
    this->interrupt_time_ = millis();
    ESP_LOGI(TAG, "Cleared audio queue and ignoring incoming audio for %u ms", INTERRUPT_IGNORE_AUDIO_MS);
  }
}

void VoiceAssistantWebSocket::websocket_event_handler_(void *handler_args, 
                                                       esp_event_base_t base, 
                                                       int32_t event_id, 
                                                       void *event_data) {
  VoiceAssistantWebSocket *instance = static_cast<VoiceAssistantWebSocket *>(handler_args);
  esp_websocket_event_id_t ws_event_id = (esp_websocket_event_id_t) event_id;
  esp_websocket_event_data_t *ws_event_data = (esp_websocket_event_data_t *) event_data;
  
  instance->handle_websocket_event_(ws_event_id, ws_event_data);
}

void VoiceAssistantWebSocket::handle_websocket_event_(esp_websocket_event_id_t event_id, 
                                                      esp_websocket_event_data_t *event_data) {
  switch (event_id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
      ESP_LOGI(TAG, "WebSocket connection attempt starting...");
      break;
      
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected");
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_RUNNING;
      this->reconnect_attempts_ = 0;
      this->reconnect_pending_ = false;
      this->last_audio_send_ = millis();

      // Wake boundary: a fresh session always begins with start() → connect → CONNECTED.
      // Tell the server "a wake just happened" so it can arm the dangling-VAD guard and
      // (flywheel) ring-buffer the opening audio as a probe. Sent here rather than in
      // start() because that's the earliest point the WS can actually carry the frame.
      // (A re-wake mid-session doesn't reconnect, so this is one wake per session — a
      // known limitation noted in docs/wakeword_flywheel_plan.md.)
      this->send_text_frame_("{\"type\":\"wake\"}");

      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }

      // Trigger connected automation
      this->connected_trigger_.trigger();
      break;
      
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_DISCONNECTED;

      // Never leave the box stuck in enrollment if the session drops mid-coach.
      this->exit_enrollment_();
      
      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }
      
      // Trigger disconnected automation (re-arms micro_wake_word via on_disconnected).
      this->disconnected_trigger_.trigger();

      // Any drop — server restart, network blip, or our own stop() — cleanly returns to
      // IDLE via loop() so the wake word works again. No auto-reconnect: sessions are short
      // and stateless, so "Neo" is the retry. (This can't be done from the WS task; the
      // loop's pending_disconnect_ handler destroys the client and sets IDLE.)
      this->explicit_disconnect_ = false;
      this->reconnect_pending_ = false;
      this->pending_disconnect_ = true;
      break;
      
    case WEBSOCKET_EVENT_DATA:
      if (event_data->op_code == 0x02) {  // Binary frame
        this->process_received_audio_(reinterpret_cast<const uint8_t *>(event_data->data_ptr), event_data->data_len);
      } else if (event_data->op_code == 0x01) {  // Text frame
        ESP_LOGI(TAG, "Received text message: %.*s", event_data->data_len, event_data->data_ptr);
        
        // Handle JSON control messages
        std::string message((const char *) event_data->data_ptr, event_data->data_len);
        if (message.find("\"type\":\"interrupt\"") != std::string::npos ||
            message.find("\"type\": \"interrupt\"") != std::string::npos) {
          ESP_LOGI(TAG, "Interrupt received, stopping speaker");
          if (this->speaker_ != nullptr) {
            this->speaker_->stop();
          }
        } else if (message.find("\"type\":\"disconnect\"") != std::string::npos ||
                   message.find("\"type\": \"disconnect\"") != std::string::npos) {
          ESP_LOGI(TAG, "Disconnect message received, stopping voice assistant and going to idle");
          // Mark that we received an explicit disconnect to prevent reconnection
          this->explicit_disconnect_ = true;
          // Stop the voice assistant (will go to idle mode)
          this->stop();
        } else if (message.find("\"type\":\"hello\"") != std::string::npos ||
                   message.find("\"type\": \"hello\"") != std::string::npos) {
          // Server → firmware config on WS open. Parse wake_open_delay_ms and store it so
          // the (optional) server-driven chime->start delay lambda can read it back.
          size_t key = message.find("wake_open_delay_ms");
          if (key != std::string::npos) {
            size_t colon = message.find(':', key);
            if (colon != std::string::npos) {
              // Skip whitespace after the colon, then parse the integer.
              size_t p = colon + 1;
              while (p < message.size() && (message[p] == ' ' || message[p] == '\t')) p++;
              long val = strtol(message.c_str() + p, nullptr, 10);
              if (val > 0) {
                this->wake_open_delay_ms_ = static_cast<uint32_t>(val);
                ESP_LOGI(TAG, "hello: wake_open_delay_ms = %u", this->wake_open_delay_ms_);
              }
            }
          }
        } else if (message.find("\"type\":\"enroll\"") != std::string::npos ||
                   message.find("\"type\": \"enroll\"") != std::string::npos) {
          // Enrollment mode enter/exit driven by the session-server EnrollmentConductor.
          if (message.find("\"mode\":\"start\"") != std::string::npos ||
              message.find("\"mode\": \"start\"") != std::string::npos) {
            this->enter_enrollment_();
          } else if (message.find("\"mode\":\"stop\"") != std::string::npos ||
                     message.find("\"mode\": \"stop\"") != std::string::npos) {
            this->exit_enrollment_();
          } else {
            ESP_LOGW(TAG, "enroll frame with no recognized mode: %.*s",
                     event_data->data_len, event_data->data_ptr);
          }
        }
      }
      break;
      
    case WEBSOCKET_EVENT_ERROR:
      if (event_data != nullptr) {
        // Log error information - note: error_handle may not be fully populated for all error types
        int sock_errno = event_data->error_handle.esp_transport_sock_errno;
        esp_err_t tls_err = event_data->error_handle.esp_tls_last_esp_err;
        
        ESP_LOGE(TAG, "WebSocket error - Type: %d, ESP-TLS Error: %s (0x%x), Socket errno: %d, Handshake Status: %d",
                 event_data->error_handle.error_type,
                 esp_err_to_name(tls_err),
                 tls_err,
                 sock_errno,
                 event_data->error_handle.esp_ws_handshake_status_code);
        
        // Log specific error types
        if (event_data->error_handle.error_type != WEBSOCKET_ERROR_TYPE_NONE) {
          switch (event_data->error_handle.error_type) {
            case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT:
              ESP_LOGE(TAG, "TCP transport error - check network connectivity and server address");
              if (sock_errno == 119) {
                ESP_LOGE(TAG, "Connection refused (errno 119) - check: 1) Server IP/port correct, 2) Same network subnet, 3) Firewall rules");
              } else if (sock_errno != 0) {
                ESP_LOGE(TAG, "Socket error (errno %d) - network connectivity issue", sock_errno);
              }
              break;
            case WEBSOCKET_ERROR_TYPE_HANDSHAKE:
              ESP_LOGE(TAG, "WebSocket handshake failed - Status code: %d", 
                       event_data->error_handle.esp_ws_handshake_status_code);
              break;
            case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:
              ESP_LOGE(TAG, "Pong timeout - server not responding to ping");
              break;
            case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE:
              ESP_LOGE(TAG, "Server closed connection");
              break;
            default:
              ESP_LOGE(TAG, "Unknown WebSocket error type: %d", event_data->error_handle.error_type);
              break;
          }
        } else {
          // Error type is NONE, but we still have error codes from ESP-IDF logs
          if (sock_errno == 119) {
            ESP_LOGE(TAG, "Connection refused (errno 119) - check: 1) Server IP/port correct, 2) Same network subnet, 3) Firewall rules");
          } else if (tls_err != ESP_OK) {
            ESP_LOGE(TAG, "Transport error: %s (0x%x)", 
                     esp_err_to_name(tls_err),
                     tls_err);
          } else if (sock_errno != 0) {
            ESP_LOGE(TAG, "Socket error (errno %d) - check network connectivity", sock_errno);
          }
        }
      } else {
        ESP_LOGE(TAG, "WebSocket error (no event data available)");
      }
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;

      // Never leave the box stuck in enrollment if the session errors mid-coach.
      this->exit_enrollment_();

      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }

      // Trigger error automation
      this->error_trigger_.trigger();

      // Connect/transport error (e.g. server down when you said "Neo") → clean up + IDLE
      // so the device is immediately ready to try again on the next wake, not stuck.
      this->reconnect_pending_ = false;
      this->pending_disconnect_ = true;
      break;
      
    default:
      break;
  }
}

}  // namespace voice_assistant_websocket
}  // namespace esphome

