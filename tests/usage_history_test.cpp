// Host unit tests for the hourly usage history (usage_history.h).
// Run: make -C tests

#include "usage_history.h"
#include "test_harness.h"

#include <cstdint>
#include <cstring>
#include <random>

using esphome::ert_scm::USAGE_HOURS;
using esphome::ert_scm::UsageHistory;

// UTC epoch minute for day `d` (counted from an arbitrary, realistic epoch
// day), hour `h`, minute `m`. Most tests treat UTC midnight as local midnight;
// the DST tests below pass a shifted one.
static uint32_t at(uint32_t d, uint32_t h, uint32_t m) { return ((20000 + d) * 24 + h) * 60 + m; }
static uint32_t midnight(uint32_t d) { return at(d, 0, 0); }

static uint32_t sum(const uint32_t (&v)[USAGE_HOURS]) {
  uint32_t t = 0;
  for (uint32_t x : v)
    t += x;
  return t;
}

static void test_first_reading_is_only_a_baseline() {
  UsageHistory u;
  CHECK(!u.has_baseline());
  u.add(1000000, at(0, 10, 5));
  CHECK(u.has_baseline());
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 10, 5), s);
  CHECK_EQ(sum(s), 0u);
  CHECK_EQ(u.today(at(0, 10, 5), midnight(0)), 0u);
}

static void test_readings_in_one_hour_accumulate() {
  UsageHistory u;
  u.add(1000, at(0, 10, 5));
  u.add(1010, at(0, 10, 17));
  u.add(1025, at(0, 10, 30));
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 10, 30), s);
  CHECK_EQ(s[USAGE_HOURS - 1], 25u);
  CHECK_EQ(sum(s), 25u);
  CHECK_EQ(u.today(at(0, 10, 30), midnight(0)), 25u);
}

static void test_reading_across_an_hour_splits_by_minutes() {
  UsageHistory u;
  u.add(1000, at(0, 10, 50));
  u.add(1020, at(0, 11, 10));  // 10 min in hour 10, 10 min in hour 11
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 11, 10), s);
  CHECK_EQ(s[USAGE_HOURS - 2], 10u);
  CHECK_EQ(s[USAGE_HOURS - 1], 10u);
}

static void test_missed_frames_spread_over_the_gap() {
  UsageHistory u;
  u.add(1000, at(0, 10, 0));
  u.add(1300, at(0, 13, 0));  // three silent hours
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 13, 0), s);
  CHECK_EQ(s[USAGE_HOURS - 4], 100u);  // hour 10
  CHECK_EQ(s[USAGE_HOURS - 3], 100u);  // hour 11
  CHECK_EQ(s[USAGE_HOURS - 2], 100u);  // hour 12
  CHECK_EQ(s[USAGE_HOURS - 1], 0u);    // hour 13 has had no time yet
  CHECK_EQ(sum(s), 300u);
}

static void test_old_hours_roll_out_of_the_window() {
  UsageHistory u;
  u.add(1000, at(0, 10, 0));
  u.add(1060, at(0, 10, 30));  // 60 in day 0 hour 10
  u.add(1060, at(0, 11, 0));
  uint32_t s[USAGE_HOURS];
  // 23 hours later day 0 hour 10 is still the oldest bar...
  u.snapshot(at(1, 9, 59), s);
  CHECK_EQ(s[0], 60u);
  // ...and one hour after that it has rolled off, with no new readings.
  u.snapshot(at(1, 10, 0), s);
  CHECK_EQ(sum(s), 0u);
  // A new reading in that hour slot must not resurrect the old value.
  u.add(1070, at(1, 10, 30));
  u.snapshot(at(1, 10, 30), s);
  CHECK(s[USAGE_HOURS - 1] < 60u);
}

