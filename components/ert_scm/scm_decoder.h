#pragma once

// Itron ERT-SCM (Standard Consumption Message) decoder.
//
// Plain C++ with no ESPHome dependencies, so it builds both in the firmware
// and in the host unit tests under tests/.
//
// Written from the public protocol description (rtl_433 `ert_scm.c` and
// rtlamr docs); no code copied from either (GPL / AGPL).
//
// Frame: 96 bits, MSB first, Manchester-coded at 32768 chips/s (~30.5 µs per
// chip, 16384 bit/s):
//   bits  0-20  preamble 0x1F2A60 (21 bits)
//   bits 21-22  ID, 2 most significant bits
//   bit  23     reserved
//   bits 24-25  physical tamper
//   bits 26-29  ERT type
//   bits 30-31  encoder tamper
//   bits 32-55  consumption (24 bits)
//   bits 56-79  ID, 24 least significant bits
//   bits 80-95  checksum: CRC-16, poly 0x6F63, init 0, over bytes 2..11
//               (incl. the checksum) must be 0
//
// Input is a remote_receiver capture: signed durations in µs, positive =
// carrier on (mark), negative = off (space).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace ert_scm {

static constexpr float SCM_CHIP_US = 1e6f / 32768.0f;  // ~30.52 µs
static constexpr size_t SCM_FRAME_BITS = 96;
static constexpr size_t SCM_FRAME_BYTES = SCM_FRAME_BITS / 8;
static constexpr size_t SCM_PREAMBLE_BITS = 21;
static constexpr uint32_t SCM_PREAMBLE = 0x1F2A60;
static constexpr uint16_t SCM_CRC_POLY = 0x6F63;

struct ScmPacket {
  uint32_t id{0};           // 26-bit ERT id
  uint8_t ert_type{0};      // 4 bits (gas meters: 2, 9, 12)
  uint8_t physical_tamper{0};
  uint8_t encoder_tamper{0};
  uint32_t consumption{0};  // 24-bit raw reading, unscaled
  uint16_t checksum{0};
  // True when the frame matched the bit-inverted preamble, i.e. the
  // opposite Manchester convention or an inverted DIO2 level. Useful for
  // pinning the polarity once real captures are in.
  bool inverted{false};
};

struct ScmDecodeStats {
  uint32_t captures{0};          // decode() calls
  uint32_t preamble_matches{0};  // candidate frames found
  uint32_t manchester_errors{0}; // candidates with an invalid chip pair in the payload
  uint32_t crc_errors{0};        // candidates that failed the checksum
  uint32_t frames{0};            // valid frames returned
};

class ScmDecoder {
 public:
  explicit ScmDecoder(float chip_us = SCM_CHIP_US) : chip_us_(chip_us) {}

  /// Decode every SCM frame in one capture and append them to `out`.
  /// Returns the number of frames appended.
  size_t decode(const int32_t *timings, size_t count, std::vector<ScmPacket> &out);
  size_t decode(const std::vector<int32_t> &timings, std::vector<ScmPacket> &out) {
    return this->decode(timings.data(), timings.size(), out);
  }

  const ScmDecodeStats &stats() const { return this->stats_; }
  void reset_stats() { this->stats_ = ScmDecodeStats{}; }

  /// CRC-16, MSB first, no reflection, no final xor.
  static uint16_t crc16(const uint8_t *data, size_t len, uint16_t poly = SCM_CRC_POLY, uint16_t init = 0);

  /// Validate the checksum of a 12-byte frame and extract its fields.
  /// The preamble bits are not checked here.
  static bool parse_frame(const uint8_t frame[SCM_FRAME_BYTES], ScmPacket &out);

 protected:
  void to_chips_(const int32_t *timings, size_t count);
  size_t scan_phase_(size_t phase, std::vector<ScmPacket> &out, size_t first_new);

  float chip_us_;
  ScmDecodeStats stats_;
  std::vector<uint8_t> chips_;  // one entry per chip, 1 = carrier on
  std::vector<uint8_t> bits_;   // 0, 1, or 2 = invalid Manchester pair
};

}  // namespace ert_scm
}  // namespace esphome
