// Host unit tests for the RSSI sample ring (rssi_tracker.h).
// Run: make -C tests

#include "rssi_tracker.h"
#include "test_harness.h"

#include <cmath>
#include <cstdint>

using esphome::ert_scm::RssiTracker;

static void test_raw_to_dbm() {
  CHECK(RssiTracker::to_dbm(0) == 0.0f);
  CHECK(RssiTracker::to_dbm(150) == -75.0f);
  CHECK(RssiTracker::to_dbm(255) == -127.5f);
}

static void test_empty_tracker_has_no_peak_or_floor() {
  RssiTracker t;
  CHECK(std::isnan(t.peak_dbm(1000, 50000)));
  CHECK(std::isnan(t.noise_dbm()));
}

static void test_peak_picks_the_frame_out_of_noise() {
  RssiTracker t;
  uint32_t us = 0;
  for (int i = 0; i < 100; i++, us += 1000)
    t.add(us, 220);  // -110 dBm noise
  for (int i = 0; i < 6; i++, us += 1000)
    t.add(us, 150);  // a 6 ms frame at -75 dBm
  for (int i = 0; i < 3; i++, us += 1000)
    t.add(us, 220);  // remote_receiver's idle gap + loop latency
  CHECK(t.peak_dbm(us, 50000) == -75.0f);
  // The noise floor barely moves for one short frame.
  CHECK(t.noise_dbm() < -105.0f);
}

static void test_frame_level_only_for_single_frame_captures() {
  RssiTracker t;
  uint32_t us = 0;
  for (int i = 0; i < 10; i++, us += 1000)
    t.add(us, 220);  // -110 dBm noise
  t.add(us, 120);    // strong meter, -60 dBm
  us += 1000;
  t.add(us, 180);    // weaker meter right behind it, -90 dBm
  us += 1000;
  CHECK(t.frame_dbm(1, us, 50000) == -60.0f);
  CHECK(t.frame_dbm(1, us, 50000) == t.peak_dbm(us, 50000));
  // Two frames in one capture can't be told apart: report nothing rather than
  // giving the weaker meter the stronger one's level.
  CHECK(std::isnan(t.frame_dbm(2, us, 50000)));
  CHECK(std::isnan(t.frame_dbm(3, us, 50000)));
  CHECK(std::isnan(t.frame_dbm(0, us, 50000)));
  // Still NaN with one frame when there's no sample in the window.
  CHECK(std::isnan(t.frame_dbm(1, us + 500000, 50000)));
}

static void test_capture_window_stops_at_the_previous_capture() {
  RssiTracker t;
  CHECK_EQ(t.capture_window(1000000, 60000), 60000u);  // first capture: full window
  CHECK_EQ(t.capture_window(1020000, 60000), 20000u);  // 20 ms after the last one
  CHECK_EQ(t.capture_window(1200000, 60000), 60000u);  // long gap: capped
  CHECK_EQ(t.capture_window(1200000, 60000), 0u);      // two captures in one loop pass
  // micros() wrap between captures
  RssiTracker w;
  w.capture_window(0xFFFFF000u, 60000);
  CHECK_EQ(w.capture_window(0x00001000u, 60000), 0x2000u);
}

static void test_earlier_strong_transmission_is_not_the_frames_level() {
  RssiTracker t;
  uint32_t us = 0;
  for (int i = 0; i < 6; i++, us += 1000)
    t.add(us, 100);                  // unrelated strong burst, -50 dBm
  us += 1000;
  t.capture_window(us, 60000);       // ...handled as its own (non-SCM) capture
  for (int i = 0; i < 10; i++, us += 1000)
    t.add(us, 220);                  // noise, -110 dBm
  for (int i = 0; i < 6; i++, us += 1000)
    t.add(us, 160);                  // the SCM frame, -80 dBm
  us += 1000;
  uint32_t window = t.capture_window(us, 60000);
  CHECK(window < 60000u);
  CHECK(t.frame_dbm(1, us, window) == -80.0f);
  // Without the bound, the earlier burst would win.
  CHECK(t.peak_dbm(us, 60000) == -50.0f);
}

