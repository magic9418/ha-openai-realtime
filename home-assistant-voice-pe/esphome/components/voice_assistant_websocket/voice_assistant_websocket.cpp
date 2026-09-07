#include "voice_assistant_websocket.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"  // App.safe_reboot() for server-requested reboot
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
  // Created before preroll_init_/uplink_init_ so their first reset already runs serialized.
  if (this->deferred_lock_ == nullptr) this->deferred_lock_ = xSemaphoreCreateMutex();
  if (this->ws_client_lock_ == nullptr) this->ws_client_lock_ = xSemaphoreCreateMutex();
  this->preroll_init_();     // pre-allocate the PSRAM mic look-back (front-of-command clip fix)
  this->uplink_init_();      // PSRAM uplink ring + the one sender task (see the header INVARIANT)
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
  
  // Register microphone data callback
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data);
    });
  }
}

void VoiceAssistantWebSocket::loop() {
  // Fire any YAML automation queued by the websocket task. MUST be first, and MUST be on this
  // (main) task: the triggers run mixer_speaker.apply_ducking, the LED ring's perform() and
  // media_player.stop, none of which are safe from a websocket event handler. See DeferredEvent.
  this->dispatch_deferred_();

  // Handle pending reboot (must be done in main task, not websocket task). Set by the WS
  // text-frame handler on a server {"type":"reboot"} frame. App.safe_reboot() flushes
  // preferences and shuts components down cleanly, then resets the chip; it does not return.
  if (this->pending_reboot_) {
    this->pending_reboot_ = false;
    ESP_LOGW(TAG, "Server-requested reboot - rebooting now");
    App.safe_reboot();
    return;  // safe_reboot() does not return; guard anyway.
  }

  // Handle pending disconnect (must be done in main task, not websocket task)
  if (this->pending_disconnect_) {
    this->pending_disconnect_ = false;
    this->disconnect_websocket_();
    // After disconnect, continue with stop() cleanup
    // Clear buffers
    this->input_buffer_.clear();
    this->output_buffer_.clear();
    
    // INVARIANT: a reconnect queued for a FRESH WAKE must survive this teardown.
    // connect_websocket_() sets pending_disconnect_ + reconnect_pending_ + wake_reconnect_queued_
    // together when "Neo" arrives while the previous session's client still exists. This branch
    // used to clear reconnect_pending_ and force IDLE right here, so the reconnect below could
    // never run: the wake was silently dropped and the LED ring went idle - the recorded "wedged
    // next wake". Testing reconnect_pending_ alone is not enough, because
    // WEBSOCKET_EVENT_DISCONNECTED clears it during the very teardown the wake is queued behind;
    // that is what wake_reconnect_queued_ is for.
    if (this->reconnect_pending_ || this->wake_reconnect_queued_) {
      this->wake_reconnect_queued_ = false;
      this->reconnect_pending_ = true;
      // STARTING, not IDLE: the session is still coming up, just by way of the queued reconnect.
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_STARTING;
      // The client is destroyed now, so let the reconnect fire on the NEXT loop() instead of
      // waiting out RECONNECT_DELAY_MS. connect_websocket_() stamps last_reconnect_attempt_ to
      // "now", which DELAYED by 5 s the retry it was meant to expedite. uint32 modular arithmetic,
      // so this is still correct across a millis() wrap.
      this->last_reconnect_attempt_ = millis() - RECONNECT_DELAY_MS - 1;
      // reconnect_attempts_ is deliberately NOT reset here: MAX_RECONNECT_ATTEMPTS stays a real
      // bound on a genuinely failing connect (a successful CONNECTED clears it).
      this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
      // No stopped_trigger_: nothing stopped. Firing it would un-duck the music and reset the LED
      // ring in the middle of a wake we are about to honour.
      ESP_LOGI(TAG, "Disconnect complete; queued wake reconnect runs on the next loop");
      return;
    }

    this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
    this->reconnect_attempts_ = 0;
    this->reconnect_pending_ = false;

    this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);

    // Trigger stopped automation (deferred, but this is already the main task - queued only so it
    // cannot jump ahead of an EV_CONNECTED/EV_DISCONNECTED still sitting in the FIFO).
    this->defer_event_(DeferredEvent::EV_STOPPED);

    ESP_LOGI(TAG, "Voice Assistant WebSocket stopped");
    return;  // Skip other loop operations after disconnect
  }
  
  // Drain the assistant-audio backlog (PSRAM ring) into the speaker as it frees up.
  // INVARIANT: bytes only ever go into a RUNNING speaker. A barge-in stop() leaves the resampler
  // in STATE_STOPPING for a beat, and ResamplerSpeaker::play() happily ACCEPTS bytes in that state
  // (it only auto-starts when fully STOPPED) - its task then discards them as it shuts down. That
  // is what clipped the first 100-250 ms of every barge-in follow-up reply.
  if (this->speaker_ != nullptr && this->audio_ring_fill_ > 0) {
    if (this->speaker_->is_running()) {
      this->audio_ring_drain_();
    } else if (this->speaker_->is_stopped() && this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
      // The stop has completed. Restarting is done HERE (main task) rather than in
      // process_received_audio_ (websocket task), so that buffered reply audio still plays even if
      // the server has already sent its last chunk. Re-assert the 24 kHz input rate or the
      // resampler treats its own 48 kHz output rate as the input and the reply plays at 2x speed,
      // high-pitched.
      audio::AudioStreamInfo input_stream_info(16, 1, 24000);  // 16-bit, mono, 24kHz (OpenAI output)
      this->speaker_->set_audio_stream_info(input_stream_info);
      this->speaker_->start();
    }
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

  // Safety valve for the branch above: a queued reconnect that can NEVER run (attempt cap reached)
  // must not leave the device parked in STARTING with the LED ring lit forever. Fall back to IDLE
  // so the next "Neo" starts from a clean slate.
  if (this->reconnect_pending_ && !this->pending_disconnect_ && this->websocket_client_ == nullptr &&
      this->reconnect_attempts_ >= MAX_RECONNECT_ATTEMPTS) {
    ESP_LOGW(TAG, "Reconnect attempt cap (%u) reached - returning to IDLE", MAX_RECONNECT_ATTEMPTS);
    this->reconnect_pending_ = false;
    this->reconnect_attempts_ = 0;
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
    this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
    this->defer_event_(DeferredEvent::EV_STOPPED);
  }
  
  // Auto-stop: Check if we should stop after inactivity
  // Stop if: speaker hasn't spoken for 5 seconds
  // Note: We only check speaker audio, not microphone audio, because:
  // - Microphone always sends audio (background noise, silence, etc.)
  // - OpenAI's server_vad handles voice activity detection
  // - If user speaks, OpenAI will generate new audio, which resets the timer
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    uint32_t current_time = millis();
    // Inactivity reference: last speaker audio once the bot has spoken, else the moment the session
    // went RUNNING. The old code only armed the timer after the FIRST audio chunk, so a bare wake
    // with no reply (you say "Neo" then nothing, or the model returns empty) left the session open
    // forever — which then wedges the next wake ("WebSocket client already exists"). Using
    // running_since_ as the fallback makes a silent session auto-close after the same timeout.
    uint32_t inactivity_ref =
        (this->last_speaker_audio_time_ > 0) ? this->last_speaker_audio_time_ : this->running_since_;

    if (inactivity_ref > 0 && (current_time - inactivity_ref) > this->auto_stop_inactivity_ms_) {
      ESP_LOGI(TAG, "Auto-stopping: inactive for %u ms (threshold: %u ms, bot_spoke=%s)",
               current_time - inactivity_ref, this->auto_stop_inactivity_ms_,
               this->last_speaker_audio_time_ > 0 ? "yes" : "no");
      // Timer/auto-stop close: drop any half-sentence still sitting in the server's
      // input buffer *at the cut-off source* so a later wake can't "complete" it.
      // Only on this timer path — NOT on user-interrupt or wake (per the guards plan;
      // a reactive clear-on-wake disturbs the server VAD). Sent while still connected,
      // before stop() tears the WS down.
      // BOUNDED, not portMAX_DELAY: this send is on the MAIN LOOP, and a stalled TCP write here
      // parks every other ESPHome component behind it (LED ring, mixer, watchdog). Losing the
      // frame only means the server keeps a half-sentence it would otherwise have dropped;
      // stalling the loop risks a watchdog reset. send_text_frame_ logs the failure.
      this->send_text_frame_("{\"type\":\"mic_flush\"}", pdMS_TO_TICKS(50));
      this->stop();
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
  // New session: no reply audio yet. Latched separately from the timestamp above precisely because
  // interrupt() zeroes that one mid-session (see turn_has_no_reply_audio()).
  this->reply_audio_seen_this_session_ = false;

  // Do NOT reset the mic look-back here. start() is the wake instant, and the audio we are trying to
  // rescue was spoken BEFORE it — inside micro_wake_word's detection latency. The ring has been
  // filling all along while idle and already holds it; clearing here would throw away the fix.

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
  
  this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
  
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
  // Stop speaker + empty the backlog ring atomically (serialized against the main-task drain).
  this->audio_ring_flush_and_stop_speaker_();
  // Drop the look-back: it belongs to a session that is ending (and its tail is Neo's own reply
  // bleeding into the mic). Leaving it would prepend that onto the NEXT wake's uplink. Idle frames
  // refill the ring within PREROLL_MS, well before anyone can say "Neo" again.
  this->preroll_reset_();

  this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
  
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
    // ...plus a flag the websocket event handlers do NOT clear. Both of them null reconnect_pending_
    // on any drop/error, and the teardown we are queueing behind fires exactly that event - which
    // is how this wake used to be silently dropped (see loop()'s pending_disconnect_ branch).
    this->wake_reconnect_queued_ = true;
    this->last_reconnect_attempt_ = millis();  // Reset timer so we retry after disconnect
    return;  // Exit early, will retry connection after disconnect completes in loop()
  }
  
  if (this->server_url_.empty()) {
    ESP_LOGE(TAG, "Server URL not set!");
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
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
    this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
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
    this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
  }
}

