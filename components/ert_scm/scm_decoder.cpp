#include "scm_decoder.h"

#include <cmath>
#include <cstdlib>

namespace esphome {
namespace ert_scm {

// A valid frame never holds a level for more than 2 chips, so longer runs
// (idle gaps, carriers from other systems) only need to stay "long".
static constexpr uint32_t MAX_RUN_CHIPS = 8;
// remote_receiver starts a capture on the first edge, so a frame whose first
// chip is "off" would lose it. Prepend a short idle gap.
static constexpr size_t LEADING_IDLE_CHIPS = 2;
// It also ends on the last mark, so a frame ending in a 1 bit (chips "on,
// off") loses its final chip. Append the same idle gap.
static constexpr size_t TRAILING_IDLE_CHIPS = 2;
static constexpr uint8_t BIT_INVALID = 2;

uint16_t ScmDecoder::crc16(const uint8_t *data, size_t len, uint16_t poly, uint16_t init) {
  uint16_t crc = init;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ poly) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool ScmDecoder::parse_frame(const uint8_t frame[SCM_FRAME_BYTES], ScmPacket &out) {
  // The 5 preamble bits in byte 2 are zero, so they don't affect a CRC with init 0.
  if (crc16(frame + 2, SCM_FRAME_BYTES - 2) != 0)
    return false;
  out.id = (static_cast<uint32_t>(frame[2] & 0x06) << 23) | (static_cast<uint32_t>(frame[7]) << 16) |
           (static_cast<uint32_t>(frame[8]) << 8) | frame[9];
  out.physical_tamper = (frame[3] >> 6) & 0x03;
  out.ert_type = (frame[3] >> 2) & 0x0F;
  out.encoder_tamper = frame[3] & 0x03;
  out.consumption = (static_cast<uint32_t>(frame[4]) << 16) | (static_cast<uint32_t>(frame[5]) << 8) | frame[6];
  out.checksum = static_cast<uint16_t>((frame[10] << 8) | frame[11]);
  return true;
}

void ScmDecoder::to_chips_(const int32_t *timings, size_t count) {
  this->chips_.assign(LEADING_IDLE_CHIPS, 0);
  for (size_t i = 0; i < count; i++) {
    int32_t d = timings[i];
    if (d == 0)
      continue;
    uint8_t level = d > 0 ? 1 : 0;
    long n = std::lround(static_cast<float>(std::labs(d)) / this->chip_us_);
    if (n < 1)
      n = 1;
    if (n > static_cast<long>(MAX_RUN_CHIPS))
      n = MAX_RUN_CHIPS;
    this->chips_.insert(this->chips_.end(), static_cast<size_t>(n), level);
  }
  this->chips_.insert(this->chips_.end(), TRAILING_IDLE_CHIPS, 0);
}

size_t ScmDecoder::scan_phase_(size_t phase, std::vector<ScmPacket> &out, size_t first_new) {
  // Manchester pairs at this chip offset: 10 -> 1, 01 -> 0, 00/11 -> invalid.
  this->bits_.clear();
  for (size_t c = phase; c + 1 < this->chips_.size(); c += 2) {
    uint8_t a = this->chips_[c], b = this->chips_[c + 1];
    this->bits_.push_back(a == b ? BIT_INVALID : a);
  }

  size_t found = 0;
  const size_t nbits = this->bits_.size();
  size_t i = 0;
  while (i + SCM_FRAME_BITS <= nbits) {
    // Compare against the preamble and its inverse at the same time.
    bool normal = true, inverted = true;
    for (size_t k = 0; k < SCM_PREAMBLE_BITS && (normal || inverted); k++) {
      uint8_t want = (SCM_PREAMBLE >> (SCM_PREAMBLE_BITS - 1 - k)) & 1;
      uint8_t got = this->bits_[i + k];
      normal = normal && got == want;
      inverted = inverted && got == (want ^ 1);
    }
    if (!normal && !inverted) {
      i++;
      continue;
    }
    this->stats_.preamble_matches++;

    uint8_t frame[SCM_FRAME_BYTES] = {};
    bool valid = true;
    for (size_t k = 0; k < SCM_FRAME_BITS; k++) {
      uint8_t bit = this->bits_[i + k];
      if (bit == BIT_INVALID) {
        valid = false;
        break;
      }
      if (inverted)
        bit ^= 1;
      frame[k / 8] |= static_cast<uint8_t>(bit << (7 - (k % 8)));
    }
    if (!valid) {
      this->stats_.manchester_errors++;
      i++;
      continue;
    }

    ScmPacket pkt;
    if (!parse_frame(frame, pkt)) {
      this->stats_.crc_errors++;
      i++;
      continue;
    }
    pkt.inverted = inverted;

    bool duplicate = false;
    for (size_t j = first_new; j < out.size(); j++) {
      if (out[j].id == pkt.id && out[j].consumption == pkt.consumption && out[j].checksum == pkt.checksum) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      out.push_back(pkt);
      found++;
    }
    i += SCM_FRAME_BITS;
  }
  return found;
}

size_t ScmDecoder::decode(const int32_t *timings, size_t count, std::vector<ScmPacket> &out) {
  this->stats_.captures++;
  const size_t first_new = out.size();
  this->to_chips_(timings, count);
  size_t found = 0;
  for (size_t phase = 0; phase < 2; phase++)
    found += this->scan_phase_(phase, out, first_new);
  this->stats_.frames += static_cast<uint32_t>(found);
  return found;
}

}  // namespace ert_scm
}  // namespace esphome
