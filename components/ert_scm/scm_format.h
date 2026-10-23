#pragma once

// MQTT payload / topic formatting for ert_scm. Plain C++ (no ESPHome
// dependencies) so the host unit tests under tests/ can cover it.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
namespace ert_scm {

/// raw / scale with exactly 2 decimals, e.g. (654321, 100) -> "6543.21".
///
/// Integer math, rounding half up. For scale 1, 10 and 100 the result is exact
/// and matches the previous collector's Python `f"{raw / scale:.2f}"`. That was
/// checked on 200k random readings. Scales like 1000 can differ from Python on
/// exact half-cent ties, which Python sometimes rounds down via binary floats.
/// scale 0 is treated as 1.
inline std::string format_scaled(uint32_t raw, uint32_t scale) {
  if (scale == 0)
    scale = 1;
  uint64_t cents = (static_cast<uint64_t>(raw) * 100 + scale / 2) / scale;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%" PRIu64 ".%02u", cents / 100, static_cast<unsigned>(cents % 100));
  return buf;
}

/// "{prefix}/{id}/{leaf}", tolerating a trailing '/' on the prefix.
inline std::string meter_topic(const std::string &prefix, uint32_t id, const char *leaf) {
  std::string topic = prefix;
  while (!topic.empty() && topic.back() == '/')
    topic.pop_back();
  char buf[48];
  std::snprintf(buf, sizeof buf, "/%" PRIu32 "/%s", id, leaf);
  return topic + buf;
}

}  // namespace ert_scm
}  // namespace esphome