void VoiceAssistantWebSocket::disconnect_websocket_() {
  if (this->websocket_client_ == nullptr) {
    return;
  }
  ESP_LOGI(TAG, "Disconnecting WebSocket...");

  // INVARIANT: the uplink sender task must not be inside esp_websocket_client_send_bin() when the
  // handle is destroyed. Three things enforce that, in this order:
  //   1. uplink_paused_ tells the sender to stop reaching for the client at all;
  //   2. websocket_client_ is published as nullptr BEFORE the destroy, and the sender re-reads it
  //      under ws_client_lock_ immediately before every send;
  //   3. ws_client_lock_ is held across the teardown.
  // The lock take is BOUNDED: esp_websocket_client_send_bin() can sit on a stalled TCP write, and
  // this runs on the main task, which must never be parked long enough to trip the watchdog. If we
  // cannot get the lock we proceed anyway - which is exactly what the previous, lockless code did
  // unconditionally, so this is never worse than before.
  this->uplink_paused_ = true;
  esp_websocket_client_handle_t client = this->websocket_client_;
  this->websocket_client_ = nullptr;

  bool locked = false;
  if (this->ws_client_lock_ != nullptr) {
    locked = xSemaphoreTake(this->ws_client_lock_, pdMS_TO_TICKS(500)) == pdTRUE;
    if (!locked) {
      ESP_LOGW(TAG, "Uplink sender still busy after 500 ms - destroying the client anyway");
    }
  }

  // Check if client is actually connected before trying graceful close
  bool was_connected = esp_websocket_client_is_connected(client);

  if (was_connected) {
    // Try graceful close first (sends close frame)
    // Use shorter timeout (1 second) to avoid blocking too long
    esp_err_t close_err = esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    if (close_err != ESP_OK) {
      ESP_LOGW(TAG, "Graceful close failed (%s), forcing stop", esp_err_to_name(close_err));
      // Fallback to immediate stop if graceful close fails
      esp_websocket_client_stop(client);
    }
  } else {
    // Client not connected, just stop and destroy immediately
    ESP_LOGD(TAG, "Client not connected, stopping immediately");
    esp_websocket_client_stop(client);
  }

  // Always destroy the client to free resources
  esp_websocket_client_destroy(client);

  if (locked) {
    xSemaphoreGive(this->ws_client_lock_);
  }
  this->uplink_paused_ = false;
  // The session is over: anything still queued belongs to it and must not leak into the next wake.
  this->uplink_reset_();
}

