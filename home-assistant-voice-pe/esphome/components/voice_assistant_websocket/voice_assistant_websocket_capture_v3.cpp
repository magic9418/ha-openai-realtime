// Wake capture v3 for voice_assistant_websocket (core/docs/wake_capture_v3_protocol.md):
//  * stage-2 / feature metadata for the normal trigger upload (see wake_trigger_loop_ in the main .cpp),
//  * server -> device stage-2 control (hello "wake_stage2"),
//  * CAPTURE-ONLY uploads for detections stage 2 rejected in enforce mode.
//
// CAPTURE-ONLY DESIGN (read this before touching it). A rejected detection must be INVISIBLE to the user:
// no LED, no duck, no chime, no mic uplink. Everything the YAML observes (on_connected/on_start/... via
// defer_event_, state_, state_callback_, audio_transport_ready_, the uplink ring, websocket_client_) belongs
// to the real session and is NEVER touched here. Capture-only has its OWN websocket client handle
// (co_client_), its OWN event handler (co_event_handler_, which only sets atomics), and its OWN state
// machine (co_state_). state_ stays IDLE throughout, so the regime/LED/duck logic cannot see it.
// A real wake (start(), capture_wake_trigger(), i.e. stage-2 pass / button / server-initiated session)
// calls co_preempt_() first: the capture-only client is handed to a cleanup worker task and the real session
// proceeds in the same loop iteration with no wait.
#include "voice_assistant_websocket.h"

#ifdef USE_ESP_IDF

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"

#include <cstring>

static const char *TAG = "voice_assistant_websocket";

