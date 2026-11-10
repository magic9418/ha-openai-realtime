#include "wake_stage2.h"

#ifdef USE_ESP32

#include "wake_stage2_expected.h"
#include "wake_stage2_selftest.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome::micro_wake_word {

static const char *const TAG = "micro_wake_word.stage2";

// Persisted server-pushed control (survives reboot, works offline).
struct Stage2Pref {
  uint8_t magic;  // 0xA5 when the rest is valid
  uint8_t mode;
  uint8_t threshold;
  uint8_t pad;
};
static const uint8_t PREF_MAGIC = 0xA5;
// Below this much free stack (words) at the firing invocation, stage 2 is skipped (fail-open) rather than risking
// an overflow of the 4 KB inference task stack.
static const UBaseType_t MIN_STACK_HEADROOM_WORDS = 384;  // 1536 B
static const uint32_t COUNTERS_LOG_INTERVAL_MS = 60000;

const char *Stage2Runner::mode_name(uint8_t mode) {
  switch (mode) {
    case STAGE2_SHADOW:
      return "shadow";
    case STAGE2_ENFORCE:
      return "enforce";
    default:
      return "off";
  }
}

void Stage2Runner::set_mode(uint8_t mode) {
  if (mode > STAGE2_ENFORCE) return;
  if (this->mode_.exchange(mode) != mode) this->pref_dirty_.store(true);
}

void Stage2Runner::set_threshold(uint8_t threshold) {
  if (this->threshold_.exchange(threshold) != threshold) this->pref_dirty_.store(true);
}

void Stage2Runner::setup(FeatureHistory *history) {
  this->history_ = history;
  this->ready_ = false;
  if (!this->configured_) {
    this->status_ = "notconfigured";
    return;
  }
  this->mode_.store(this->default_mode_);
  this->threshold_.store(0);
  this->pref_ = global_preferences->make_preference<Stage2Pref>(fnv1_hash("neo_mww_stage2_v1"));
  Stage2Pref p{};
  if (this->pref_.load(&p) && p.magic == PREF_MAGIC && p.mode <= STAGE2_ENFORCE) {
    this->mode_.store(p.mode);
    this->threshold_.store(p.threshold);
  }

  if (history == nullptr || !history->attached()) {
    this->status_ = "nomem";
    ESP_LOGE(TAG, "stage2 disabled: feature history not allocated");
    return;
  }
  // Arena: PSRAM only (explicit). Internal RAM is scarce (Voice PE) and the arena is ~40 KB.
  this->arena_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, this->arena_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->arena_ == nullptr) {
    this->status_ = "nopsram";
    ESP_LOGE(TAG, "stage2 DISABLED (mode off, detections pass): could not allocate %u B tensor arena in PSRAM",
             (unsigned) this->arena_size_);
    return;
  }

  const tflite::Model *model = tflite::GetModel(this->model_data_);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    this->status_ = "nomodel";
    ESP_LOGE(TAG, "stage2 DISABLED: model schema version %u unsupported", (unsigned) model->version());
    return;
  }
  if (this->resolver_.AddConv2D() != kTfLiteOk || this->resolver_.AddDepthwiseConv2D() != kTfLiteOk ||
      this->resolver_.AddMaxPool2D() != kTfLiteOk || this->resolver_.AddFullyConnected() != kTfLiteOk ||
      this->resolver_.AddLogistic() != kTfLiteOk) {
    this->status_ = "nomodel";
    ESP_LOGE(TAG, "stage2 DISABLED: op registration failed");
    return;
  }
  this->interpreter_ = std::unique_ptr<tflite::MicroInterpreter>(
      new (std::nothrow) tflite::MicroInterpreter(model, this->resolver_, this->arena_, this->arena_size_));
  if (this->interpreter_ == nullptr || this->interpreter_->AllocateTensors() != kTfLiteOk) {
    this->status_ = "nomem";
    ESP_LOGE(TAG, "stage2 DISABLED: AllocateTensors failed with a %u B arena", (unsigned) this->arena_size_);
    this->interpreter_.reset();
    return;
  }
  TfLiteTensor *in = this->interpreter_->input(0);
  TfLiteTensor *out = this->interpreter_->output(0);
  if (in == nullptr || out == nullptr || in->type != kTfLiteInt8 || out->type != kTfLiteInt8 || in->dims->size != 4 ||
      in->dims->data[1] != (int) STAGE2_WINDOW_FRAMES || in->dims->data[2] != (int) FEATURE_CHANNELS) {
    this->status_ = "nomodel";
    ESP_LOGE(TAG, "stage2 DISABLED: unexpected model input/output tensors");
    this->interpreter_.reset();
    return;
  }
  this->arena_used_ = this->interpreter_->arena_used_bytes();

  const bool ok = this->selftest_();
  this->ready_ = ok;
  this->status_ = ok ? "ok" : "selftest_fail";
  ESP_LOGI(TAG, "stage2 %s: model=%s arena used %u of %u B (PSRAM) mode=%s threshold=%u (default %u)",
           ok ? "ready" : "FORCED OFF", this->model_id_.c_str(), (unsigned) this->arena_used_,
           (unsigned) this->arena_size_, mode_name(this->mode()), (unsigned) this->threshold(),
           (unsigned) this->default_threshold_);
}

