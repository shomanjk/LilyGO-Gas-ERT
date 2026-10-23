// Host unit tests for the ERT-SCM decoder (no ESPHome needed).
// Run: make -C tests
//
// Frames are built here by an encoder written independently of the decoder,
// and the checksum is cross-checked with a separate GF(2) long division, so
// the CRC test is not just the decoder's crc16() agreeing with itself.
// Meter ids in these tests are synthetic placeholders.

#include "scm_decoder.h"
#include "test_harness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using esphome::ert_scm::SCM_CHIP_US;
using esphome::ert_scm::SCM_FRAME_BYTES;
using esphome::ert_scm::ScmDecoder;
using esphome::ert_scm::ScmPacket;

// ---------------------------------------------------------------------------
// Independent encoder

struct Fields {
  uint32_t id;
  uint8_t ert_type;
  uint8_t physical_tamper;
  uint8_t encoder_tamper;
  uint32_t consumption;
};

// Remainder of (message * x^16) mod (x^16 + poly), by bitwise long division.
static uint16_t gf2_crc(const std::vector<uint8_t> &bits, uint16_t poly) {
  std::vector<uint8_t> work(bits);
  work.resize(bits.size() + 16, 0);
  const uint32_t full = 0x10000u | poly;  // 17-bit divisor
  for (size_t i = 0; i < bits.size(); i++) {
    if (!work[i])
      continue;
    for (int k = 0; k <= 16; k++)
      work[i + k] ^= (full >> (16 - k)) & 1;
  }
  uint16_t rem = 0;
  for (size_t i = bits.size(); i < work.size(); i++)
    rem = static_cast<uint16_t>((rem << 1) | work[i]);
  return rem;
}

static std::vector<uint8_t> bytes_to_bits(const uint8_t *data, size_t len) {
  std::vector<uint8_t> bits;
  for (size_t i = 0; i < len; i++)
    for (int b = 7; b >= 0; b--)
      bits.push_back((data[i] >> b) & 1);
  return bits;
}

static std::vector<uint8_t> build_frame(const Fields &f) {
  std::vector<uint8_t> b(SCM_FRAME_BYTES, 0);
  // 21-bit preamble 0x1F2A60, left-aligned in the first 3 bytes.
  b[0] = 0xF9;
  b[1] = 0x53;
  b[2] = static_cast<uint8_t>(((f.id >> 24) & 0x03) << 1);  // + reserved bit 0
  b[3] = static_cast<uint8_t>(((f.physical_tamper & 0x03) << 6) | ((f.ert_type & 0x0F) << 2) |
                              (f.encoder_tamper & 0x03));
  b[4] = (f.consumption >> 16) & 0xFF;
  b[5] = (f.consumption >> 8) & 0xFF;
  b[6] = f.consumption & 0xFF;
  b[7] = (f.id >> 16) & 0xFF;
  b[8] = (f.id >> 8) & 0xFF;
  b[9] = f.id & 0xFF;
  uint16_t crc = gf2_crc(bytes_to_bits(&b[2], 8), 0x6F63);
  b[10] = crc >> 8;
  b[11] = crc & 0xFF;
  return b;
}

// Manchester: bit 1 -> chips 1,0 ; bit 0 -> chips 0,1.
static std::vector<uint8_t> manchester(const std::vector<uint8_t> &frame, bool invert = false) {
  std::vector<uint8_t> chips;
  for (uint8_t bit : bytes_to_bits(frame.data(), frame.size())) {
    uint8_t first = bit ^ (invert ? 1 : 0);
    chips.push_back(first);
    chips.push_back(first ^ 1);
  }
  return chips;
}

struct PulseOpts {
  float chip_us = SCM_CHIP_US;
  float jitter_us = 0;  // uniform ±jitter per pulse
  // 0 = like a real capture: it ends on the last mark, so the idle gap and a
  // frame's final "off" chip never appear. Non-zero keeps a trailing space.
  int32_t trailing_idle_us = 0;
};

// Chips -> remote_receiver-style timings. Like the real capture, it starts at
// the first mark (leading spaces are dropped).
static void append_pulses(std::vector<int32_t> &out, const std::vector<uint8_t> &chips, const PulseOpts &o,
                          std::mt19937 &rng) {
  std::uniform_real_distribution<float> jit(-o.jitter_us, o.jitter_us);
  size_t i = 0;
  if (out.empty())
    while (i < chips.size() && chips[i] == 0)
      i++;
  while (i < chips.size()) {
    size_t j = i;
    while (j < chips.size() && chips[j] == chips[i])
      j++;
    float us = static_cast<float>(j - i) * o.chip_us + (o.jitter_us > 0 ? jit(rng) : 0.0f);
    int32_t d = static_cast<int32_t>(std::lround(us));
    if (d < 1)
      d = 1;
    out.push_back(chips[i] ? d : -d);
    i = j;
  }
}