static void test_gap_longer_than_the_window_keeps_only_its_tail() {
  UsageHistory u;
  u.add(1000, at(0, 0, 0));
  u.add(1000 + 48 * 10, at(2, 0, 0));  // 48 h at 10/h, device was off
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(2, 0, 0), s);
  // Hours 1..23 of day 1 are in the window (hour 0 of day 2 has no time yet).
  CHECK_EQ(sum(s), 23u * 10u);
  for (size_t i = 0; i + 1 < USAGE_HOURS; i++)
    CHECK_EQ(s[i], 10u);
}

static void test_counter_going_backwards_rebaselines() {
  UsageHistory u;
  u.add(1000, at(0, 10, 0));
  u.add(1050, at(0, 10, 20));
  u.add(10, at(0, 10, 30));  // new meter
  u.add(20, at(0, 10, 40));
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 10, 40), s);
  CHECK_EQ(s[USAGE_HOURS - 1], 60u);
}

static void test_clock_stepping_back_does_not_lose_usage() {
  UsageHistory u;
  u.add(1000, at(0, 10, 30));
  u.add(1010, at(0, 10, 20));  // SNTP correction
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 10, 30), s);
  CHECK_EQ(s[USAGE_HOURS - 1], 10u);
  CHECK_EQ(u.state().last_minute, at(0, 10, 30));
}

static void test_today_starts_at_local_midnight() {
  UsageHistory u;
  u.add(1000, at(0, 22, 0));
  u.add(1120, at(1, 2, 0));  // 30 per hour over 22, 23, 0, 1
  CHECK_EQ(u.today(at(1, 2, 0), midnight(1)), 60u);
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(1, 2, 0), s);
  CHECK_EQ(sum(s), 120u);
  // Asked about 23:59 the day before, "today" is that day's hours 22-23,
  // which the later reading has since filled in.
  CHECK_EQ(u.today(at(0, 23, 59), midnight(0)), 60u);
}

static void test_rounding_leftover_stays_in_hours_that_overlap() {
  // A reading exactly on the hour: the new hour has no time in the interval,
  // so not even a rounding leftover may land there (Codex review, PR 10).
  UsageHistory u;
  u.add(1000, at(0, 22, 50));
  u.add(1001, at(1, 0, 0));  // 1 count over 70 min: 10 in hour 22, 60 in hour 23
  CHECK_EQ(u.today(at(1, 0, 0), midnight(1)), 0u);
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(1, 0, 0), s);
  CHECK_EQ(s[USAGE_HOURS - 1], 0u);  // hour 00
  CHECK_EQ(s[USAGE_HOURS - 2], 1u);  // hour 23, the newest hour with overlap
  CHECK_EQ(sum(s), 1u);
}

static void test_readings_across_a_dst_change_are_weighted_by_real_time() {
  // Codex review, PR 10: 01:55 CST and 03:05 CDT on spring-forward night are
  // only 10 real minutes apart (07:55 and 08:05 UTC). In UTC minutes the usage
  // splits 5/5 across two real hours, with no bar for the skipped 02:00.
  UsageHistory u;
  u.add(1000, at(0, 7, 55));
  u.add(1070, at(0, 8, 5));
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 8, 5), s);
  CHECK_EQ(s[USAGE_HOURS - 2], 35u);  // 07:00 UTC
  CHECK_EQ(s[USAGE_HOURS - 1], 35u);  // 08:00 UTC
  CHECK_EQ(sum(s), 70u);
}

static void test_today_on_a_23_hour_local_day() {
  // America/Chicago: midnight before spring-forward is 06:00 UTC, the next
  // one is 05:00 UTC, so that local day is 23 real hours long.
  UsageHistory u;
  u.add(1000, at(0, 6, 0));  // local midnight, CST
  u.add(1230, at(1, 5, 0));  // next local midnight, CDT: 23 h at 10/h
  CHECK_EQ(u.today(at(1, 5, 0), at(0, 6, 0)), 230u);  // the whole short day
  u.add(1240, at(1, 5, 30));
  CHECK_EQ(u.today(at(1, 5, 30), at(1, 5, 0)), 10u);  // the new day so far
}