void VoiceAssistantWebSocket::send_audio_chunk_(const uint8_t *data, size_t len) {
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    return;
  }
  
  // Hand the frame to the uplink ring; the dedicated sender task does the actual network write
  // with a bounded timeout.
  // INVARIANT: every caller of this function runs on the i2s MIC TASK (on_microphone_data_ and
  // preroll_flush_), so it must never block on the network. It used to call
  // esp_websocket_client_send_bin(..., portMAX_DELAY) directly, taking the websocket client lock -
  // the same lock held during event dispatch and for up to 1 s inside close(). That starved
  // micro_wake_word's ~120 ms producer ring on every connect and every session end, and mww resets
  // the ring when it overflows: the device was literally deaf at exactly those moments.
  this->uplink_enqueue_(data, len);
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
  
  // Speaker (re)start is NOT done here any more - see loop(). This runs on the websocket task,
  // and is_stopped() is false while the resampler is still STOPPING after a barge-in, so this
  // could not restart it anyway; the drain that followed then wrote into the stopping resampler.
  // Kept as a debug breadcrumb only.
  if (this->speaker_->is_stopped()) {
    ESP_LOGD(TAG, "Speaker is stopped; loop() will restart it before draining");
    // Re-assert the 24kHz input stream info before restarting. start() sets it at session begin,
    // but a barge-in interrupt STOPS the speaker; restarting here without re-declaring the rate makes
    // the resampler assume its 48kHz output rate as input (no 24->48 upsample), so the follow-up reply
    // plays 24kHz PCM at 48kHz — 2x speed, high-pitched. (Matches the Sat1 fix.)
    audio::AudioStreamInfo input_stream_info(16, 1, 24000);  // 16-bit, mono, 24kHz (OpenAI output)
    this->speaker_->set_audio_stream_info(input_stream_info);
    this->speaker_->start();
  }
  
  // FIFO through the PSRAM ring: append the new audio, then drain as much as the speaker will
  // accept. Strict order (never plays new bytes ahead of buffered backlog) with ZERO per-chunk
  // heap allocation — this is the fix for the long-playback (read-a-book) OOM crash.
  this->last_speaker_audio_time_ = millis();
  // Latched for the whole session (see turn_has_no_reply_audio()). NOT derived from the timestamp
  // above, which interrupt() zeroes on every barge-in.
  this->reply_audio_seen_this_session_ = true;
  this->audio_ring_push_(data, len);
  // NO direct drain here. This runs on the WEBSOCKET task, and the speaker may still be STOPPING
  // from a barge-in; ResamplerSpeaker::play() accepts those bytes and its task then throws them
  // away, clipping the front of the follow-up reply. loop() drains on the main task, gated on
  // speaker_->is_running(), and restarts the speaker there if it has finished stopping.
}

// ---- PSRAM audio backlog ring buffer (see header) ----------------------------------------
// ---- Mic look-back: the PREROLL_MS of room audio preceding the wake -------------------------
// Circular, and filled continuously while idle. A buffer armed by start() cannot fix the clip;
// see the header for why (the lost words are spoken before the wake fires).
void VoiceAssistantWebSocket::preroll_init_() {
  // Must exist before the preroll_reset_() at the end of this function.
  if (this->preroll_lock_ == nullptr) {
    this->preroll_lock_ = xSemaphoreCreateMutex();
  }
  if (this->preroll_ != nullptr) return;
#ifdef USE_ESP_IDF
  this->preroll_ = static_cast<uint8_t *>(heap_caps_malloc(PREROLL_CAPACITY, MALLOC_CAP_SPIRAM));
  if (this->preroll_ == nullptr) {
    ESP_LOGE(TAG, "PSRAM look-back alloc failed (%zu bytes); trying internal", PREROLL_CAPACITY);
    this->preroll_ = static_cast<uint8_t *>(heap_caps_malloc(PREROLL_CAPACITY, MALLOC_CAP_8BIT));
  }
#else
  this->preroll_ = static_cast<uint8_t *>(malloc(PREROLL_CAPACITY));
#endif
  if (this->preroll_ == nullptr) {
    ESP_LOGE(TAG, "Look-back allocation failed — the front of commands will be clipped again");
  }
  this->preroll_reset_();
}

// Circular write. Overwriting the oldest byte is the POINT — we always want the most recent
// PREROLL_MS, so a full ring is the steady state, not an overflow condition.
void VoiceAssistantWebSocket::preroll_push_(const uint8_t *data, size_t len) {
  if (this->preroll_ == nullptr || len == 0) return;
  // Tiny critical section - a memcpy and two cursor updates - so the mic task is never delayed.
  // portMAX_DELAY is safe here precisely BECAUSE every other holder of this lock is equally tiny
  // and none of them ever touches the network (see the header INVARIANT).
  if (this->preroll_lock_ != nullptr) xSemaphoreTake(this->preroll_lock_, portMAX_DELAY);
  if (len >= PREROLL_CAPACITY) {              // one chunk bigger than the ring: keep only its tail
    memcpy(this->preroll_, data + (len - PREROLL_CAPACITY), PREROLL_CAPACITY);
    this->preroll_head_ = 0;
    this->preroll_fill_ = PREROLL_CAPACITY;
    if (this->preroll_lock_ != nullptr) xSemaphoreGive(this->preroll_lock_);
    return;
  }
  size_t first = PREROLL_CAPACITY - this->preroll_head_;   // room before the wrap
  if (first > len) first = len;
  memcpy(this->preroll_ + this->preroll_head_, data, first);
  if (len > first) {
    memcpy(this->preroll_, data + first, len - first);     // remainder at the bottom
  }
  this->preroll_head_ = (this->preroll_head_ + len) % PREROLL_CAPACITY;
  this->preroll_fill_ =
      (this->preroll_fill_ + len > PREROLL_CAPACITY) ? PREROLL_CAPACITY : this->preroll_fill_ + len;
  if (this->preroll_lock_ != nullptr) xSemaphoreGive(this->preroll_lock_);
}

