// Host unit tests for MQTT payload / topic formatting (scm_format.h).
// Run: make -C tests

#include "scm_format.h"
#include "test_harness.h"

#include <cstdint>

using esphome::ert_scm::format_scaled;
using esphome::ert_scm::meter_topic;

static void test_scale_100_two_decimals() {
  // Same output as the previous collector's Python f"{raw / 100:.2f}".
  CHECK_STR(format_scaled(654321, 100), "6543.21");
  CHECK_STR(format_scaled(654300, 100), "6543.00");
  CHECK_STR(format_scaled(654305, 100), "6543.05");
  CHECK_STR(format_scaled(5, 100), "0.05");
  CHECK_STR(format_scaled(0, 100), "0.00");
  CHECK_STR(format_scaled(99, 100), "0.99");
  CHECK_STR(format_scaled(100, 100), "1.00");
  CHECK_STR(format_scaled(0xFFFFFF, 100), "167772.15");  // largest 24-bit SCM reading
}

static void test_other_scales() {
  CHECK_STR(format_scaled(654321, 1), "654321.00");
  CHECK_STR(format_scaled(654321, 10), "65432.10");
  CHECK_STR(format_scaled(654321, 1000), "654.32");
  CHECK_STR(format_scaled(654325, 1000), "654.33");  // rounds half up
  CHECK_STR(format_scaled(1, 3), "0.33");
  CHECK_STR(format_scaled(2, 3), "0.67");
  CHECK_STR(format_scaled(0xFFFFFFFFu, 1), "4294967295.00");  // no 32-bit overflow
  CHECK_STR(format_scaled(42, 0), "42.00");                   // scale 0 treated as 1
}

static void test_scale_100_is_exact_for_every_cent() {
  // Integer math: raw/100 must reproduce the digits exactly over a wide range.
  for (uint32_t raw = 0; raw < 2000000; raw += 7) {
    std::string s = format_scaled(raw, 100);
    char want[32];
    std::snprintf(want, sizeof want, "%u.%02u", raw / 100, raw % 100);
    if (s != want) {
      CHECK_STR(s, want);
      return;
    }
  }
  CHECK(true);
}

static void test_meter_topic() {
  CHECK_STR(meter_topic("meters/gas", 12345678, "consumption"), "meters/gas/12345678/consumption");
  CHECK_STR(meter_topic("meters/gas-lilygo", 12345678, "raw"), "meters/gas-lilygo/12345678/raw");
  CHECK_STR(meter_topic("meters/gas/", 7, "last_seen"), "meters/gas/7/last_seen");
  CHECK_STR(meter_topic("meters/gas//", 7, "raw"), "meters/gas/7/raw");
  CHECK_STR(meter_topic("", 67108863, "raw"), "/67108863/raw");  // max 26-bit id
}

int main() {
  static const TestCase tests[] = {
      {"scale 100 gives two exact decimals", test_scale_100_two_decimals},
      {"other scales, rounding, overflow, scale 0", test_other_scales},
      {"scale 100 exact across 0..2,000,000", test_scale_100_is_exact_for_every_cent},
      {"meter topic building", test_meter_topic},
  };
  return run_tests(tests);
}