static void test_snapshot_does_not_mutate() {
  UsageHistory u;
  u.add(1000, at(0, 10, 0));
  u.add(1040, at(0, 10, 45));
  const UsageHistory::State before = u.state();
  uint32_t s[USAGE_HOURS];
  u.snapshot(at(0, 15, 0), s);  // five empty hours later
  CHECK_EQ(s[USAGE_HOURS - 6], 40u);
  CHECK_EQ(sum(s), 40u);
  CHECK(std::memcmp(&before, &u.state(), sizeof before) == 0);
  u.snapshot(at(3, 0, 0), s);  // days later
  CHECK_EQ(sum(s), 0u);
  CHECK_EQ(u.today(at(3, 0, 0), midnight(3)), 0u);
}

static void test_state_round_trip_and_bad_versions() {
  UsageHistory a;
  a.add(1000, at(0, 8, 0));
  a.add(1100, at(0, 9, 30));
  UsageHistory b;
  CHECK(b.load(a.state()));
  uint32_t sa[USAGE_HOURS], sb[USAGE_HOURS];
  a.snapshot(at(0, 9, 30), sa);
  b.snapshot(at(0, 9, 30), sb);
  CHECK(std::memcmp(sa, sb, sizeof sa) == 0);
  // Carries on accumulating where it left off.
  b.add(1110, at(0, 9, 45));
  CHECK_EQ(b.today(at(0, 9, 45), midnight(0)), 110u);

  UsageHistory::State bad = a.state();
  bad.version = 1;  // the earlier local-hour format
  CHECK(!b.load(bad));
  CHECK(!b.has_baseline());
  bad = a.state();
  bad.has_baseline = 7;  // garbage from an unrelated preference
  CHECK(!b.load(bad));
  CHECK(!b.has_baseline());
}

static void test_random_readings_conserve_usage() {
  std::mt19937 rng(42);
  for (int run = 0; run < 200; run++) {
    UsageHistory u;
    uint32_t raw = static_cast<uint32_t>(rng() % 1000000);
    uint32_t minute = at(0, 0, 0) + static_cast<uint32_t>(rng() % 60);
    u.add(raw, minute);
    const uint32_t start = raw;
    // Readings every 1-40 min, all within 23 h so nothing leaves the window.
    while (minute < at(0, 23, 0)) {
      minute += 1 + static_cast<uint32_t>(rng() % 40);
      raw += static_cast<uint32_t>(rng() % 50);
      u.add(raw, minute);
    }
    uint32_t s[USAGE_HOURS];
    u.snapshot(minute, s);
    if (sum(s) != raw - start) {
      CHECK_EQ(sum(s), raw - start);
      return;
    }
  }
  CHECK(true);
}

int main() {
  static const TestCase tests[] = {
      {"first reading is only a baseline", test_first_reading_is_only_a_baseline},
      {"readings in one hour accumulate", test_readings_in_one_hour_accumulate},
      {"reading across an hour splits by minutes", test_reading_across_an_hour_splits_by_minutes},
      {"missed frames spread over the gap", test_missed_frames_spread_over_the_gap},
      {"old hours roll out of the window", test_old_hours_roll_out_of_the_window},
      {"gap longer than the window keeps only its tail", test_gap_longer_than_the_window_keeps_only_its_tail},
      {"counter going backwards rebaselines", test_counter_going_backwards_rebaselines},
      {"clock stepping back does not lose usage", test_clock_stepping_back_does_not_lose_usage},
      {"today starts at local midnight", test_today_starts_at_local_midnight},
      {"rounding leftover stays in hours that overlap", test_rounding_leftover_stays_in_hours_that_overlap},
      {"readings across a DST change are weighted by real time",
       test_readings_across_a_dst_change_are_weighted_by_real_time},
      {"today on a 23-hour local day", test_today_on_a_23_hour_local_day},
      {"snapshot does not mutate", test_snapshot_does_not_mutate},
      {"state round trip and bad versions", test_state_round_trip_and_bad_versions},
      {"random readings conserve usage", test_random_readings_conserve_usage},
  };
  return run_tests(tests);
}
