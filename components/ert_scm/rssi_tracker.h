#pragma once

// Recent SX1276 RSSI samples, so a decoded frame can be given the signal
// strength it arrived with.
//
// Plain C++ with no ESPHome dependencies, so it builds both in the firmware
// and in the host unit tests under tests/.
//
// In continuous OOK mode the radio only offers a live RSSI register, and a
// frame (~6 ms) is over by the time remote_receiver hands it to the decoder.
// So the component samples the register on every main-loop pass and, when a
// frame decodes, takes the strongest sample from just before it. The running
// average of all samples tracks the noise floor, since frames are rare.
//
// Samples are the raw FSK/OOK RegRssiValue (0x11): dBm = -raw / 2, so a
// smaller raw value is a stronger signal.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace ert_scm {

class RssiTracker {
 public:
  static constexpr size_t CAPACITY = 256;
  /// Minimum spacing between stored samples, so the ring spans a useful time.
  static constexpr uint32_t MIN_INTERVAL_US = 250;
  /// Samples averaged before the noise floor is reported.
  static constexpr uint32_t NOISE_WARMUP = 64;

  static float to_dbm(uint8_t raw) { return -static_cast<float>(raw) / 2.0f; }

  /// Offer a sample taken at `now_us` (a wrapping microsecond clock). Returns
  /// false if it was skipped because it came too soon after the last one.
  bool add(uint32_t now_us, uint8_t raw) {
    if (this->count_ > 0 && now_us - this->last_us_ < MIN_INTERVAL_US)
      return false;
    this->last_us_ = now_us;
    this->us_[this->head_] = now_us;
    this->raw_[this->head_] = raw;
    this->head_ = (this->head_ + 1) % CAPACITY;
    if (this->count_ < CAPACITY)
      this->count_++;
    // Exponential average with a slow (1/256) time constant: a 6 ms frame
    // among minutes of noise barely moves it.
    const float dbm = to_dbm(raw);
    if (this->noise_samples_ == 0)
      this->noise_dbm_ = dbm;
    else
      this->noise_dbm_ += (dbm - this->noise_dbm_) / 256.0f;
    if (this->noise_samples_ < NOISE_WARMUP)
      this->noise_samples_++;
    return true;
  }

  /// Strongest sample from the last `window_us` before `now_us`, in dBm.
  /// NaN if there is no sample in that window.
  float peak_dbm(uint32_t now_us, uint32_t window_us) const {
    bool found = false;
    uint8_t best = 0;
    for (size_t i = 0; i < this->count_; i++) {
      const size_t idx = (this->head_ + CAPACITY - 1 - i) % CAPACITY;
      if (now_us - this->us_[idx] > window_us)
        break;  // newest first, so everything after this is older
      if (!found || this->raw_[idx] < best) {
        best = this->raw_[idx];
        found = true;
      }
    }
    return found ? to_dbm(best) : NAN;
  }

  /// Call once for every remote_receiver capture, decoded or not, when it is
  /// handled at `now_us`. Returns how far back that capture's samples can go:
  /// `max_window_us`, shortened so it never reaches past the previous capture.
  /// Samples from before then belong to that capture, e.g. a stronger unrelated
  /// transmission that ended just before an SCM frame.
  uint32_t capture_window(uint32_t now_us, uint32_t max_window_us) {
    uint32_t window = max_window_us;
    if (this->have_capture_ && now_us - this->last_capture_us_ < window)
      window = now_us - this->last_capture_us_;
    this->last_capture_us_ = now_us;
    this->have_capture_ = true;
    return window;
  }

  /// The signal level to report for each frame of a capture that decoded
  /// `frames_in_capture` frames: peak_dbm() when it holds exactly one frame,
  /// otherwise NaN. Samples are taken on main-loop passes, so they can't be
  /// lined up with a frame's position inside a capture. A capture holding
  /// several frames (e.g. two meters colliding) would otherwise give every
  /// frame the strongest one's level.
  float frame_dbm(size_t frames_in_capture, uint32_t now_us, uint32_t window_us) const {
    return frames_in_capture == 1 ? this->peak_dbm(now_us, window_us) : NAN;
  }

  /// Running average of all samples, in dBm; NaN until warmed up.
  float noise_dbm() const { return this->noise_samples_ >= NOISE_WARMUP ? this->noise_dbm_ : NAN; }

  size_t size() const { return this->count_; }

 protected:
  uint32_t us_[CAPACITY]{};
  uint8_t raw_[CAPACITY]{};
  size_t head_{0};
  size_t count_{0};
  uint32_t last_us_{0};
  float noise_dbm_{0};
  uint32_t noise_samples_{0};
  uint32_t last_capture_us_{0};
  bool have_capture_{false};
};

}  // namespace ert_scm
}  // namespace esphome