void VoiceAssistantWebSocket::preroll_flush_() {
  if (this->preroll_ == nullptr) return;
  // Snapshot the window UNDER THE LOCK, then release it before a single byte is sent. This runs on
  // the mic task, which is also the ring's only producer, so the snapshotted bytes cannot be
  // overwritten while we read them; a concurrent preroll_reset_() only zeroes the cursors.
  // INVARIANT (finding 4): the lock is NOT held across the sends below.
  if (this->preroll_lock_ != nullptr) xSemaphoreTake(this->preroll_lock_, portMAX_DELAY);
  size_t total = this->preroll_fill_;
  // Oldest byte sits `total` behind the write cursor, modulo the ring.
  size_t start = (this->preroll_head_ + PREROLL_CAPACITY - total) % PREROLL_CAPACITY;
  // Clear FIRST: re-entering here with a non-zero fill would double-send the look-back.
  this->preroll_fill_ = 0;
  this->preroll_head_ = 0;
  if (this->preroll_lock_ != nullptr) xSemaphoreGive(this->preroll_lock_);
  if (total == 0) return;
  uint32_t ms = (uint32_t) (total / BYTES_PER_MS_24K);
  ESP_LOGI(TAG, "Flushing %zu B look-back (%u ms of pre-wake mic audio)", total, (unsigned) ms);
  // Tell the server how much PRE-wake audio it is about to receive. Guard B asks "did the user speak
  // AFTER the wake?"; without this it would see speech begin the instant the socket opens — that is
  // the wake word itself, now sitting at the front of this flush — and either cancel a real command
  // or wave through a bare false wake. The server subtracts this to locate the wake instant in the
  // audio timeline. Sent BEFORE the audio so it can never arrive late.
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"type\":\"preroll\",\"ms\":%u}", (unsigned) ms);
  // BOUNDED: still the mic task. Unbounded here would stall micro_wake_word's producer exactly at
  // the moment of a wake. The audio itself now goes through the non-blocking uplink ring below.
  this->send_text_frame_(buf, pdMS_TO_TICKS(100));
  for (size_t sent = 0; sent < total; ) {
    size_t idx = (start + sent) % PREROLL_CAPACITY;
    size_t n = (total - sent) < PREROLL_CHUNK ? (total - sent) : PREROLL_CHUNK;
    if (idx + n > PREROLL_CAPACITY) {
      n = PREROLL_CAPACITY - idx;             // never span the wrap in a single send
    }
    this->send_audio_chunk_(this->preroll_ + idx, n);
    sent += n;
  }
}

void VoiceAssistantWebSocket::preroll_reset_() {
  // Called from the MIC task (contiguity break), the MAIN task (init) and the WEBSOCKET task
  // (stop()). Serialized so a reset can never land between preroll_push_'s head and fill updates
  // and leave a stale fill over a rewound head - which would let a later flush send bytes that
  // were never contiguous, breaking the ring's one invariant.
  if (this->preroll_lock_ != nullptr) xSemaphoreTake(this->preroll_lock_, portMAX_DELAY);
  this->preroll_fill_ = 0;
  this->preroll_head_ = 0;
  if (this->preroll_lock_ != nullptr) xSemaphoreGive(this->preroll_lock_);
}

// ---- Deferred YAML-automation dispatch (finding 2; see DeferredEvent in the header) --------
// PRODUCER. Runs on whichever task observed the event - usually the websocket task. Records a few
// words and returns; it never runs YAML and never takes any other lock.
void VoiceAssistantWebSocket::defer_event_(DeferredEvent event, VoiceAssistantWebSocketState state) {
  if (this->deferred_lock_ == nullptr) {
    return;  // before setup(): nothing is wired up yet, so there is nothing to fire
  }
  // Bounded take: the consumer only ever holds this for a pop, but the websocket task must not be
  // able to wedge behind the main task under any circumstance.
  if (xSemaphoreTake(this->deferred_lock_, pdMS_TO_TICKS(50)) != pdTRUE) {
    this->deferred_overflow_++;
    return;
  }
  if (this->deferred_count_ < DEFERRED_QUEUE_LEN) {
    this->deferred_[this->deferred_head_] = DeferredEntry{event, state};
    this->deferred_head_ = (this->deferred_head_ + 1) % DEFERRED_QUEUE_LEN;
    this->deferred_count_++;
  } else {
    // Drop the NEWEST so the queue's ORDER stays intact. 16 slots is far more than a session can
    // generate between two loop() iterations; a drop here means loop() itself is wedged, which is
    // a much bigger problem than the lost trigger.
    this->deferred_overflow_++;
  }
  xSemaphoreGive(this->deferred_lock_);
}