static std::vector<int32_t> to_capture(const std::vector<uint8_t> &chips, const PulseOpts &o, std::mt19937 &rng) {
  std::vector<int32_t> t;
  append_pulses(t, chips, o, rng);
  if (o.trailing_idle_us == 0) {
    if (!t.empty() && t.back() < 0)
      t.pop_back();
  } else if (!t.empty() && t.back() < 0) {
    t.back() -= o.trailing_idle_us;
  } else {
    t.push_back(-o.trailing_idle_us);
  }
  return t;
}

static bool same_fields(const ScmPacket &p, const Fields &f) {
  return p.id == f.id && p.ert_type == f.ert_type && p.physical_tamper == f.physical_tamper &&
         p.encoder_tamper == f.encoder_tamper && p.consumption == f.consumption;
}

// ---------------------------------------------------------------------------
// Tests

static const Fields kGas{12345678, 12, 0, 0, 654321};

static void test_crc_matches_independent_division() {
  std::mt19937 rng(1);
  for (int n = 0; n < 500; n++) {
    uint8_t msg[8];
    for (auto &x : msg)
      x = static_cast<uint8_t>(rng());
    CHECK_EQ(ScmDecoder::crc16(msg, sizeof msg), gf2_crc(bytes_to_bits(msg, sizeof msg), 0x6F63));
  }
  auto frame = build_frame(kGas);
  CHECK_EQ(ScmDecoder::crc16(&frame[2], 10), 0);
}

static void test_parse_frame_fields() {
  const Fields cases[] = {
      kGas,
      {0, 0, 0, 0, 0},
      {(1u << 26) - 1, 15, 3, 3, 0xFFFFFF},  // every field at its maximum
      {1u << 24, 2, 1, 2, 1},                 // only an ID MSB bit set
      {1u << 25, 9, 2, 1, 0x800000},
  };
  for (const auto &f : cases) {
    auto frame = build_frame(f);
    ScmPacket p;
    CHECK(ScmDecoder::parse_frame(frame.data(), p));
    CHECK(same_fields(p, f));
    CHECK_EQ(p.checksum, (frame[10] << 8) | frame[11]);
  }
}

static void test_parse_frame_rejects_bad_crc() {
  auto frame = build_frame(kGas);
  for (size_t bit = 21; bit < 96; bit++) {  // every non-preamble bit
    auto bad = frame;
    bad[bit / 8] ^= static_cast<uint8_t>(0x80 >> (bit % 8));
    ScmPacket p;
    CHECK(!ScmDecoder::parse_frame(bad.data(), p));
  }
}