bool Stage2Runner::selftest_() {
  TfLiteTensor *in = this->interpreter_->input(0);
  int got[STAGE2_SELFTEST_VECTORS];
  bool ok = true;
  for (int k = 0; k < STAGE2_SELFTEST_VECTORS; k++) {
    stage2_selftest_vector(k, in->data.int8, STAGE2_WINDOW_FRAMES * FEATURE_CHANNELS);
    if (this->interpreter_->Invoke() != kTfLiteOk) {
      got[k] = -999;
      ok = false;
      continue;
    }
    got[k] = this->interpreter_->output(0)->data.int8[0];
    const int d = got[k] - STAGE2_SELFTEST_EXPECTED[k];
    if (d < -2 || d > 2) ok = false;
  }
  ESP_LOGI(TAG, "stage2 selftest: got %d,%d,%d expected %d,%d,%d (%s, tolerance +-2)", got[0], got[1], got[2],
           (int) STAGE2_SELFTEST_EXPECTED[0], (int) STAGE2_SELFTEST_EXPECTED[1], (int) STAGE2_SELFTEST_EXPECTED[2],
           ok ? "PASS" : "FAIL");
  return ok;
}

Stage2Result Stage2Runner::evaluate(uint32_t fire_frame) {
  Stage2Result r;
  r.fire_frame = fire_frame;
  r.cold_idle = this->cold_idle_.load(std::memory_order_relaxed);
  const uint8_t mode = this->mode();
  r.mode = mode;
  r.threshold = this->threshold();
  if (mode == STAGE2_OFF || this->interpreter_ == nullptr) return r;  // valid=false: pass
  if (uxTaskGetStackHighWaterMark(nullptr) < MIN_STACK_HEADROOM_WORDS) {
    this->n_skipped_.fetch_add(1);
    return r;
  }
  TfLiteTensor *in = this->interpreter_->input(0);
  if (!this->history_->copy_last(fire_frame, STAGE2_WINDOW_FRAMES, in->data.int8)) {
    this->n_errors_.fetch_add(1);
    return r;
  }
  const int64_t t0 = esp_timer_get_time();
  if (this->interpreter_->Invoke() != kTfLiteOk) {
    this->n_errors_.fetch_add(1);
    return r;
  }
  const int64_t dt = esp_timer_get_time() - t0;
  r.score = static_cast<uint8_t>(this->interpreter_->output(0)->data.int8[0] + 128);
  r.infer_ms = static_cast<uint16_t>(std::min<int64_t>((dt + 500) / 1000, 65535));
  r.valid = true;
  r.reject = r.score < r.threshold;
  r.enforced = r.reject && mode == STAGE2_ENFORCE && r.cold_idle;
  this->n_detections_.fetch_add(1);
  if (!r.reject)
    this->n_passed_.fetch_add(1);
  else if (r.enforced)
    this->n_rejected_.fetch_add(1);
  else
    this->n_shadow_reject_.fetch_add(1);
  this->last_infer_ms_.store(r.infer_ms);
  this->last_score_.store(r.score);
  return r;
}

void Stage2Runner::loop() {
  if (!this->configured_) return;
  if (this->pref_dirty_.exchange(false)) {
    Stage2Pref p{PREF_MAGIC, this->mode_.load(), this->threshold_.load(), 0};
    this->pref_.save(&p);
    ESP_LOGI(TAG, "stage2 control persisted: mode=%s threshold=%u (0 = default %u)", mode_name(p.mode),
             (unsigned) p.threshold, (unsigned) this->default_threshold_);
  }
  const uint32_t now = millis();
  if (now - this->last_log_ms_ >= COUNTERS_LOG_INTERVAL_MS) {
    this->last_log_ms_ = now;
    ESP_LOGI(TAG,
             "[stage2] mode=%s thr=%u status=%s det=%u pass=%u rej=%u shadow_would_reject=%u skipped=%u err=%u "
             "last_ms=%u last_score=%u arena=%u/%u",
             mode_name(this->mode()), (unsigned) this->threshold(), this->status_, (unsigned) this->n_detections_.load(),
             (unsigned) this->n_passed_.load(), (unsigned) this->n_rejected_.load(),
             (unsigned) this->n_shadow_reject_.load(), (unsigned) this->n_skipped_.load(),
             (unsigned) this->n_errors_.load(), (unsigned) this->last_infer_ms_.load(),
             (unsigned) this->last_score_.load(), (unsigned) this->arena_used_, (unsigned) this->arena_size_);
  }
}

void Stage2Runner::dump_config() {
  ESP_LOGCONFIG(TAG,
                "  Stage 2: model=%s status=%s mode=%s threshold=%u (default %u) arena=%u B PSRAM, feature history %u B",
                this->model_id_.c_str(), this->status_, mode_name(this->mode()), (unsigned) this->threshold(),
                (unsigned) this->default_threshold_, (unsigned) this->arena_size_, (unsigned) FeatureHistory::BYTES);
}

}  // namespace esphome::micro_wake_word

#endif  // USE_ESP32
