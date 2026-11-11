#pragma once

#ifdef USE_ESP32

#include "wake_features.h"

#include "esphome/core/preferences.h"

#include <atomic>
#include <memory>
#include <string>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

namespace esphome::micro_wake_word {

/// Second-stage wake check ("stage 2"): a tiny int8 TFLite-Micro model that scores the 100 feature frames
/// ending at the stage-1 firing frame. Runs ONCE per stage-1 detection, on the mww inference task.
/// Fail-open everywhere: any allocation/selftest/inference problem => mode reports "off", detections pass.
class Stage2Runner {
 public:
  void configure(const uint8_t *model, size_t arena_size, uint8_t default_mode, uint8_t default_threshold,
                 const char *model_id) {
    this->model_data_ = model;
    this->arena_size_ = arena_size;
    this->default_mode_ = default_mode;
    this->default_threshold_ = default_threshold;
    this->model_id_ = model_id;
    this->configured_ = model != nullptr;
  }
  bool configured() const { return this->configured_; }

  /// Main task, from MicroWakeWord::setup(): allocate (PSRAM), AllocateTensors, boot self-test, load prefs.
  void setup(FeatureHistory *history);
  /// Main task, every loop: persists a server-pushed mode/threshold, prints the once-a-minute counters line.
  void loop(uint32_t frames_total, uint32_t infers_total);
  void dump_config(uint32_t frames_total, uint32_t infers_total);

  /// Inference task only. Scores the window ending at `fire_frame`.
  Stage2Result evaluate(uint32_t fire_frame);

  // Thread-safe runtime control (server hello / YAML). threshold 0 = "use the compiled default".
  void set_mode(uint8_t mode);
  void set_threshold(uint8_t threshold);
  /// capture-v3 feature upload bookkeeping (counted by voice_assistant_websocket).
  void note_feature_upload(bool sent) { (sent ? this->n_feat_sent_ : this->n_feat_skipped_).fetch_add(1); }
  void set_cold_idle(bool cold_idle) { this->cold_idle_.store(cold_idle, std::memory_order_relaxed); }

  /// Effective mode: "off" whenever stage 2 is not usable, whatever was requested.
  uint8_t mode() const { return this->ready_ ? this->mode_.load() : uint8_t(STAGE2_OFF); }
  uint8_t requested_mode() const { return this->mode_.load(); }
  uint8_t threshold() const {
    const uint8_t t = this->threshold_.load();
    return t == 0 ? this->default_threshold_ : t;
  }
  bool ready() const { return this->ready_; }
  // ok | nohist | nopsram | alloc_tensors | nomodel | selftest_fail | notconfigured
  const char *status() const { return this->status_; }
  const std::string &model_id() const { return this->model_id_; }
  static const char *mode_name(uint8_t mode);

 protected:
  bool selftest_();
  bool build_interpreter_(size_t arena_size);  // alloc arena + interpreter + AllocateTensors; frees on failure
  void release_();
  void log_summary_(const char *prefix, uint32_t frames_total, uint32_t infers_total);
  tflite::MicroMutableOpResolver<5> resolver_;
  std::unique_ptr<tflite::MicroInterpreter> interpreter_;
  FeatureHistory *history_{nullptr};
  uint8_t *arena_{nullptr};
  size_t arena_size_{40000};
  const uint8_t *model_data_{nullptr};
  std::string model_id_;
  bool configured_{false};
  bool ready_{false};
  const char *status_{"notconfigured"};
  uint8_t default_mode_{STAGE2_SHADOW};
  uint8_t default_threshold_{0};
  std::atomic<uint8_t> mode_{STAGE2_SHADOW};
  std::atomic<uint8_t> threshold_{0};  // 0 = compiled default
  std::atomic<bool> cold_idle_{true};
  std::atomic<bool> pref_dirty_{false};
  ESPPreferenceObject pref_;

  // Counters: written by the inference task, read by the main loop (relaxed is enough for diagnostics).
  std::atomic<uint32_t> n_detections_{0}, n_passed_{0}, n_rejected_{0}, n_shadow_reject_{0}, n_skipped_{0},
      n_errors_{0};
  std::atomic<uint32_t> n_feat_sent_{0}, n_feat_skipped_{0};
  std::atomic<uint32_t> last_infer_ms_{0};
  std::atomic<uint32_t> last_score_{0};
  size_t arena_used_{0};
  size_t arena_actual_{0};      // arena size actually in use (after an optional 2x retry)
  char selftest_text_[40]{"n/a"};
  uint32_t setup_free_psram_{0}, setup_free_internal_{0};
  uint32_t last_log_ms_{0};
};

}  // namespace esphome::micro_wake_word

#endif  // USE_ESP32