static void test_decode_clean_capture() {
  std::mt19937 rng(2);
  auto t = to_capture(manchester(build_frame(kGas)), {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 1u);
  CHECK_EQ(out.size(), 1u);
  if (!out.empty()) {
    CHECK(same_fields(out[0], kGas));
    CHECK(!out[0].inverted);
  }
  CHECK_EQ(dec.stats().frames, 1u);
  CHECK_EQ(dec.stats().crc_errors, 0u);
}

static void test_decode_either_final_bit_with_or_without_trailing_space() {
  // A frame ending in a 1 bit ends "on, off"; remote_receiver drops that last
  // off chip along with the idle gap. Cover both final bits, both endings.
  for (uint8_t last_bit : {0, 1}) {
    Fields f = kGas;
    while ((build_frame(f)[11] & 1) != last_bit)
      f.consumption++;
    for (int32_t idle : {0, 250}) {
      std::mt19937 rng(12);
      PulseOpts o;
      o.trailing_idle_us = idle;
      auto t = to_capture(manchester(build_frame(f)), o, rng);
      ScmDecoder dec;
      std::vector<ScmPacket> out;
      CHECK_EQ(dec.decode(t, out), 1u);
      if (!out.empty())
        CHECK(same_fields(out[0], f));
    }
  }
}

static void test_decode_inverted_polarity() {
  // Opposite Manchester convention / inverted DIO2: the capture now starts
  // with an "off" chip that remote_receiver never sees.
  std::mt19937 rng(3);
  auto t = to_capture(manchester(build_frame(kGas), /*invert=*/true), {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 1u);
  if (!out.empty()) {
    CHECK(same_fields(out[0], kGas));
    CHECK(out[0].inverted);
  }
}

static void test_decode_with_jitter_and_clock_error() {
  std::mt19937 rng(4);
  int ok = 0, runs = 0;
  for (float clock : {0.97f, 1.0f, 1.03f}) {
    for (int n = 0; n < 200; n++) {
      PulseOpts o;
      o.chip_us = SCM_CHIP_US * clock;
      o.jitter_us = 8.0f;
      Fields f{static_cast<uint32_t>(rng()) & 0x3FFFFFF, 12, 0, 0, static_cast<uint32_t>(rng()) & 0xFFFFFF};
      auto t = to_capture(manchester(build_frame(f)), o, rng);
      ScmDecoder dec;
      std::vector<ScmPacket> out;
      runs++;
      if (dec.decode(t, out) == 1 && same_fields(out[0], f))
        ok++;
    }
  }
  CHECK_EQ(ok, runs);
}

static void test_decode_after_leading_noise() {
  std::mt19937 rng(5);
  std::uniform_int_distribution<int> run(1, 3);
  for (int n = 0; n < 100; n++) {
    std::vector<uint8_t> chips;
    uint8_t level = 1;
    for (int k = 0; k < 60; k++, level ^= 1)
      chips.insert(chips.end(), run(rng), level);
    chips.insert(chips.end(), 6, 0);  // quiet gap before the meter keys up
    auto frame = manchester(build_frame(kGas));
    chips.insert(chips.end(), frame.begin(), frame.end());
    auto t = to_capture(chips, {}, rng);
    ScmDecoder dec;
    std::vector<ScmPacket> out;
    CHECK_EQ(dec.decode(t, out), 1u);
    if (!out.empty())
      CHECK(same_fields(out[0], kGas));
  }
}

static void test_two_frames_in_one_capture() {
  std::mt19937 rng(6);
  Fields a = kGas, b{7654321, 12, 1, 0, 42};
  std::vector<uint8_t> chips = manchester(build_frame(a));
  chips.insert(chips.end(), 10, 0);
  auto fb = manchester(build_frame(b));
  chips.insert(chips.end(), fb.begin(), fb.end());
  auto t = to_capture(chips, {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 2u);
  if (out.size() == 2) {
    CHECK(same_fields(out[0], a) || same_fields(out[1], a));
    CHECK(same_fields(out[0], b) || same_fields(out[1], b));
  }
}

static void test_corrupted_payload_counts_crc_error() {
  std::mt19937 rng(7);
  auto frame = build_frame(kGas);
  frame[5] ^= 0x10;  // flip a consumption bit after the CRC was computed
  auto t = to_capture(manchester(frame), {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 0u);
  CHECK(dec.stats().preamble_matches >= 1);
  CHECK(dec.stats().crc_errors >= 1);
}

static void test_manchester_violation_rejected() {
  std::mt19937 rng(8);
  auto chips = manchester(build_frame(kGas));
  chips.insert(chips.begin() + 120, 2, chips[120]);  // stretch one level by 2 chips mid-payload
  auto t = to_capture(chips, {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 0u);
}

static void test_truncated_frame() {
  std::mt19937 rng(9);
  auto chips = manchester(build_frame(kGas));
  chips.resize(chips.size() - 20);  // receiver dropped out before the checksum ended
  auto t = to_capture(chips, {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(t, out), 0u);
}

static void test_empty_and_tiny_inputs() {
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  CHECK_EQ(dec.decode(nullptr, 0, out), 0u);
  std::vector<int32_t> tiny{30, -30, 61, -250};
  CHECK_EQ(dec.decode(tiny, out), 0u);
  std::vector<int32_t> zeros{0, 0, 0};
  CHECK_EQ(dec.decode(zeros, out), 0u);
  std::vector<int32_t> huge{2000000, -2000000};
  CHECK_EQ(dec.decode(huge, out), 0u);
  CHECK(out.empty());
}

static void test_random_noise_has_no_false_positives() {
  std::mt19937 rng(10);
  std::uniform_int_distribution<int> width(10, 80);
  ScmDecoder dec;
  std::vector<ScmPacket> out;
  for (int n = 0; n < 3000; n++) {
    std::vector<int32_t> t;
    for (int k = 0; k < 300; k++)
      t.push_back((k % 2 == 0) ? width(rng) : -width(rng));
    dec.decode(t, out);
  }
  CHECK(out.empty());
  CHECK_EQ(dec.stats().captures, 3000u);
}

static void test_appends_without_clearing_output() {
  std::mt19937 rng(11);
  auto t = to_capture(manchester(build_frame(kGas)), {}, rng);
  ScmDecoder dec;
  std::vector<ScmPacket> out(1);  // caller already holds one packet
  CHECK_EQ(dec.decode(t, out), 1u);
  CHECK_EQ(out.size(), 2u);
}

int main() {
  static const TestCase tests[] = {
      {"crc matches independent GF(2) division", test_crc_matches_independent_division},
      {"parse_frame extracts every field", test_parse_frame_fields},
      {"parse_frame rejects any single-bit error", test_parse_frame_rejects_bad_crc},
      {"decode clean capture", test_decode_clean_capture},
      {"decode either final bit, with or without trailing space",
       test_decode_either_final_bit_with_or_without_trailing_space},
      {"decode inverted polarity", test_decode_inverted_polarity},
      {"decode with ±8us jitter and ±3% clock", test_decode_with_jitter_and_clock_error},
      {"decode after leading noise", test_decode_after_leading_noise},
      {"two frames in one capture", test_two_frames_in_one_capture},
      {"corrupted payload counts CRC error", test_corrupted_payload_counts_crc_error},
      {"Manchester violation rejected", test_manchester_violation_rejected},
      {"truncated frame rejected", test_truncated_frame},
      {"empty and tiny inputs", test_empty_and_tiny_inputs},
      {"random noise: no false positives", test_random_noise_has_no_false_positives},
      {"decode appends to caller's vector", test_appends_without_clearing_output},
  };
  return run_tests(tests);
}