// CONSUMER. MAIN TASK ONLY. Pops under the lock, then fires with the lock RELEASED: a trigger runs
// arbitrary YAML (ducking, LED ring, media_player) and must never do so inside a mutex the
// websocket task is waiting on.
void VoiceAssistantWebSocket::dispatch_deferred_() {
  if (this->deferred_lock_ == nullptr) return;
  // Bounded: a trigger may itself defer another event, so draining at most one queue's worth per
  // loop() guarantees this returns.
  for (size_t i = 0; i < DEFERRED_QUEUE_LEN; i++) {
    DeferredEntry entry{};
    bool have = false;
    uint32_t overflow = 0;
    if (xSemaphoreTake(this->deferred_lock_, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (this->deferred_count_ > 0) {
      entry = this->deferred_[this->deferred_tail_];
      this->deferred_tail_ = (this->deferred_tail_ + 1) % DEFERRED_QUEUE_LEN;
      this->deferred_count_--;
      have = true;
    }
    overflow = this->deferred_overflow_;
    this->deferred_overflow_ = 0;
    xSemaphoreGive(this->deferred_lock_);

    if (overflow > 0) {
      ESP_LOGW(TAG, "Deferred automation queue overflow - dropped %u event(s)", (unsigned) overflow);
    }
    if (!have) return;

    switch (entry.event) {
      case DeferredEvent::EV_STATE_CALLBACK:
        if (this->state_callback_) this->state_callback_(entry.state);
        break;
      case DeferredEvent::EV_CONNECTED:
        this->connected_trigger_.trigger();
        break;
      case DeferredEvent::EV_DISCONNECTED:
        this->disconnected_trigger_.trigger();
        break;
      case DeferredEvent::EV_ERROR:
        this->error_trigger_.trigger();
        break;
      case DeferredEvent::EV_ENROLL_START:
        this->enroll_start_trigger_.trigger();
        break;
      case DeferredEvent::EV_ENROLL_STOP:
        this->enroll_stop_trigger_.trigger();
        break;
      case DeferredEvent::EV_STOPPED:
        this->stopped_trigger_.trigger();
        break;
    }
  }
}

// ---- Uplink ring + sender task (finding 3; see the INVARIANT in the header) ----------------
void VoiceAssistantWebSocket::uplink_init_() {
  if (this->uplink_lock_ == nullptr) this->uplink_lock_ = xSemaphoreCreateMutex();
  if (this->uplink_wake_ == nullptr) this->uplink_wake_ = xSemaphoreCreateBinary();
#ifdef USE_ESP_IDF
  if (this->uplink_ring_ == nullptr) {
    this->uplink_ring_ = static_cast<uint8_t *>(heap_caps_malloc(UPLINK_RING_CAPACITY, MALLOC_CAP_SPIRAM));
    if (this->uplink_ring_ == nullptr) {
      ESP_LOGE(TAG, "PSRAM uplink ring alloc failed (%zu bytes); trying internal", UPLINK_RING_CAPACITY);
      this->uplink_ring_ = static_cast<uint8_t *>(heap_caps_malloc(UPLINK_RING_CAPACITY, MALLOC_CAP_8BIT));
    }
  }
  // The staging copy is handed straight to lwip, so keep that one in internal RAM.
  if (this->uplink_stage_ == nullptr) {
    this->uplink_stage_ =
        static_cast<uint8_t *>(heap_caps_malloc(UPLINK_SEND_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
#else
  if (this->uplink_ring_ == nullptr) this->uplink_ring_ = static_cast<uint8_t *>(malloc(UPLINK_RING_CAPACITY));
  if (this->uplink_stage_ == nullptr) this->uplink_stage_ = static_cast<uint8_t *>(malloc(UPLINK_SEND_CHUNK));
#endif
  this->uplink_read_ = 0;
  this->uplink_fill_ = 0;
  if (this->uplink_ring_ == nullptr || this->uplink_stage_ == nullptr || this->uplink_lock_ == nullptr ||
      this->uplink_wake_ == nullptr) {
    // uplink_enqueue_ falls back to the old direct send in this case - degraded, but not silent.
    ESP_LOGE(TAG, "Uplink ring unavailable - falling back to blocking sends on the mic task");
    return;
  }
  // Create the sender task exactly ONCE, for the life of the component.
  if (this->uplink_task_ == nullptr) {
    this->uplink_task_exit_ = false;
    // Priority 5 == websocket_cfg.task_prio, so the sender never preempts the websocket client's
    // own receive/dispatch task; both sit far below the i2s mic task (17), whose latency is the
    // entire point of this change, and well above the ESPHome main loop (1) so audio keeps moving
    // while loop() works.
    if (xTaskCreate(&VoiceAssistantWebSocket::uplink_task_fn_, "va_ws_uplink", 4096, this, 5,
                    &this->uplink_task_) != pdPASS) {
      this->uplink_task_ = nullptr;
      ESP_LOGE(TAG, "Failed to create uplink sender task - falling back to blocking sends");
    }
  }
}

// MIC TASK. Never blocks: a memcpy under a mutex held for microseconds, drop-OLDEST on overflow
// (the newest audio is the audio the server still needs).
void VoiceAssistantWebSocket::uplink_enqueue_(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0) return;
  if (this->uplink_ring_ == nullptr || this->uplink_lock_ == nullptr || this->uplink_task_ == nullptr) {
    // Fail-safe: allocation or task creation failed at boot. Fall back to a direct (but still
    // BOUNDED) send so the device keeps working, accepting the mic-task stall that costs.
    if (this->websocket_client_ != nullptr) {
      esp_websocket_client_send_bin(this->websocket_client_, (const char *) data, len,
                                    pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS));
    }
    return;
  }
  if (len > UPLINK_RING_CAPACITY) {   // cannot happen with ~100 ms chunks; keep the tail if it does
    data += (len - UPLINK_RING_CAPACITY);
    len = UPLINK_RING_CAPACITY;
  }
  // Bounded take, never portMAX_DELAY: the whole point is that this call site cannot stall.
  if (xSemaphoreTake(this->uplink_lock_, pdMS_TO_TICKS(10)) != pdTRUE) {
    this->uplink_dropped_bytes_ += len;
    this->uplink_drop_events_++;
    return;
  }
  size_t avail = UPLINK_RING_CAPACITY - this->uplink_fill_;
  if (len > avail) {
    // DROP-OLDEST: advance the read cursor past exactly as much as we need to fit the new frame.
    size_t discard = len - avail;
    this->uplink_read_ = (this->uplink_read_ + discard) % UPLINK_RING_CAPACITY;
    this->uplink_fill_ -= discard;
    this->uplink_dropped_bytes_ += discard;
    this->uplink_drop_events_++;
  }
  size_t write = (this->uplink_read_ + this->uplink_fill_) % UPLINK_RING_CAPACITY;
  size_t first = std::min(len, UPLINK_RING_CAPACITY - write);   // bytes until the wrap
  memcpy(this->uplink_ring_ + write, data, first);
  if (len > first) memcpy(this->uplink_ring_, data + first, len - first);
  this->uplink_fill_ += len;
  xSemaphoreGive(this->uplink_lock_);
  xSemaphoreGive(this->uplink_wake_);   // binary semaphore; a give while already given is a no-op
}

// Drop whatever is still queued for a session that is over, so a dead turn's audio can never be
// prepended to the next one.
void VoiceAssistantWebSocket::uplink_reset_() {
  if (this->uplink_lock_ == nullptr) return;
  if (xSemaphoreTake(this->uplink_lock_, pdMS_TO_TICKS(50)) != pdTRUE) return;
  this->uplink_read_ = 0;
  this->uplink_fill_ = 0;
  xSemaphoreGive(this->uplink_lock_);
}

void VoiceAssistantWebSocket::uplink_task_fn_(void *param) {
  static_cast<VoiceAssistantWebSocket *>(param)->uplink_run_();
  vTaskDelete(nullptr);
}

void VoiceAssistantWebSocket::uplink_run_() {
  this->uplink_task_running_ = true;
  while (!this->uplink_task_exit_) {
    // Wake on data; the timeout also gives us a tick for the exit check and the drop report.
    xSemaphoreTake(this->uplink_wake_, pdMS_TO_TICKS(100));

    while (!this->uplink_task_exit_) {
      size_t n = 0;
      const size_t chunk = UPLINK_SEND_CHUNK;   // local: keep the static const out of std::min's refs
      if (xSemaphoreTake(this->uplink_lock_, pdMS_TO_TICKS(50)) != pdTRUE) break;
      if (this->uplink_fill_ > 0) {
        n = std::min(this->uplink_fill_, chunk);
        n = std::min(n, UPLINK_RING_CAPACITY - this->uplink_read_);   // never span the wrap
        // Copy out UNDER the lock so a concurrent drop-oldest can never overwrite bytes that are
        // already in flight - and so the send below holds no lock at all (finding 4's rule).
        memcpy(this->uplink_stage_, this->uplink_ring_ + this->uplink_read_, n);
        this->uplink_read_ = (this->uplink_read_ + n) % UPLINK_RING_CAPACITY;
        this->uplink_fill_ -= n;
      }
      xSemaphoreGive(this->uplink_lock_);
      if (n == 0) break;

      if (this->uplink_paused_) continue;   // client teardown in progress: discard, do not touch it
      // Bounded take. disconnect_websocket_() publishes websocket_client_ = nullptr before it
      // destroys the handle and holds this lock across the destroy, so re-reading the handle here
      // under the lock is what makes this send safe.
      if (this->ws_client_lock_ == nullptr ||
          xSemaphoreTake(this->ws_client_lock_, pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS)) != pdTRUE) {
        this->uplink_dropped_bytes_ += n;
        this->uplink_drop_events_++;
        continue;
      }
      if (this->websocket_client_ != nullptr && esp_websocket_client_is_connected(this->websocket_client_)) {
        int sent = esp_websocket_client_send_bin(this->websocket_client_, (const char *) this->uplink_stage_, n,
                                                 pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS));
        if (sent < 0) {
          this->uplink_dropped_bytes_ += n;
          this->uplink_drop_events_++;
        }
      }
      xSemaphoreGive(this->ws_client_lock_);
    }

    // Rate-limited, greppable drop report.
    if (this->uplink_drop_events_ > 0 && (millis() - this->uplink_last_drop_log_) > UPLINK_DROP_LOG_INTERVAL_MS) {
      ESP_LOGW(TAG, "[uplink] dropped %u B in %u event(s); ring %zu/%zu", (unsigned) this->uplink_dropped_bytes_,
               (unsigned) this->uplink_drop_events_, this->uplink_fill_, UPLINK_RING_CAPACITY);
      this->uplink_dropped_bytes_ = 0;
      this->uplink_drop_events_ = 0;
      this->uplink_last_drop_log_ = millis();
    }
  }
  this->uplink_task_running_ = false;
}

// MAIN TASK. Ask the sender to exit and wait (bounded) for it to actually leave uplink_run_()
// before anything it touches can go away.
void VoiceAssistantWebSocket::uplink_shutdown_() {
  if (this->uplink_task_ == nullptr) return;
  this->uplink_task_exit_ = true;
  if (this->uplink_wake_ != nullptr) xSemaphoreGive(this->uplink_wake_);
  for (uint32_t i = 0; i < 100 && this->uplink_task_running_; i++) {
    vTaskDelay(pdMS_TO_TICKS(5));   // <= 500 ms
  }
  this->uplink_task_ = nullptr;
}

void VoiceAssistantWebSocket::on_shutdown() { this->uplink_shutdown_(); }

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
  if (this->audio_ring_lock_ == nullptr) {
    this->audio_ring_lock_ = xSemaphoreCreateMutex();   // serialize drain vs push/clear/stop
  }
}

