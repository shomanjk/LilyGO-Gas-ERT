#include "ert_scm.h"

#include <cmath>

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "scm_format.h"

namespace esphome {
namespace ert_scm {

static const char *const TAG = "ert_scm";
static const uint32_t STATS_INTERVAL_MS = 60 * 1000;
// Same as the previous collector (rtl_433 -M time:iso): local time, no offset.
static const char *const LAST_SEEN_FORMAT = "%Y-%m-%dT%H:%M:%S";
// FSK/OOK-page RegRssiValue (0x11 is IRQ_FLAGS_MASK on the LoRa page, which is
// why sx127x_reg.h has no name for it). dBm = -value / 2.
static const uint8_t REG_FSK_RSSI_VALUE = 0x11;
// How far back from a decoded frame to look for its RSSI: the ~6 ms frame,
// remote_receiver's 250 us idle, and main-loop latency (a display refresh can
// hold the loop ~30 ms).
static const uint32_t RSSI_WINDOW_US = 60 * 1000;
// An SCM frame is at least 97 pulses. Captures of at most this many (40 RMT
// symbols) are noise; this replaces remote_receiver's filter_symbols, which
// would hide them from the RSSI window tracking.
static const int32_t SHORT_CAPTURE_PULSES = 80;

void ErtScmComponent::setup() {
  ESP_LOGI(TAG, "MQTT prefix='%s' scale=%" PRIu32 " meters=%u", mqtt_topic_prefix_.c_str(), scale_,
           (unsigned) meters_.size());
  for (auto &m : meters_) {
    ESP_LOGI(TAG, "  meter id=%" PRIu32 " name='%s' enabled=%s history=%s", m.id, m.name.c_str(),
             m.enabled ? "true" : "false", m.history ? "true" : "false");
    if (!m.history)
      continue;
    m.usage_pref = global_preferences->make_preference<UsageHistory::State>(
        fnv1_hash("ert_scm_hist_" + std::to_string(m.id)), true);
    UsageHistory::State saved;
    if (m.usage_pref.load(&saved) && m.usage.load(saved)) {
      ESP_LOGI(TAG, "  restored usage history for id=%" PRIu32, m.id);
    }
  }
  this->set_interval(STATS_INTERVAL_MS, [this]() { this->log_stats_(); });
  if (this->radio_ != nullptr) {
    // Run loop() on every main-loop pass so RSSI is sampled densely enough to
    // catch a 6 ms frame.
    this->high_freq_.start();
  }
}

uint8_t ErtScmComponent::read_rssi_raw_() {
  // Same transaction as SX127x::read_register_() (protected): SX127x is a
  // public SPIDevice, and everything runs on the main loop, so no locking.
  this->radio_->enable();
  this->radio_->write_byte(REG_FSK_RSSI_VALUE & 0x7F);
  const uint8_t value = this->radio_->read_byte();
  this->radio_->disable();
  return value;
}

void ErtScmComponent::loop() {
  if (this->radio_ == nullptr)
    return;
  const uint32_t now = micros();
  if (now - this->rssi_last_read_us_ < RssiTracker::MIN_INTERVAL_US)
    return;  // skip the SPI read entirely, not just the store
  this->rssi_last_read_us_ = now;
  this->rssi_.add(now, this->read_rssi_raw_());
}

MeterConfig *ErtScmComponent::find_meter_(uint32_t id) {
  for (auto &m : this->meters_) {
    if (m.id == id)
      return &m;
  }
  return nullptr;
}

const MeterConfig *ErtScmComponent::find_history_(uint32_t id) const {
  for (const auto &m : this->meters_) {
    if (m.id == id && m.history)
      return &m;
  }
  return nullptr;
}

bool ErtScmComponent::clock_(uint32_t &minute, uint32_t *midnight) const {
  if (this->time_ == nullptr)
    return false;
  ESPTime now = this->time_->now();
  if (!now.is_valid())
    return false;
  // UTC, so the history's hours are real hours even across DST changes.
  minute = static_cast<uint32_t>(now.timestamp / 60);
  if (midnight != nullptr) {
    // Local midnight as a UTC instant; recalc_timestamp_local() applies the
    // offset in effect at midnight, which differs from now's on DST days.
    ESPTime m = now;
    m.hour = 0;
    m.minute = 0;
    m.second = 0;
    m.recalc_timestamp_local();
    if (m.timestamp < 0)
      return false;
    *midnight = static_cast<uint32_t>(m.timestamp / 60);
  }
  return true;
}

void ErtScmComponent::record_usage_(MeterConfig &meter, uint32_t raw) {
  uint32_t minute;
  if (!this->clock_(minute)) {
    ESP_LOGD(TAG, "Clock not synced yet; not recording usage history for id=%" PRIu32, meter.id);
    return;
  }
  meter.usage.add(raw, minute);
  // ESPHome batches flash commits (preferences flash_write_interval, 60 s by default).
  meter.usage_pref.save(&meter.usage.state());
}

bool ErtScmComponent::usage_last_24h(uint32_t meter_id, float out[USAGE_HOURS]) const {
  const MeterConfig *m = this->find_history_(meter_id);
  uint32_t minute;
  if (m == nullptr || !m->usage.has_baseline() || !this->clock_(minute))
    return false;
  uint32_t raw[USAGE_HOURS];
  m->usage.snapshot(minute, raw);
  for (size_t i = 0; i < USAGE_HOURS; i++)
    out[i] = static_cast<float>(static_cast<double>(raw[i]) / static_cast<double>(this->scale_));
  return true;
}

float ErtScmComponent::today_usage(uint32_t meter_id) const {
  const MeterConfig *m = this->find_history_(meter_id);
  uint32_t minute, midnight;
  if (m == nullptr || !m->usage.has_baseline() || !this->clock_(minute, &midnight))
    return NAN;
  return static_cast<float>(static_cast<double>(m->usage.today(minute, midnight)) /
                            static_cast<double>(this->scale_));
}

int32_t ErtScmComponent::seconds_since_seen(uint32_t meter_id) const {
  for (const auto &m : this->meters_) {
    if (m.id == meter_id && m.seen)
      return static_cast<int32_t>((millis() - m.last_seen_ms) / 1000);
  }
  return -1;
}

bool ErtScmComponent::dump(remote_base::RemoteReceiveData data) {
  // Every capture, decoded or not, bounds the RSSI window of the next one.
  const uint32_t now_us = micros();
  const uint32_t rssi_window_us = this->rssi_.capture_window(now_us, RSSI_WINDOW_US);
  if (data.size() <= SHORT_CAPTURE_PULSES) {
    // Noise burst. Claim it so a secondary `raw` dumper stays quiet, as
    // filter_symbols did.
    this->short_captures_++;
    return true;
  }
  this->packets_.clear();
  if (this->decoder_.decode(data.get_raw_data(), this->packets_) == 0) {
    // Not SCM: returning false lets the secondary `raw` dumper print the capture.
    return false;
  }
  // Strongest sample since the previous capture (at most 60 ms back): the
  // frame's level. NaN (not reported) if the capture held more than one frame.
  const float rssi = this->rssi_.frame_dbm(this->packets_.size(), now_us, rssi_window_us);
  const float noise = this->rssi_.noise_dbm();
  char level[40] = "rssi=n/a";
  if (!std::isnan(rssi)) {
    if (std::isnan(noise))
      snprintf(level, sizeof level, "rssi=%.1f dBm", rssi);
    else
      snprintf(level, sizeof level, "rssi=%.1f dBm snr=%.1f dB", rssi, rssi - noise);
  }
  for (const auto &p : this->packets_) {
    MeterConfig *meter = this->find_meter_(p.id);
    ESP_LOGI(TAG, "SCM id=%" PRIu32 " type=%u consumption=%" PRIu32 " tamper=%u/%u %s%s%s", p.id,
             (unsigned) p.ert_type, p.consumption, (unsigned) p.physical_tamper, (unsigned) p.encoder_tamper, level,
             p.inverted ? " [inverted]" : "",
             meter == nullptr ? "" : (meter->enabled ? " [allowlisted]" : " [disabled]"));
    if (meter != nullptr && meter->enabled) {
      this->allowlisted_frames_++;
      meter->seen = true;
      meter->last_seen_ms = millis();
      if (meter->consumption_sensor != nullptr) {
        meter->consumption_sensor->publish_state(
            static_cast<float>(static_cast<double>(p.consumption) / static_cast<double>(this->scale_)));
      }
      if (meter->rssi_sensor != nullptr && !std::isnan(rssi))
        meter->rssi_sensor->publish_state(rssi);
      if (meter->history)
        this->record_usage_(*meter, p.consumption);
      this->publish_(p, rssi);
    }
  }
  return true;
}

void ErtScmComponent::publish_(const ScmPacket &p, float rssi) {
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr || !client->is_connected()) {
    // Readings are cumulative; the meter's next transmission will be published.
    this->publish_skipped_++;
    ESP_LOGW(TAG, "MQTT not connected; not publishing id=%" PRIu32, p.id);
    return;
  }
  const std::string consumption = format_scaled(p.consumption, this->scale_);
  bool ok = client->publish(meter_topic(this->mqtt_topic_prefix_, p.id, "consumption"), consumption, 0, true);
  ok &= client->publish(meter_topic(this->mqtt_topic_prefix_, p.id, "raw"), std::to_string(p.consumption), 0, true);
  // rssi: the frame's peak from the live RSSI register, in dBm (rtl_433's was
  // relative dB, so the scale changes at cutover). Skipped when no sample.
  if (!std::isnan(rssi)) {
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f", rssi);
    ok &= client->publish(meter_topic(this->mqtt_topic_prefix_, p.id, "rssi"), std::string(buf), 0, true);
  }
  if (this->time_ != nullptr) {
    ESPTime now = this->time_->now();
    if (now.is_valid()) {
      ok &= client->publish(meter_topic(this->mqtt_topic_prefix_, p.id, "last_seen"), now.strftime(LAST_SEEN_FORMAT),
                            0, true);
    } else {
      ESP_LOGD(TAG, "Clock not synced yet; skipping last_seen for id=%" PRIu32, p.id);
    }
  }
  if (ok) {
    this->published_++;
    ESP_LOGD(TAG, "Published %s/%" PRIu32 " consumption=%s", this->mqtt_topic_prefix_.c_str(), p.id,
             consumption.c_str());
  } else {
    this->publish_skipped_++;
    ESP_LOGW(TAG, "MQTT publish failed for id=%" PRIu32, p.id);
  }
}

void ErtScmComponent::log_stats_() {
  const ScmDecodeStats &s = this->decoder_.stats();
  ESP_LOGI(TAG,
           "stats: short=%" PRIu32 " captures=%" PRIu32 " preambles=%" PRIu32 " manchester_err=%" PRIu32 " crc_err=%" PRIu32
           " frames=%" PRIu32 " allowlisted=%" PRIu32 " published=%" PRIu32 " publish_skipped=%" PRIu32
           " noise=%.1f dBm",
           this->short_captures_, s.captures, s.preamble_matches, s.manchester_errors, s.crc_errors, s.frames, this->allowlisted_frames_,
           this->published_, this->publish_skipped_, this->rssi_.noise_dbm());
}

}  // namespace ert_scm
}  // namespace esphome