static void test_peak_ignores_samples_outside_the_window() {
  RssiTracker t;
  t.add(0, 100);          // strong, but long ago
  t.add(200000, 200);     // 200 ms later
  t.add(210000, 210);
  CHECK(t.peak_dbm(220000, 50000) == -100.0f);
  CHECK(std::isnan(t.peak_dbm(400000, 50000)));
}

static void test_samples_closer_than_the_interval_are_skipped() {
  RssiTracker t;
  CHECK(t.add(1000, 200));
  CHECK(!t.add(1100, 100));  // 100 us later: dropped
  CHECK(t.add(1000 + RssiTracker::MIN_INTERVAL_US, 190));
  CHECK_EQ(t.size(), 2u);
  CHECK(t.peak_dbm(2000, 50000) == -95.0f);
}

static void test_ring_wraps_and_keeps_the_newest() {
  RssiTracker t;
  uint32_t us = 0;
  for (size_t i = 0; i < RssiTracker::CAPACITY * 3; i++, us += 1000)
    t.add(us, static_cast<uint8_t>(100 + (i % 50)));
  CHECK_EQ(t.size(), RssiTracker::CAPACITY);
  // The last 10 samples are raw 100 + (i % 50) for the final i values.
  const float peak = t.peak_dbm(us, 10000);
  CHECK(!std::isnan(peak));
  CHECK(peak <= -50.0f && peak >= -75.0f);
}

static void test_microsecond_clock_wrap() {
  RssiTracker t;
  const uint32_t start = 0xFFFFFFFFu - 5000;  // ~71 min after boot, micros() wraps
  t.add(start, 220);
  t.add(start + 2000, 140);
  t.add(start + 4000, 220);
  t.add(start + 6000, 220);  // wrapped past zero
  t.add(start + 8000, 220);
  CHECK(t.peak_dbm(start + 9000, 50000) == -70.0f);
}

static void test_noise_floor_warms_up_then_tracks() {
  RssiTracker t;
  uint32_t us = 0;
  for (uint32_t i = 0; i + 1 < RssiTracker::NOISE_WARMUP; i++, us += 1000)
    t.add(us, 210);
  CHECK(std::isnan(t.noise_dbm()));
  t.add(us, 210);
  us += 1000;
  CHECK(std::fabs(t.noise_dbm() - -105.0f) < 0.01f);
  // A lasting change in the floor is followed, slowly.
  for (int i = 0; i < 3000; i++, us += 1000)
    t.add(us, 190);
  CHECK(std::fabs(t.noise_dbm() - -95.0f) < 0.5f);
}

int main() {
  static const TestCase tests[] = {
      {"raw to dBm", test_raw_to_dbm},
      {"empty tracker has no peak or floor", test_empty_tracker_has_no_peak_or_floor},
      {"peak picks the frame out of noise", test_peak_picks_the_frame_out_of_noise},
      {"peak ignores samples outside the window", test_peak_ignores_samples_outside_the_window},
      {"frame level only for single-frame captures", test_frame_level_only_for_single_frame_captures},
      {"capture window stops at the previous capture", test_capture_window_stops_at_the_previous_capture},
      {"earlier strong transmission is not the frame's level", test_earlier_strong_transmission_is_not_the_frames_level},
      {"samples closer than the interval are skipped", test_samples_closer_than_the_interval_are_skipped},
      {"ring wraps and keeps the newest", test_ring_wraps_and_keeps_the_newest},
      {"microsecond clock wrap", test_microsecond_clock_wrap},
      {"noise floor warms up then tracks", test_noise_floor_warms_up_then_tracks},
  };
  return run_tests(tests);
}