void VoiceAssistantWebSocket::audio_ring_push_(const uint8_t *data, size_t len) {
  if (this->audio_ring_ == nullptr || data == nullptr || len == 0) return;
  if (this->audio_ring_lock_ != nullptr) xSemaphoreTake(this->audio_ring_lock_, portMAX_DELAY);
  size_t avail = AUDIO_RING_CAPACITY - this->audio_ring_fill_;
  if (len > avail) {
    // Backlog full. Server paces ~real-time, so this is rare jitter, not steady state — drop the
    // chunk rather than ever growing memory. (Bounded by construction: this is the OOM guard.)
    ESP_LOGW(TAG, "Audio ring full (%zu/%zu), dropping %zu bytes", this->audio_ring_fill_,
             AUDIO_RING_CAPACITY, len);
    if (this->audio_ring_lock_ != nullptr) xSemaphoreGive(this->audio_ring_lock_);
    return;
  }
  size_t write = (this->audio_ring_read_ + this->audio_ring_fill_) % AUDIO_RING_CAPACITY;
  size_t first = std::min(len, AUDIO_RING_CAPACITY - write);   // bytes until wrap
  memcpy(this->audio_ring_ + write, data, first);
  if (len > first) memcpy(this->audio_ring_, data + first, len - first);
  this->audio_ring_fill_ += len;
  if (this->audio_ring_lock_ != nullptr) xSemaphoreGive(this->audio_ring_lock_);
  this->report_audio_free_(false);   // fill rose → advertise reduced headroom to the server
}

void VoiceAssistantWebSocket::audio_ring_drain_() {
  if (this->audio_ring_ == nullptr || this->speaker_ == nullptr) return;
  // Hold the lock across the whole drain (including speaker_->play(), which is non-blocking): a
  // concurrent barge-in clear/stop must not zero fill between the play and the `fill -= written`
  // (that underflowed size_t → endless "loop audio") nor stop the speaker mid-play.
  if (this->audio_ring_lock_ != nullptr) xSemaphoreTake(this->audio_ring_lock_, portMAX_DELAY);
  while (this->audio_ring_fill_ > 0) {
    size_t contiguous = std::min(this->audio_ring_fill_, AUDIO_RING_CAPACITY - this->audio_ring_read_);
    size_t written = this->speaker_->play(this->audio_ring_ + this->audio_ring_read_, contiguous);
    if (written == 0) break;   // speaker buffer full — retry next loop()
    if (written > this->audio_ring_fill_) written = this->audio_ring_fill_;   // belt-and-braces: never underflow
    this->audio_ring_read_ = (this->audio_ring_read_ + written) % AUDIO_RING_CAPACITY;
    this->audio_ring_fill_ -= written;
  }
  if (this->audio_ring_lock_ != nullptr) xSemaphoreGive(this->audio_ring_lock_);
  this->report_audio_free_(false);   // fill fell → advertise freed headroom so the server tops up
}

void VoiceAssistantWebSocket::audio_ring_clear_() {
  if (this->audio_ring_lock_ != nullptr) xSemaphoreTake(this->audio_ring_lock_, portMAX_DELAY);
  this->audio_ring_read_ = 0;
  this->audio_ring_fill_ = 0;
  if (this->audio_ring_lock_ != nullptr) xSemaphoreGive(this->audio_ring_lock_);
  this->report_audio_free_(true);    // ring emptied (interrupt/flush) → re-prime the server now
}

