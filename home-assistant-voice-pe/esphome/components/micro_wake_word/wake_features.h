#pragma once
// NEO stage-2 / capture-v3: feature history ring + shared result types. Pure logic (no ESPHome / IDF /
// TFLite dependency) so it is host-testable: tests/test_wake_stage2.cpp.
//
// The ring holds the last FEATURE_HISTORY_FRAMES int8 feature frames (40 B each, exactly what the
// stage-1 model consumes), plus, per frame, the stage-1 and VAD probability HELD from their most recent
// invocation. Layout is ONE block of BYTES, allocated once by the component (PSRAM), never in the hot path.
//
// CONCURRENCY: single producer (the mww inference task) / reader (stage-2 on the same task, or the main
// loop for the upload). A reader copies, then re-checks that the producer did not wrap over its oldest
// frame while it copied (seqlock-style); a failed check returns false and the caller drops the data.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::micro_wake_word {

constexpr size_t FEATURE_HISTORY_FRAMES = 250;   // 2.5 s at the 10 ms feature step
constexpr size_t FEATURE_CHANNELS = 40;          // == PREPROCESSOR_FEATURE_SIZE
constexpr size_t FEATURE_UPLOAD_MAX_FRAMES = FEATURE_HISTORY_FRAMES;
constexpr size_t FEATURE_UPLOAD_PRE_FRAMES = 198;  // fire frame + 197 before it (multiple of the stride 3)
constexpr size_t FEATURE_UPLOAD_MAX_PROBS = FEATURE_HISTORY_FRAMES / 3 + 1;  // 84
constexpr size_t STAGE2_WINDOW_FRAMES = 100;     // model input: 1.0 s
constexpr size_t FEATURE_COPY_MARGIN = 3;        // newest-overwrite safety margin when copying a window

enum Stage2Mode : uint8_t { STAGE2_OFF = 0, STAGE2_SHADOW = 1, STAGE2_ENFORCE = 2 };

// Outcome of one stage-2 evaluation, produced on the inference task, carried to the main loop inside the
// DetectionEvent. `valid` false = stage 2 did not score this detection (off / not ready / error): treat as pass.
struct Stage2Result {
  bool valid{false};
  uint8_t mode{STAGE2_OFF};
  uint8_t score{0};          // 0-255 (= int8 output + 128)
  uint8_t threshold{0};      // resolved threshold applied
  bool reject{false};        // verdict: score < threshold (in shadow = "would reject")
  bool enforced{false};      // reject AND mode==enforce AND cold-idle regime: the detection is dropped
  bool cold_idle{false};     // regime seen at scoring time
  uint16_t infer_ms{0};
  uint32_t fire_frame{0};    // absolute index of the stage-1 firing frame in the feature history
};

// Metadata for one feature upload (see core/docs/wake_capture_v3_protocol.md section 4).
struct FeatureWindow {
  uint16_t frames{0};      // N: frames copied (pre + post)
  uint16_t pre_frames{0};  // frames up to and including the fire frame
  uint16_t post_frames{0}; // M: frames after the fire frame
  uint16_t fire_idx{0};    // index of the fire frame inside the upload (= pre_frames - 1 = N - 1 - M)
  uint8_t stride{3};
  uint8_t n_probs{0};      // floor(N / stride)
  bool start_frame_is_invocation_start{false};
  uint8_t probs[FEATURE_UPLOAD_MAX_PROBS]{};
  uint8_t vad[FEATURE_UPLOAD_MAX_PROBS]{};
};

class FeatureHistory {
 public:
  static constexpr size_t BYTES = FEATURE_HISTORY_FRAMES * FEATURE_CHANNELS + 2 * FEATURE_HISTORY_FRAMES;

  bool attach(uint8_t *mem) {
    if (mem == nullptr) return false;
    feats_ = reinterpret_cast<int8_t *>(mem);
    prob_ = mem + FEATURE_HISTORY_FRAMES * FEATURE_CHANNELS;
    vad_ = prob_ + FEATURE_HISTORY_FRAMES;
    written_.store(0);
    return true;
  }
  bool attached() const { return feats_ != nullptr; }
  void reset() { written_.store(0, std::memory_order_release); }
  uint32_t frames_written() const { return written_.load(std::memory_order_acquire); }
  /// Frames ever pushed (never reset): a liveness counter that proves the detector is consuming audio.
  uint32_t total_pushed() const { return total_.load(std::memory_order_relaxed); }