namespace esphome {
namespace voice_assistant_websocket {

static const uint32_t CO_TOTAL_TIMEOUT_MS = 10000;   // protocol: close 10 s after connect at the latest
static const uint32_t CO_MIN_INTERVAL_MS = 10000;    // device-side limit: >= 10 s between uploads (start to start)
static const uint32_t CO_POST_TIMEOUT_MS = 2000;     // mic produced no post-trigger audio: drop the job
static const size_t CO_FEAT_BYTES = WF_FEATURE_BYTES_MAX;

void VoiceAssistantWebSocket::capture_v3_init_() {
  if (!this->wt_enabled_) return;  // no detector-audio ring: nothing to capture
  // ONE PSRAM block: 2 capture-only audio snapshots + 3 feature windows (2 capture-only + 1 normal wake).
  const size_t bytes = 2 * WT_BYTES + 3 * CO_FEAT_BYTES;
  this->co_mem_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->co_mem_ == nullptr) {
    ESP_LOGE(TAG, "Capture v3 DISABLED (no capture-only uploads, no feature upload): PSRAM alloc of %u B failed",
             (unsigned) bytes);
    return;
  }
  this->co_audio_[0] = this->co_mem_;
  this->co_audio_[1] = this->co_mem_ + WT_BYTES;
  this->co_feat_[0] = reinterpret_cast<int8_t *>(this->co_mem_ + 2 * WT_BYTES);
  this->co_feat_[1] = reinterpret_cast<int8_t *>(this->co_mem_ + 2 * WT_BYTES + CO_FEAT_BYTES);
  this->wt_feat_ = reinterpret_cast<int8_t *>(this->co_mem_ + 2 * WT_BYTES + 2 * CO_FEAT_BYTES);
  this->co_enabled_ = true;
  ESP_LOGI(TAG, "Capture v3 ready: %u B PSRAM (2 capture-only clips %u + 3 feature windows %u)", (unsigned) bytes,
           (unsigned) (2 * WT_BYTES), (unsigned) (3 * CO_FEAT_BYTES));
}

void VoiceAssistantWebSocket::apply_wake_stage2_(uint8_t mode, uint8_t threshold) {
  if (this->detector_ == nullptr) return;
  // Thread-safe setters; the detector persists the values from its own main-loop pass.
  this->detector_->set_stage2_mode(mode);
  this->detector_->set_stage2_threshold(threshold);
  ESP_LOGI(TAG, "hello: wake_stage2 mode=%s threshold=%u (0 = firmware default)", wf_mode_name(mode),
           (unsigned) threshold);
}

// Snapshot of the detector's verdict for the detection being dispatched (main loop, inside the YAML trigger).
void VoiceAssistantWebSocket::capture_s2_meta_(WakeS2Meta &m, uint32_t &fire_frame) {
  m = WakeS2Meta{};
  fire_frame = 0xFFFFFFFFu;
  if (this->detector_ == nullptr) return;
  const auto &r = this->detector_->get_last_stage2();
  const char *st = this->detector_->get_stage2_status();
  fire_frame = r.fire_frame;
  snprintf(m.status, sizeof(m.status), "%s", st);
  snprintf(m.model, sizeof(m.model), "%s", this->detector_->get_stage2_model_id().c_str());
  if (r.valid) {
    m.present = true;
    m.mode = r.mode;
    m.score = r.score;
    m.threshold = r.threshold;
    m.reject = r.reject;
    m.infer_ms = r.infer_ms;
  } else if (std::strcmp(st, "ok") != 0 && std::strcmp(st, "notconfigured") != 0) {
    // Stage 2 forced off (self-test failed / no memory): say so, so the server sidecar shows why.
    m.present = true;
    m.mode = 0;
    m.threshold = this->detector_->get_stage2_threshold();
  }
  m.follow_features = this->co_enabled_ && fire_frame != 0xFFFFFFFFu;
  if (!m.follow_features) this->set_features_skip_(m, fire_frame);
}

// Short machine-readable reason a feature upload cannot happen (diagnosable from the sidecar next time).
void VoiceAssistantWebSocket::set_features_skip_(WakeS2Meta &m, uint32_t fire_frame) {
  const char *why = "window";
  if (this->detector_ == nullptr) why = "nodetector";
  else if (!this->co_enabled_ || this->wt_feat_ == nullptr) why = "nobuf";
  else if (!this->detector_->feature_history_ready()) why = "disabled";
  snprintf(m.features_skip, sizeof(m.features_skip), "%s", why);
  (void) fire_frame;
}

// YAML on_wake_word_rejected -> here. Main loop only.
void VoiceAssistantWebSocket::capture_rejected_wake(uint8_t cutoff, const char *regime, int score_avg, int score_max) {
  if (!this->co_enabled_ || this->enrolling_ || this->detector_ == nullptr) {
    this->co_n_dropped_++;
    return;
  }
  if (this->wt_ring_.filled_bytes() < WT_PRE_BYTES) {
    this->co_n_dropped_++;
    return;
  }
  // Limits: one in flight (co_active_), at most ONE more queued; everything else is dropped and counted.
  int queued = 0, slot = -1;
  for (int i = 0; i < 2; i++) {
    if (this->co_jobs_[i].phase != 0 && i != this->co_active_) queued++;
    if (this->co_jobs_[i].phase == 0 && slot < 0) slot = i;
  }
  if (queued >= 1 || slot < 0) {
    this->co_n_dropped_++;
    ESP_LOGI(TAG, "Capture-only: dropped (in flight/queued limit), %u dropped so far", (unsigned) this->co_n_dropped_);
    return;
  }
  CoJob &j = this->co_jobs_[slot];
  j = CoJob{};
  capture_s2_meta_(j.s2, j.fire_frame);
  j.cutoff = cutoff;
  snprintf(j.regime, sizeof(j.regime), "%s", regime != nullptr ? regime : "idle");
  j.score_avg = (score_avg >= 0 && score_avg <= 255) ? (int16_t) score_avg : (int16_t) -1;
  j.score_max = (score_max >= 0 && score_max <= 255) ? (int16_t) score_max : (int16_t) -1;
  j.end_total = wt_total_add(this->wt_ring_.total_bytes(), (uint32_t) WT_POST_BYTES);
  j.armed_ms = millis();
  j.phase = 1;  // waiting for the post-trigger audio
  ESP_LOGI(TAG, "Capture-only armed (stage-2 rejected: score %u < %u)", (unsigned) j.s2.score, (unsigned) j.s2.threshold);
}

void VoiceAssistantWebSocket::co_free_job_(int i) {
  if (i < 0 || i > 1) return;
  this->co_jobs_[i].phase = 0;
  if (this->co_active_ == i) this->co_active_ = -1;
}

// Main loop. Hands the capture-only client (if any) to a cleanup worker; never blocks.
void VoiceAssistantWebSocket::co_teardown_(bool graceful) {
  esp_websocket_client_handle_t client = this->co_client_;
  this->co_client_ = nullptr;
  this->co_state_ = CO_IDLE;
  this->co_free_job_(this->co_active_);
  if (client == nullptr) return;
  this->co_cleanup_client_ = client;
  this->co_cleanup_graceful_ = graceful;
  this->co_cleanup_active_ = true;
  if (xTaskCreate(&VoiceAssistantWebSocket::co_cleanup_task_, "va_co_clean", 4096, this, 3, nullptr) != pdPASS) {
    ESP_LOGW(TAG, "Capture-only: no cleanup worker; closing inline");
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    this->co_cleanup_client_ = nullptr;
    this->co_cleanup_active_ = false;
  }
}

void VoiceAssistantWebSocket::co_cleanup_task_(void *param) {
  auto *self = static_cast<VoiceAssistantWebSocket *>(param);
  esp_websocket_client_handle_t c = self->co_cleanup_client_;
  if (c != nullptr) {
    if (self->co_cleanup_graceful_ && esp_websocket_client_is_connected(c)) {
      if (esp_websocket_client_close(c, pdMS_TO_TICKS(1000)) != ESP_OK) esp_websocket_client_stop(c);
    } else {
      esp_websocket_client_stop(c);
    }
    esp_websocket_client_destroy(c);
  }
  self->co_cleanup_client_ = nullptr;
  self->co_cleanup_active_ = false;
  vTaskDelete(nullptr);
}

// A REAL wake arrived (stage-2 pass, button, server-initiated session): drop the capture-only upload NOW.
// The in-flight job is discarded (its clip is lost, the pending one is kept); nothing here waits.
void VoiceAssistantWebSocket::co_preempt_() {
  if (this->co_state_.load() == CO_IDLE) return;
  ESP_LOGI(TAG, "Capture-only upload pre-empted by a real wake (state %u)", (unsigned) this->co_state_.load());
  this->co_n_preempted_++;
  this->co_teardown_(false);
}

void VoiceAssistantWebSocket::co_start_connect_(int job) {
  this->co_url_ = this->server_url_;
  this->co_url_ += (this->co_url_.find('?') == std::string::npos) ? "?capture_only=1" : "&capture_only=1";
  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->co_url_.c_str();
  cfg.user_context = this;
  cfg.buffer_size = 2048;  // rx is only hello/ack; tx frames <= 1.5 KB (features) or fragmented (audio, as today)
  cfg.task_prio = 4;
  cfg.task_stack = 6144;
  cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;
  cfg.network_timeout_ms = 5000;
  cfg.disable_auto_reconnect = true;
  this->co_client_ = esp_websocket_client_init(&cfg);
  if (this->co_client_ == nullptr) {
    this->co_n_failed_++;
    this->co_free_job_(job);
    return;
  }
  this->co_hello_ = 0;
  this->co_ack_ = 0;
  this->co_connected_ = false;
  this->co_closed_ = false;
  esp_websocket_register_events(this->co_client_, (esp_websocket_event_id_t) WEBSOCKET_EVENT_ANY,
                                &VoiceAssistantWebSocket::co_event_handler_, this);
  this->co_active_ = job;
  this->co_seq_ = 0;
  this->co_old_server_ = false;
  this->co_start_ms_ = millis();
  this->co_last_start_ms_ = this->co_start_ms_;
  this->co_ever_started_ = true;
  this->co_state_ = CO_CONNECTING;
  this->co_n_started_++;
  if (esp_websocket_client_start(this->co_client_) != ESP_OK) {
    ESP_LOGW(TAG, "Capture-only: client start failed");
    this->co_n_failed_++;
    this->co_teardown_(false);
  }
}

// Websocket task. ONLY sets atomics: no state_, no defer_event_, no triggers, no logging of audio state.
void VoiceAssistantWebSocket::co_event_handler_(void *args, esp_event_base_t base, int32_t event_id, void *data) {
  auto *self = static_cast<VoiceAssistantWebSocket *>(args);
  auto *ev = static_cast<esp_websocket_event_data_t *>(data);
  switch ((esp_websocket_event_id_t) event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      self->co_connected_ = true;
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
      self->co_closed_ = true;
      break;
    case WEBSOCKET_EVENT_DATA:
      if (ev != nullptr && ev->op_code == 0x01 && ev->data_len > 0 && ev->data_len < 1024) {
        const std::string m((const char *) ev->data_ptr, ev->data_len);
        bool ok = false;
        if (wf_is_type(m, "hello")) {
          uint8_t md, th;
          if (wf_parse_wake_stage2(m, md, th)) self->apply_wake_stage2_(md, th);
          self->co_hello_ = wf_hello_is_capture_only(m) ? 1 : 2;
        } else if (wf_parse_capture_ack(m, ok)) {
          self->co_ack_ = ok ? 1 : 2;
        }
      }
      break;
    default:
      break;
  }
}

void VoiceAssistantWebSocket::co_loop_() {
  if (!this->co_enabled_) return;
  const uint32_t now = millis();
  // 1. Jobs waiting for their post-trigger audio: freeze clip + features into the job's own buffers.
  for (int i = 0; i < 2; i++) {
    CoJob &j = this->co_jobs_[i];
    if (j.phase != 1) continue;
    if (!this->wt_ring_.reached(j.end_total)) {
      if ((now - j.armed_ms) > CO_POST_TIMEOUT_MS) {
        this->co_n_dropped_++;
        this->co_free_job_(i);
      }
      continue;
    }
    if (!this->wt_ring_.snapshot_ending_at(j.end_total, this->co_audio_[i])) {
      ESP_LOGW(TAG, "Capture-only: ring overrun/late snapshot; dropping");
      this->co_n_dropped_++;
      this->co_free_job_(i);
      continue;
    }
    j.feat_ok = j.s2.follow_features && this->detector_ != nullptr &&
                this->detector_->copy_feature_upload(j.fire_frame, 50, this->co_feat_[i], &j.fw);
    j.s2.follow_features = j.feat_ok;
    if (!j.feat_ok) this->set_features_skip_(j.s2, j.fire_frame);
    if (!j.feat_ok && this->detector_ != nullptr) this->detector_->note_feature_upload(false);
    j.phase = 2;
  }

  const uint8_t st = this->co_state_.load();
  if (st == CO_IDLE) {
    if (this->co_active_ >= 0 || this->co_cleanup_active_.load()) return;
    // Quiet start conditions: no session, no wake queued, no normal trigger upload, network up, >= 10 s apart.
    if (this->state_ != VOICE_ASSISTANT_WEBSOCKET_IDLE || this->pending_start_ || this->wt_state_.load() != WT_NONE ||
        this->websocket_client_ != nullptr || !network::is_connected())
      return;
    if (this->co_ever_started_ && (now - this->co_last_start_ms_) < CO_MIN_INTERVAL_MS) return;
    const int pick = (this->co_jobs_[0].phase == 2) ? 0 : ((this->co_jobs_[1].phase == 2) ? 1 : -1);
    if (pick < 0) return;
    if (this->co_jobs_[pick].phase == 2 && this->co_jobs_[1 - pick].phase == 2 &&
        (int32_t)(this->co_jobs_[1 - pick].armed_ms - this->co_jobs_[pick].armed_ms) < 0)
      this->co_start_connect_(1 - pick);  // oldest first
    else
      this->co_start_connect_(pick);
    return;
  }

  // In flight.
  if ((now - this->co_start_ms_) > CO_TOTAL_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Capture-only: 10 s limit reached; closing");
    this->co_n_failed_++;
    this->co_teardown_(false);
    return;
  }
  if (this->co_closed_.load() && this->co_ack_.load() == 0 && st != CO_WAIT_ACK) {
    this->co_n_failed_++;
    this->co_teardown_(false);
    return;
  }
  CoJob &j = this->co_jobs_[this->co_active_];
  switch (st) {
    case CO_CONNECTING:
      if (this->co_connected_.load()) this->co_state_ = CO_WAIT_HELLO;
      break;
    case CO_WAIT_HELLO:
      if (this->co_ack_.load() == 2) {  // rate_limited straight after hello: nothing to upload
        ESP_LOGI(TAG, "Capture-only: server refused (rate limited); not uploading");
        this->co_n_dropped_++;
        this->co_teardown_(true);
      } else if (this->co_hello_.load() != 0) {
        this->co_old_server_ = this->co_hello_.load() == 2;
        this->co_seq_ = 0;
        this->co_next_ms_ = now;
        this->co_state_ = CO_SENDING;
      }
      break;
    case CO_SENDING: {
      if ((int32_t)(now - this->co_next_ms_) < 0) break;
      if (this->co_ack_.load() == 2) {
        this->co_n_dropped_++;
        this->co_teardown_(true);
        break;
      }
      char *buf = this->wt_ring_.json_buf();
      const size_t chunks = wt_chunk_count(WT_BYTES);
      const size_t nfeat = j.feat_ok ? wf_feature_frame_count(j.fw) : 0;
      const size_t total = chunks + 2 + nfeat;
      size_t n = 0;
      if (this->co_seq_ == 0) {
        n = wf_build_trigger_start(buf, WT_JSON_BYTES, false, true, WT_SAMPLE_RATE, WT_SAMPLES, (uint32_t) chunks,
                                   this->enrollment_wake_channel_, this->enrollment_wake_gain_factor_, j.cutoff,
                                   this->wt_window_, j.regime, this->wt_model_.c_str(), this->wt_fw_.c_str(), WT_POST_MS,
                                   WT_PRE_MS, j.score_avg, j.score_max, j.s2);
      } else if (this->co_seq_ <= chunks) {
        n = wt_build_chunk_json(this->co_audio_[this->co_active_], WT_BYTES, this->co_seq_ - 1, buf, WT_JSON_BYTES);
      } else if (this->co_seq_ == chunks + 1) {
        n = (size_t) snprintf(buf, WT_JSON_BYTES, "{\"type\":\"wake_trigger_end\",\"samples\":%u}", (unsigned) WT_SAMPLES);
      } else {
        n = wf_build_feature_frame(this->co_seq_ - chunks - 2, this->co_feat_[this->co_active_], j.fw, buf, WT_JSON_BYTES);
      }
      if (n == 0 || this->co_client_ == nullptr ||
          esp_websocket_client_send_text(this->co_client_, buf, (int) n, pdMS_TO_TICKS(50)) != (int) n) {
        ESP_LOGW(TAG, "Capture-only: frame %u not sent; aborting", (unsigned) this->co_seq_);
        this->co_n_failed_++;
        this->co_teardown_(false);
        break;
      }
      this->co_seq_++;
      this->co_next_ms_ = now + WT_PACE_MS;  // same pacing as the normal upload
      if (this->co_seq_ >= total) {
        if (j.feat_ok && this->detector_ != nullptr) this->detector_->note_feature_upload(true);
        if (this->co_old_server_) {  // old server: no capture_ack ever; close ourselves right after the last frame
          ESP_LOGI(TAG, "Capture-only upload sent (old server, closing)");
          this->co_n_acked_++;
          this->co_teardown_(true);
        } else {
          this->co_state_ = CO_WAIT_ACK;
        }
      }
      break;
    }
    case CO_WAIT_ACK:
      if (this->co_ack_.load() != 0) {
        if (this->co_ack_.load() == 1) this->co_n_acked_++;
        ESP_LOGI(TAG, "Capture-only done (ack %s); uploads ok/fail/drop/preempt = %u/%u/%u/%u",
                 this->co_ack_.load() == 1 ? "ok" : "refused", (unsigned) this->co_n_acked_,
                 (unsigned) this->co_n_failed_, (unsigned) this->co_n_dropped_, (unsigned) this->co_n_preempted_);
        this->co_teardown_(true);
      } else if (this->co_closed_.load()) {
        this->co_n_failed_++;
        this->co_teardown_(false);
      }
      break;
    default:
      this->co_teardown_(false);
      break;
  }
}

}  // namespace voice_assistant_websocket
}  // namespace esphome

#endif  // USE_ESP_IDF