// Barge-in / server interrupt: stop the speaker AND empty the ring as one atomic step, serialized
// against a concurrent drain. Doing the two separately (the old `speaker_->stop(); audio_ring_clear_()`)
// let the main-task drain push already-buffered stale bytes into the speaker AFTER the stop — the
// audio that "kept talking" / looped after a break-in. Callers run on the websocket task.
void VoiceAssistantWebSocket::audio_ring_flush_and_stop_speaker_() {
  if (this->audio_ring_lock_ != nullptr) xSemaphoreTake(this->audio_ring_lock_, portMAX_DELAY);
  if (this->speaker_ != nullptr) this->speaker_->stop();
  this->audio_ring_read_ = 0;
  this->audio_ring_fill_ = 0;
  if (this->audio_ring_lock_ != nullptr) xSemaphoreGive(this->audio_ring_lock_);
  this->report_audio_free_(true);
}

// Closed-loop flow control: tell the server how many bytes are free in the PSRAM ring so it sends
// only into real headroom. Absolute (capacity - fill), not a delta, so a dropped report is self-
// correcting. Hysteresis (QUANTUM) keeps this to a few frames/second, not one per audio chunk.
void VoiceAssistantWebSocket::report_audio_free_(bool force) {
  if (!this->flow_control_enabled_ || !this->is_connected()) return;
  size_t delta = (this->audio_ring_fill_ > this->audio_free_reported_fill_)
                     ? this->audio_ring_fill_ - this->audio_free_reported_fill_
                     : this->audio_free_reported_fill_ - this->audio_ring_fill_;
  if (!force && delta < AUDIO_FREE_REPORT_QUANTUM) return;
  this->audio_free_reported_fill_ = this->audio_ring_fill_;
  size_t free_bytes = AUDIO_RING_CAPACITY - this->audio_ring_fill_;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"type\":\"audio_free\",\"bytes\":%zu}", free_bytes);
  // BOUNDED: audio_ring_drain_() calls this from the MAIN LOOP. Safe to drop - the report is
  // absolute (capacity - fill), not a delta, so the next one self-corrects.
  this->send_text_frame_(buf, pdMS_TO_TICKS(50));
}

