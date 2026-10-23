#pragma once

#include <string>
#include <vector>

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/sx127x/sx127x.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "rssi_tracker.h"
#include "scm_decoder.h"
#include "usage_history.h"

namespace esphome {
namespace ert_scm {

struct MeterConfig {
  uint32_t id{0};
  std::string name;
  bool enabled{true};
  sensor::Sensor *consumption_sensor{nullptr};
  sensor::Sensor *rssi_sensor{nullptr};
  bool seen{false};
  uint32_t last_seen_ms{0};
  // Hourly usage for the display, saved to flash (only when `history: true`).
  bool history{false};
  UsageHistory usage;
  ESPPreferenceObject usage_pref;
};

/** Decodes SCM frames from remote_receiver captures, logs them, and publishes allowlisted meters to MQTT. */
class ErtScmComponent : public Component, public remote_base::RemoteReceiverDumperBase {
 public:
  void set_radio(sx127x::SX127x *radio) { radio_ = radio; }
  void set_time(time::RealTimeClock *time) { time_ = time; }
  void set_scale(uint32_t scale) { scale_ = scale; }
  void set_mqtt_topic_prefix(const std::string &prefix) { mqtt_topic_prefix_ = prefix; }
  void add_meter(uint32_t id, const std::string &name, bool enabled, sensor::Sensor *consumption, bool history,
                 sensor::Sensor *rssi) {
    MeterConfig m;
    m.id = id;
    m.name = name;
    m.enabled = enabled;
    m.consumption_sensor = consumption;
    m.history = history;
    m.rssi_sensor = rssi;
    meters_.push_back(m);
  }

  /** Seconds since the last frame from an enabled meter, or -1 if none since boot (or the id
   *  isn't configured). For display lambdas. */
  int32_t seconds_since_seen(uint32_t meter_id) const;

  /** Usage per hour (raw ÷ scale) for the last 24 hours, oldest first, for a meter with
   *  `history: true`. Returns false if there's no history yet or the clock isn't synced. */
  bool usage_last_24h(uint32_t meter_id, float out[USAGE_HOURS]) const;

  /** Usage since local midnight (raw ÷ scale), or NaN if there's no history yet or the
   *  clock isn't synced. */
  float today_usage(uint32_t meter_id) const;

  void setup() override;
  /** Samples the radio's RSSI register on every pass (see rssi_tracker.h). */
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /** Called as a primary remote_receiver dumper for every capture. Returning true (an SCM frame
   *  was found) stops secondary dumpers such as `raw` from also printing it. */
  bool dump(remote_base::RemoteReceiveData data) override;

 protected:
  MeterConfig *find_meter_(uint32_t id);
  const MeterConfig *find_history_(uint32_t id) const;
  /** Current UTC epoch minute and, if asked, local midnight's (see usage_history.h); false
   *  until SNTP has synced. */
  bool clock_(uint32_t &minute, uint32_t *midnight = nullptr) const;
  void record_usage_(MeterConfig &meter, uint32_t raw);
  void publish_(const ScmPacket &p, float rssi);
  void log_stats_();
  /** Live RSSI register in FSK/OOK mode; the sx127x component doesn't expose it. */
  uint8_t read_rssi_raw_();

  ScmDecoder decoder_;
  std::vector<ScmPacket> packets_;
  sx127x::SX127x *radio_{nullptr};
  time::RealTimeClock *time_{nullptr};
  uint32_t scale_{100};
  std::string mqtt_topic_prefix_{"meters/gas"};
  std::vector<MeterConfig> meters_;
  uint32_t allowlisted_frames_{0};
  uint32_t published_{0};
  uint32_t publish_skipped_{0};
  uint32_t short_captures_{0};
  RssiTracker rssi_;
  uint32_t rssi_last_read_us_{0};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace ert_scm
}  // namespace esphome
