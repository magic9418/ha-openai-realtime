#pragma once

#ifdef USE_ESP32

#include "preprocessor_settings.h"
#include "streaming_model.h"
#include "wake_features.h"
#include "wake_stage2.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/static_task.h"

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

#include <freertos/event_groups.h>

#include <frontend.h>
#include <frontend_util.h>

namespace esphome::micro_wake_word {

enum State {
  STARTING,
  DETECTING_WAKE_WORD,
  STOPPING,
  STOPPED,
};

class MicroWakeWord : public Component
#ifdef USE_OTA_STATE_LISTENER
    ,
                      public ota::OTAGlobalStateListener
#endif
{
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;
  void dump_config() override;

#ifdef USE_OTA_STATE_LISTENER
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

  void start();
  void stop();

  bool is_running() const { return this->state_ != State::STOPPED; }

  void set_features_step_size(uint8_t step_size) { this->features_step_size_ = step_size; }

  void set_microphone_source(microphone::MicrophoneSource *microphone_source) {
    this->microphone_source_ = microphone_source;
  }

  void set_stop_after_detection(bool stop_after_detection) { this->stop_after_detection_ = stop_after_detection; }

  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  Trigger<std::string> *get_wake_word_detected_trigger() { return &this->wake_word_detected_trigger_; }

  // NEO (held-duck patch): confidence of the most recent wake-word detection, quantised 0-255
  // (cutoff scale). average = sliding-window mean that crossed the cutoff; max = peak single
  // inference in that window. Valid inside on_wake_word_detected (same main-loop call).
  uint8_t get_last_detection_average() const { return this->last_detection_average_; }
  uint8_t get_last_detection_max() const { return this->last_detection_max_; }

  // ---- NEO stage 2 (second-stage wake check) + feature history; see NEO_PATCH.md ----------------------
  void configure_stage2(const uint8_t *model, size_t model_len, size_t arena_size, uint8_t default_mode,
                        uint8_t default_threshold, const char *model_id) {
    this->stage2_.configure(model, arena_size, default_mode, default_threshold, model_id);
  }
  /// Fires (main loop) for a stage-1 detection stage 2 rejected in enforce mode. The normal
  /// on_wake_word_detected trigger is NOT raised for it; the detector re-arms exactly as after a detection.
  Trigger<std::string> *get_wake_word_rejected_trigger() { return &this->wake_word_rejected_trigger_; }
  // Thread-safe runtime control. threshold 0 = compiled default. Persisted across reboots (main loop).
  void set_stage2_mode(uint8_t mode) { this->stage2_.set_mode(mode); }
  void set_stage2_threshold(uint8_t threshold) { this->stage2_.set_threshold(threshold); }
  // Regime: true only for a cold idle wake (no session, no reply, no music). Outside it stage 2 only shadows.
  void set_stage2_cold_idle(bool cold_idle) { this->stage2_.set_cold_idle(cold_idle); }
  uint8_t get_stage2_mode() const { return this->stage2_.mode(); }
  uint8_t get_stage2_threshold() const { return this->stage2_.threshold(); }
  const char *get_stage2_status() const { return this->stage2_.status(); }
  const char *get_stage2_mode_name() const { return Stage2Runner::mode_name(this->stage2_.mode()); }
  const std::string &get_stage2_model_id() const { return this->stage2_.model_id(); }
  /// Stage-2 result of the detection being dispatched (valid inside on_wake_word_detected / _rejected).
  const Stage2Result &get_last_stage2() const { return this->last_stage2_; }
  /// Main loop. Copy the feature-upload window for the detection whose fire frame is `fire_frame`
  /// (up to `want_post` frames after it) into dst (FEATURE_UPLOAD_MAX_FRAMES * 40 bytes).
  bool copy_feature_upload(uint32_t fire_frame, size_t want_post, int8_t *dst, FeatureWindow *w) const {
    const uint8_t stride = this->wake_word_models_.empty() ? uint8_t(0) : this->wake_word_models_[0]->get_stride();
    return this->history_.copy_upload(fire_frame, stride == 0 ? uint8_t(3) : stride, want_post, dst, w);
  }

  void add_wake_word_model(WakeWordModel *model);

#ifdef USE_MICRO_WAKE_WORD_VAD
  void add_vad_model(const uint8_t *model_start, uint8_t probability_cutoff, size_t sliding_window_size,
                     size_t tensor_arena_size);

  // Intended for the voice assistant component to fetch VAD status
  bool get_vad_state() { return this->vad_state_; }
#endif

  // Intended for the voice assistant component to access which wake words are available
  // Since these are pointers to the WakeWordModel objects, the voice assistant component can enable or disable them
  std::vector<WakeWordModel *> get_wake_words();

 protected:
  microphone::MicrophoneSource *microphone_source_{nullptr};
  Trigger<std::string> wake_word_detected_trigger_;
  Trigger<std::string> wake_word_rejected_trigger_;
  Stage2Runner stage2_;
  FeatureHistory history_;  // PSRAM, 250 frames + probability rings
  Stage2Result last_stage2_;
  uint8_t last_detection_average_{0};
  uint8_t last_detection_max_{0};
  State state_{State::STOPPED};

  std::weak_ptr<ring_buffer::RingBuffer> ring_buffer_;
  std::vector<WakeWordModel *> wake_word_models_;

#ifdef USE_MICRO_WAKE_WORD_VAD
  std::unique_ptr<VADModel> vad_model_;
  bool vad_state_{false};
#endif

  bool pending_start_{false};
  bool pending_stop_{false};

  bool stop_after_detection_;

  bool task_stack_in_psram_{false};

  uint8_t features_step_size_;

  // Audio frontend handles generating spectrogram features
  struct FrontendConfig frontend_config_;
  struct FrontendState frontend_state_;

  // Handles managing the stop/state of the inference task
  EventGroupHandle_t event_group_;

  // Used to send messages about the models' states to the main loop
  QueueHandle_t detection_queue_;

  StaticTask inference_task_;

  static void inference_task(void *params);

  /// @brief Suspends the inference task
  void suspend_task_();
  /// @brief Resumes the inference task
  void resume_task_();

  void set_state_(State state);

  /// NEO: record one feature frame + the stage-1/VAD probabilities held as of it (inference task, no allocation).
  void record_feature_frame_(const int8_t *features);
  uint32_t total_stage1_invokes_() const;

  /// @brief Generates a spectrogram feature from an input buffer of audio samples. The frontend buffers samples
  /// internally, so callers may stream arbitrary-sized chunks; a feature is only emitted once enough samples have
  /// accumulated to fill a full analysis window.
  /// @param audio_buffer (const int16_t *) Buffer containing input audio samples
  /// @param samples_available (size_t) Number of samples available in the input buffer
  /// @param features_buffer (int8_t *) Buffer to store the generated feature, valid only when the return value is true
  /// @param processed_samples (size_t *) Set to the number of samples consumed from the input buffer
  /// @return True if a new feature was generated; false if more samples are required
  bool generate_features_(const int16_t *audio_buffer, size_t samples_available,
                          int8_t features_buffer[PREPROCESSOR_FEATURE_SIZE], size_t *processed_samples);

  /// @brief Processes any new probabilities for each model. If any wake word is detected, it will send a DetectionEvent
  /// to the detection_queue_.
  void process_probabilities_();

  /// @brief Deletes each model's TFLite interpreters and frees tensor arena memory.
  void unload_models_();

  /// @brief Runs an inference with each model using the new spectrogram features
  /// @param audio_features (int8_t *) Buffer containing new spectrogram features
  /// @return True if successful, false if any errors were encountered
  bool update_model_probabilities_(const int8_t audio_features[PREPROCESSOR_FEATURE_SIZE]);
};

}  // namespace esphome::micro_wake_word

#endif  // USE_ESP32
