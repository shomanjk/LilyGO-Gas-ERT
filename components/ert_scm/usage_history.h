#pragma once

// Rolling 24 h of hourly usage for one meter, built from its cumulative
// readings.
//
// Plain C++ with no ESPHome dependencies, so it builds both in the firmware
// and in the host unit tests under tests/.
//
// Time is passed in as a UTC "epoch minute" (Unix time / 60), which never
// jumps, so the hours are real elapsed hours even across daylight saving
// changes. Local time only matters for "today": the caller passes the epoch
// minute of local midnight. Values are raw meter counts; the caller divides
// by the scale.
//
// The usage between two readings is spread over the hours between them in
// proportion to the minutes in each, so a run of missed frames (or the device
// being off) doesn't pile up as one spike in the hour the next frame lands.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace ert_scm {

static constexpr size_t USAGE_HOURS = 24;

class UsageHistory {
 public:
  /// Everything needed to resume after a reboot. Trivially copyable, so it
  /// can be saved as-is in ESPHome preferences (~112 bytes).
  struct State {
    uint32_t version;
    uint32_t has_baseline;  // 0 or 1; uint32_t keeps the struct free of padding
    uint32_t last_minute;   // UTC epoch minute of the newest reading
    uint32_t last_raw;      // cumulative reading at last_minute
    uint32_t buckets[USAGE_HOURS];  // usage in hour h lives at buckets[h % 24]
  };
  // 2: buckets are UTC hours (1 used local hours; not convertible, so dropped).
  static constexpr uint32_t STATE_VERSION = 2;

  UsageHistory() { this->clear(); }

  void clear() {
    this->state_ = State{};
    this->state_.version = STATE_VERSION;
  }

  /// Restore saved state. Returns false, and clears, if it isn't a version
  /// this code understands.
  bool load(const State &s) {
    if (s.version != STATE_VERSION || s.has_baseline > 1) {
      this->clear();
      return false;
    }
    this->state_ = s;
    return true;
  }

  const State &state() const { return this->state_; }
  bool has_baseline() const { return this->state_.has_baseline != 0; }

  /// Record a cumulative reading taken at UTC epoch minute `minute`.
  void add(uint32_t raw, uint32_t minute) {
    State &s = this->state_;
    if (!s.has_baseline) {
      // First reading: nothing to compare against yet.
      for (auto &b : s.buckets)
        b = 0;
      s.has_baseline = 1;
      s.last_minute = minute;
      s.last_raw = raw;
      return;
    }
    // A clock that steps backwards (SNTP correction) can't un-use gas; treat
    // the reading as belonging to the newest minute we have.
    if (minute < s.last_minute)
      minute = s.last_minute;
    this->advance_(minute / 60);

    if (raw < s.last_raw) {
      // Counter went backwards: meter replaced or wrapped. Start over from
      // here without inventing usage.
      s.last_raw = raw;
      s.last_minute = minute;
      return;
    }
    this->spread_(raw - s.last_raw, s.last_minute, minute);
    s.last_raw = raw;
    s.last_minute = minute;
  }

  /// Usage per hour for the 24 hours ending with the hour containing
  /// `minute`, oldest first. Hours with no data are 0.
  void snapshot(uint32_t minute, uint32_t out[USAGE_HOURS]) const {
    const int64_t now_hour = minute / 60;
    for (size_t i = 0; i < USAGE_HOURS; i++)
      out[i] = this->value_(now_hour - static_cast<int64_t>(USAGE_HOURS - 1) + static_cast<int64_t>(i));
  }

  /// Usage from `midnight` (UTC epoch minute of local midnight) through
  /// `minute`. Hourly resolution: the hour containing midnight counts in
  /// full, which is exact for zones whose offset is a whole number of hours.
  uint32_t today(uint32_t minute, uint32_t midnight) const {
    const int64_t now_hour = minute / 60;
    uint32_t total = 0;
    for (int64_t h = midnight / 60; h <= now_hour; h++)
      total += this->value_(h);
    return total;
  }

 protected:
  /// Bucket for absolute hour `h`, or 0 if it's outside the 24 hours that
  /// end at the newest reading.
  uint32_t value_(int64_t h) const {
    const State &s = this->state_;
    if (!s.has_baseline || h < 0)
      return 0;
    const int64_t last_hour = s.last_minute / 60;
    if (h > last_hour || h <= last_hour - static_cast<int64_t>(USAGE_HOURS))
      return 0;
    return s.buckets[static_cast<size_t>(h % USAGE_HOURS)];
  }

  /// Move the window forward to end at `hour`, zeroing the hours that fell
  /// out of it.
  void advance_(uint32_t hour) {
    State &s = this->state_;
    const uint32_t last_hour = s.last_minute / 60;
    if (hour <= last_hour)
      return;
    if (hour - last_hour >= USAGE_HOURS) {
      for (auto &b : s.buckets)
        b = 0;
    } else {
      for (uint32_t h = last_hour + 1; h <= hour; h++)
        s.buckets[h % USAGE_HOURS] = 0;
    }
    // The caller always moves last_minute into `hour` right after this.
  }

  /// Add `delta` counts used between minutes `from` and `to` (from <= to),
  /// split across hours by the minutes of overlap. Hours that are already
  /// outside the window ending at `to` are dropped.
  void spread_(uint32_t delta, uint32_t from, uint32_t to) {
    if (delta == 0)
      return;
    State &s = this->state_;
    const uint32_t to_hour = to / 60;
    if (to == from) {
      s.buckets[to_hour % USAGE_HOURS] += delta;
      return;
    }
    const uint64_t span = to - from;
    const uint32_t first_hour = from / 60;
    const uint32_t window_start = to_hour + 1 >= USAGE_HOURS ? to_hour + 1 - USAGE_HOURS : 0;
    const uint32_t start_hour = first_hour > window_start ? first_hour : window_start;

    // Integer shares; rounding leftovers go to the newest hour that actually
    // overlaps the interval. A reading exactly on the hour gives the new hour
    // no time at all, so it must not get any usage either.
    uint64_t in_window_minutes = 0;
    uint64_t allocated = 0;
    bool any_overlap = false;
    uint32_t newest_overlap = to_hour;
    for (uint32_t h = start_hour; h <= to_hour; h++) {
      const uint64_t lo = static_cast<uint64_t>(h) * 60 > from ? static_cast<uint64_t>(h) * 60 : from;
      const uint64_t hi = static_cast<uint64_t>(h + 1) * 60 < to ? static_cast<uint64_t>(h + 1) * 60 : to;
      if (hi <= lo)
        continue;
      const uint64_t share = static_cast<uint64_t>(delta) * (hi - lo) / span;
      s.buckets[h % USAGE_HOURS] += static_cast<uint32_t>(share);
      allocated += share;
      in_window_minutes += hi - lo;
      any_overlap = true;
      newest_overlap = h;
    }
    const uint64_t expected = static_cast<uint64_t>(delta) * in_window_minutes / span;
    if (any_overlap && expected > allocated)
      s.buckets[newest_overlap % USAGE_HOURS] += static_cast<uint32_t>(expected - allocated);
  }

  State state_;
};

}  // namespace ert_scm
}  // namespace esphome