void VoiceAssistantWebSocket::on_microphone_data_(const std::vector<uint8_t> &data) {
  // Only stream if connected. Normally we stream only during a RUNNING session — BUT during
  // enrollment ("teach me my voice") there is no wake-session: the mic is pinned open with wake
  // disarmed, and the server needs every rep frame to build the training WAV. Without the
  // `enrolling_` exception the enrollment recorder captured ~nothing (empty WAVs).
  const bool live = this->is_connected() &&
                    (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING || this->enrolling_);
  // LOOK-BACK (front-of-command clip). When not live we no longer return early — we keep the most
  // recent PREROLL_MS in a ring so that a run-together "Neo play some alternative music" survives
  // micro_wake_word's detection latency. Those frames are already being delivered here (the callback
  // is registered unconditionally in setup() and mww keeps the mic running); we just used to drop
  // them. Enrollment is excluded: it has no wake, and its own live path already streams every frame.
  if (!live && this->enrolling_) {
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
    // Contiguity invariant (see header): a dropped frame breaks the ring's "recent, continuous room
    // audio" guarantee, so clear it. This is also what keeps Neo's OWN reply out of the look-back —
    // is_bot_speaking() stays true for a beat after stop(), which is exactly when the speaker is
    // still sounding into the mic. Without this, the next wake would prepend Neo talking to itself.
    this->preroll_reset_();
    return;
  }
#endif

  // Wake-chime gate: drop uplink for the wake-open window after a wake so the chime doesn't bleed
  // into ASR. The WS connect overlaps this window, so the mic opens ~wake_open_delay_ms after wake
  // (not chime + delay + connect). Enrollment pins the mic open regardless.
  // (Only while live — by the time we are live the look-back has already been flushed, so this gate
  // can no longer eat the front of a command.)
  if (live && !this->enrolling_ && this->mic_gate_until_ != 0) {
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

  if (!live) {
    // Idle or still connecting — keep the rolling PREROLL_MS instead of dropping.
    this->preroll_push_(reinterpret_cast<const uint8_t *>(resampled_24khz), resampled_bytes);
    return;
  }
  // First live frame after a connect: emit the look-back (pre-wake audio through the connect), in
  // order, BEFORE this chunk. Done here on the mic task rather than in the WS event handler so all
  // sends stay on one task and the stream can't interleave out of order.
  if (this->preroll_fill_ > 0) {
    this->preroll_flush_();
  }
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

void VoiceAssistantWebSocket::send_text_frame_(const char *json, TickType_t ticks_to_wait) {
  // Shared JSON text-frame sender for all control messages (interrupt / wake / mic_flush /
  // false_flag / button_cancel). No-op (with a warning) if the WS isn't connected.
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send control frame '%s' - not connected", json);
    return;
  }
  int sent = esp_websocket_client_send_text(this->websocket_client_, json, strlen(json), ticks_to_wait);
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
  // BOUNDED: driven by a YAML button automation, i.e. the main loop (finding 7).
  this->send_text_frame_("{\"type\":\"false_flag\"}", pdMS_TO_TICKS(50));
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
  // BOUNDED: driven by a YAML button automation, i.e. the main loop (finding 7).
  this->send_text_frame_("{\"type\":\"button_cancel\"}", pdMS_TO_TICKS(50));
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
  // Deferred: this is reached from the websocket text-frame handler, and the YAML behind it calls
  // micro_wake_word.stop - a main-loop-only API (finding 2).
  this->defer_event_(DeferredEvent::EV_ENROLL_START);
}

void VoiceAssistantWebSocket::exit_enrollment_() {
  if (!this->enrolling_) {
    return;
  }
  ESP_LOGI(TAG, "Exiting enrollment mode: restoring normal wake/stop-model operation");
  this->enrolling_ = false;
  this->enroll_start_time_ = 0;
  // The YAML enroll_stop trigger re-arms micro_wake_word - again a main-loop-only API, and this is
  // also reached from the websocket DISCONNECTED/ERROR handlers (finding 2).
  this->defer_event_(DeferredEvent::EV_ENROLL_STOP);
}

void VoiceAssistantWebSocket::interrupt() {
  if (!this->is_connected() || this->websocket_client_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send interrupt - not connected");
    return;
  }

  ESP_LOGI(TAG, "Sending interrupt message to server");

  // Send interrupt message as JSON text frame
  const char *interrupt_msg = "{\"type\":\"interrupt\"}";
  // BOUNDED: interrupt() is driven by the YAML wake-word automation on the MAIN LOOP. On timeout we
  // fall through to the existing sent<0 path and leave the speaker alone - exactly what a failed
  // send has always done - but without parking the whole loop on a stalled socket.
  int sent = esp_websocket_client_send_text(this->websocket_client_, interrupt_msg, strlen(interrupt_msg),
                                            pdMS_TO_TICKS(100));

  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send interrupt message");
  } else {
    ESP_LOGI(TAG, "Interrupt message sent successfully");
    // Stop speaker + drop the backlog atomically so buffered speech stops immediately on interrupt
    // (and the main-task drain can't re-feed stale bytes into the speaker after the stop).
    this->audio_ring_flush_and_stop_speaker_();
    // Open the mic for the follow-up NOW: clear last_speaker_audio_time_ so is_bot_speaking()
    // (the half-duplex guard) reads false immediately instead of staying true ~500ms after the
    // last server frame — that lingering guard ate the front of the barge-in follow-up command.
    this->last_speaker_audio_time_ = 0;
    // ...but last_speaker_audio_time_ is ALSO the auto-stop inactivity reference (see loop()), and
    // zeroing it falls back to running_since_ — the moment the session started, which by now is
    // long past. On any session older than auto_stop_inactivity_ms that made loop() auto-stop
    // IMMEDIATELY on barge-in: "Neo" over a reply killed the session before the follow-up command
    // could be spoken. (Symptom: 1st barge-in works, the 2nd/3rd — once the session has been alive
    // >15 s, e.g. after a long calendar readout — drops the moment you finish talking.)
    // start() gets away with the same zeroing only because it sets running_since_ fresh too.
    // Re-base it here for the same reason: post-barge-in we're waiting on a new user command with
    // no reply audio yet — exactly the "fresh turn" state running_since_ is meant to time.
    this->running_since_ = millis();
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
      this->running_since_ = millis();  // start the inactivity clock even if the bot never speaks
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

      this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);

      // Trigger connected automation - DEFERRED to loop(). The YAML behind it calls
      // mixer_speaker.apply_ducking and the LED ring's control_leds script, neither of which is
      // safe from this task (finding 2).
      this->defer_event_(DeferredEvent::EV_CONNECTED);
      break;
      
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_DISCONNECTED;

      // Never leave the box stuck in enrollment if the session drops mid-coach.
      this->exit_enrollment_();
      
      this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);
      
      // Trigger disconnected automation (re-arms micro_wake_word via on_disconnected) - DEFERRED
      // to loop(): media_player.stop + apply_ducking + the LED ring are all main-loop-only.
      this->defer_event_(DeferredEvent::EV_DISCONNECTED);

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
          // Stop speaker + drop backlog atomically (matches the local barge-in path); the serialized
          // helper stops the main-task drain from re-feeding stale bytes into the speaker after stop.
          this->audio_ring_flush_and_stop_speaker_();
        } else if (message.find("\"type\":\"disconnect\"") != std::string::npos ||
                   message.find("\"type\": \"disconnect\"") != std::string::npos) {
          ESP_LOGI(TAG, "Disconnect message received, stopping voice assistant and going to idle");
          // Mark that we received an explicit disconnect to prevent reconnection
          this->explicit_disconnect_ = true;
          // Stop the voice assistant (will go to idle mode)
          this->stop();
        } else if (message.find("\"type\":\"reboot\"") != std::string::npos ||
                   message.find("\"type\": \"reboot\"") != std::string::npos) {
          // Server-requested device reboot (Console "Reboot" button -> session-server
          // /reboot_device -> {"type":"reboot"} down this WS). Cannot reboot from the
          // websocket task; defer to loop() (main task) via pending_reboot_, mirroring the
          // pending_disconnect_ pattern above.
          //
          // DISABLED to match the Sat1 build. The Sat1 left it off so a firmware flash carrying
          // other fixes wasn't testing two variables at once; keeping the PE identical means the
          // Console's Reboot button is a no-op on BOTH devices rather than one of each. Re-arm the
          // two together when that decision is made (docs/STATE.md "Console Reboot button").
          ESP_LOGW(TAG, "reboot requested by server [handler present but disabled]");
          // this->pending_reboot_ = true;  // DISABLED: re-arm to enable the Console Reboot button
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
          // Server-driven session auto-stop (HW-agnostic "feeling" setting — the box owns it so PE,
          // Sat1, and future HW behave identically). Overrides the compiled default when present.
          size_t akey = message.find("auto_stop_inactivity_ms");
          if (akey != std::string::npos) {
            size_t acolon = message.find(':', akey);
            if (acolon != std::string::npos) {
              size_t p = acolon + 1;
              while (p < message.size() && (message[p] == ' ' || message[p] == '\t')) p++;
              long val = strtol(message.c_str() + p, nullptr, 10);
              if (val > 0) {
                this->auto_stop_inactivity_ms_ = static_cast<uint32_t>(val);
                ESP_LOGI(TAG, "hello: auto_stop_inactivity_ms = %u", this->auto_stop_inactivity_ms_);
              }
            }
          }
          // Opt into closed-loop audio flow control if the server advertises it. Prime the server
          // with our full ring capacity so it can send the first ~2s before our first drain report.
          if (message.find("\"flow_control\":\"credit\"") != std::string::npos ||
              message.find("\"flow_control\": \"credit\"") != std::string::npos) {
            this->flow_control_enabled_ = true;
            this->audio_free_reported_fill_ = this->audio_ring_fill_;
            ESP_LOGI(TAG, "hello: flow_control=credit — reporting audio_free (%zu B ring)",
                     AUDIO_RING_CAPACITY);
            this->report_audio_free_(true);
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

      this->defer_event_(DeferredEvent::EV_STATE_CALLBACK, this->state_);

      // Trigger error automation - DEFERRED to loop() (apply_ducking + LED ring).
      this->defer_event_(DeferredEvent::EV_ERROR);

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