  // PRODUCER. `prob`/`vad` = latest stage-1 / VAD outputs as of this frame (held between invocations).
  void push(const int8_t *frame, uint8_t prob, uint8_t vad) {
    if (feats_ == nullptr) return;
    const uint32_t w = written_.load(std::memory_order_relaxed);
    const size_t i = w % FEATURE_HISTORY_FRAMES;
    std::memcpy(feats_ + i * FEATURE_CHANNELS, frame, FEATURE_CHANNELS);
    prob_[i] = prob;
    vad_[i] = vad;
    written_.store(w + 1, std::memory_order_release);
    total_.fetch_add(1, std::memory_order_relaxed);
  }

  // Copy the n frames ending at absolute frame `end_frame` (inclusive), oldest first, to dst[n*40].
  bool copy_last(uint32_t end_frame, size_t n, int8_t *dst) const {
    if (feats_ == nullptr || n == 0 || n > FEATURE_HISTORY_FRAMES || end_frame + 1 < n) return false;
    const uint32_t start = end_frame + 1 - uint32_t(n);
    if (frames_written() <= end_frame) return false;
    for (size_t k = 0; k < n; k++)
      std::memcpy(dst + k * FEATURE_CHANNELS, feats_ + ((start + k) % FEATURE_HISTORY_FRAMES) * FEATURE_CHANNELS,
                  FEATURE_CHANNELS);
    return frames_written() <= start + FEATURE_HISTORY_FRAMES;  // oldest frame still intact after the copy
  }

  // Upload window: pre frames ending at `fire` (a multiple of `stride`, so the first frame starts a
  // stage-1 invocation) plus up to `want_post` frames after it, whatever is available. dst needs
  // FEATURE_UPLOAD_MAX_FRAMES*40 bytes.
  bool copy_upload(uint32_t fire, uint8_t stride, size_t want_post, int8_t *dst, FeatureWindow *w) const {
    if (feats_ == nullptr || stride == 0 || stride > 8) return false;
    const uint32_t written = frames_written();
    if (written == 0 || written <= fire) return false;
    // The ring holds only the newest 250 frames. The caller freezes the window ~500 ms after the fire frame, so
    // 50+ post frames normally exist already, and 198 pre + 53 post would overwrite the oldest wanted frame.
    // Never fail on timing: take what exists (post <= want_post) and, if the ring has advanced, shorten the PRE part
    // (in whole strides, so the window still starts at an invocation boundary). Frames within MARGIN of the
    // overwrite point are not trusted (the producer may be writing them right now).
    const uint32_t oldest_ok = written > FEATURE_HISTORY_FRAMES - FEATURE_COPY_MARGIN
                                   ? written - uint32_t(FEATURE_HISTORY_FRAMES - FEATURE_COPY_MARGIN)
                                   : 0;
    if (fire + 1 <= oldest_ok) return false;
    size_t pre = (FEATURE_UPLOAD_PRE_FRAMES / stride) * stride;
    const size_t pre_fit = ((size_t(fire) + 1 - oldest_ok) / stride) * stride;  // pre frames still intact
    pre = std::min(pre, pre_fit);
    if (pre < STAGE2_WINDOW_FRAMES) return false;  // window no longer holds the 1 s stage 2 scored
    const size_t avail_post = written - 1 - fire;
    size_t post = std::min(want_post, avail_post);
    post = std::min(post, FEATURE_UPLOAD_MAX_FRAMES - pre);
    const size_t n = pre + post;
    const uint32_t start = fire + 1 - uint32_t(pre);
    for (size_t k = 0; k < n; k++)
      std::memcpy(dst + k * FEATURE_CHANNELS, feats_ + ((start + k) % FEATURE_HISTORY_FRAMES) * FEATURE_CHANNELS,
                  FEATURE_CHANNELS);
    w->frames = uint16_t(n);
    w->pre_frames = uint16_t(pre);
    w->post_frames = uint16_t(post);
    w->fire_idx = uint16_t(pre - 1);
    w->stride = stride;
    w->n_probs = uint8_t(std::min<size_t>(n / stride, FEATURE_UPLOAD_MAX_PROBS));
    w->start_frame_is_invocation_start = true;
    for (size_t j = 0; j < w->n_probs; j++) {
      const size_t i = (start + stride * j + stride - 1) % FEATURE_HISTORY_FRAMES;
      w->probs[j] = prob_[i];
      w->vad[j] = vad_[i];
    }
    return frames_written() <= start + FEATURE_HISTORY_FRAMES;
  }

 private:
  int8_t *feats_{nullptr};
  uint8_t *prob_{nullptr};
  uint8_t *vad_{nullptr};
  std::atomic<uint32_t> written_{0};
  std::atomic<uint32_t> total_{0};
};

}  // namespace esphome::micro_wake_word
