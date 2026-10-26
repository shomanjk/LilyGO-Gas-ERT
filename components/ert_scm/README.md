# `ert_scm` external component

Phase 2 component: decode Itron **ERT-SCM** from SX1276 OOK pulse timings and publish the Pi-compatible MQTT contract.

## Status

**Decodes, logs, updates local sensors, and optionally publishes MQTT.** Each `remote_receiver` capture goes through `ScmDecoder`. Every valid frame is logged. Enabled allowlisted meters update optional `consumption:` / `rssi:` sensors (Home Assistant native API) and, when an MQTT client is configured, publish retained topics (see *Inputs / outputs*):

```text
[I][ert_scm]: SCM id=12345678 type=12 consumption=654321 tamper=0/0 rssi=-78.5 dBm snr=31.0 dB [allowlisted]
```

A summary line every minute is the Phase 1 health signal:

```text
[I][ert_scm]: stats: short=… captures=… preambles=… manchester_err=… crc_err=… frames=… allowlisted=… published=… publish_skipped=… noise=… dBm
```

- `short` counts captures of 80 pulses or fewer, which are noise (an SCM frame is at least 97). They're dropped before decoding and never printed by `dump: raw`; `captures` counts the rest.
- Rising `preambles` with zero `frames` means timing or polarity trouble.
- Rising `crc_err` means bit errors: weak signal, or the `rx_floor` / bandwidth needs tuning.
- `[inverted]` on a frame means it matched the bit-inverted preamble. Note which way real captures go so the polarity can be pinned later.

- `rssi` / `snr` on a frame, and `noise` in the stats line, come from the SX1276's live RSSI register (see *Signal strength* below). Use them to compare antenna positions or frequencies.

- `publish_skipped` counts frames that weren't published, because MQTT was disconnected or a publish failed. The next transmission catches up, since readings are cumulative.

The component is still commented out in `esphome/lilygo-gas-ert.yaml`; enable it after Phase 1 shows raw pulses on DIO2. The example there publishes to the **shadow prefix** `meters/gas-lilygo`, for running alongside an existing collector.

### Decoder (`scm_decoder.h` / `.cpp`)

Plain C++ with no ESPHome dependencies, written from the protocol description in rtl_433 `ert_scm.c` and the rtlamr docs. No code was copied; those projects are GPL and AGPL.

1. Round each pulse to whole 30.52 µs chips (32768 chips/s), capped at 8 chips. Prepend a 2-chip idle gap, because a capture starts on the first edge.
2. Manchester-decode at both chip phases: `10` → 1, `01` → 0, `00`/`11` → invalid.
3. Find the 21-bit preamble `0x1F2A60` **or its inverse**. This covers either Manchester convention and either DIO2 polarity.
4. Take the 96-bit frame, reject any invalid pair, then require CRC-16 (poly 0x6F63, init 0) over bytes 2–11 to equal 0.
5. Extract ID (26 bits), ERT type, both tamper fields and consumption (24 bits).

### Tests

```bash
make -C tests
```

These host tests run under AddressSanitizer and UBSan. They build frames with an independent encoder and cross-check the CRC with a separate GF(2) long division. They cover:
- both polarities
- ±8 µs jitter and ±3 % clock error
- leading noise and two frames in one capture
- corruption, Manchester violations and truncation
- 3000 random-noise captures with zero false positives

`rssi_tracker_test` covers the RSSI sample ring: picking a frame's peak out of noise, the time window, sample spacing, ring and `micros()` wrap-around, the noise-floor average, reporting no level for multi-frame captures, and keeping an earlier capture's peak out of the next frame's window.

`usage_history_test` covers the hourly history (`usage_history.h`): splitting usage across hours, spreading missed-frame gaps, the 24 h window rolling, a meter swap, the clock stepping back, local midnight, and restoring saved state.

Not covered yet: **real captures**. Once Phase 1 records an SCM frame on the board, add its raw timings as a fixture.

## Intended YAML (draft)

`receiver_id`, `sx127x_id` and `time_id` can be omitted when there is only one of each.

The device is built in the Home Assistant ESPHome add-on, which can't see this repo's files. So the component comes from GitHub:
- **`refresh: 5min`** picks up new commits within minutes. The default caches for one day. Avoid `0s`: the dashboard validates configs continuously, so every check would contact GitHub. ESPHome recommends at least a few minutes.
- **Local changes:** to test them before pushing, swap in `type: local` / `path: ../components` and build outside HA.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/shomanjk/LilyGO-Gas-ERT
      ref: v1.1.0
    components: [ert_scm]
    refresh: 1d

ert_scm:
  id: gas_ert
  receiver_id: ook_rx
  sx127x_id: radio
  time_id: sntp_time
  scale: 100
  meters:
    - id: YOUR_METER_ID
      name: House gas
      enabled: true
      history: true             # optional hourly history, see Inputs / outputs
      consumption:              # optional local sensor, see Inputs / outputs
        id: house_gas_consumption
        name: "House gas consumption"
        internal: true
      rssi:                     # optional per-frame signal strength
        name: "House gas RSSI"
    - id: OPTIONAL_NEIGHBOR_ID
      name: Neighbor gas
      enabled: false
  mqtt_topic_prefix: meters/gas
```

## Inputs / outputs

- **In:** `remote_receiver` raw timings (GPIO32 / DIO2).
- **Out:** retained, QoS 0, under `{mqtt_topic_prefix}/{id}/`. Published on every valid frame from an enabled allowlisted meter, like the previous collector:
  - `consumption`: raw ÷ `scale`, exactly 2 decimals (`scm_format.h`, integer math). It matches the previous collector's Python output for scale 1, 10 and 100.
  - `raw`: integer `consumption_data`.
  - `last_seen`: local time `%Y-%m-%dT%H:%M:%S`, no offset, the same as rtl_433 `-M time:iso`. Skipped until SNTP has synced.
  - `rssi`: the frame's signal strength in **dBm**, 1 decimal (see *Signal strength*). Skipped if there was no RSSI sample, or if the capture held more than one frame. (rtl_433 often reports a different, relative scale.)
- **When MQTT is disconnected,** the frame is logged and counted in `publish_skipped`, not queued.
- **Local, optional:** a per-meter `consumption:` sensor (raw ÷ `scale`, `CCF` by default, `device_class: gas`, `total_increasing`) updates on every frame from an enabled meter, whether or not MQTT is up. Any ESPHome sensor option works: `internal: true` keeps it off MQTT and HA, or override `unit_of_measurement` for a non-default scale. ESPHome's own MQTT state topic for it (`lilygo-gas-ert/sensor/...`) is separate from the `meters/gas/...` contract.
- **Local, optional:** `history: true` on a meter keeps a rolling 24 h of hourly usage (`usage_history.h`).
  - Usage between two readings is split across the hours between them by minutes, so missed frames don't show up as one spike.
  - Hours are real (UTC) hours, so daylight-saving changes don't distort the graph. "Today" starts at local midnight from the `time` component's timezone, and is exact on 23- and 25-hour DST days. Nothing is recorded until SNTP has synced.
  - It's saved to flash (ESPHome preferences, ~112 bytes) on every reading, so the graph survives reboots and OTA updates. ESPHome commits at most once per `flash_write_interval` (60 s). At one reading per 12.5 min, that's ~28 erase cycles per NVS sector per year: centuries of flash life.
  - The saved history is keyed by meter `id`, so changing the id starts a new history.
- **Local, optional:** a per-meter `rssi:` sensor (dBm, `signal_strength`, diagnostic) is updated with each frame from an enabled meter, the same value as the `rssi` topic.
- **For lambdas:**
  - `id(gas_ert).seconds_since_seen(METER_ID)` returns seconds since that meter's last frame, or `-1` before the first one.
  - With `history: true`, `id(gas_ert).today_usage(METER_ID)` returns usage since local midnight (raw ÷ `scale`, NaN if there's no history yet).
  - `id(gas_ert).usage_last_24h(METER_ID, float out[24])` fills per-hour usage, oldest first, and returns false if there's nothing yet.
  - The OLED example in `esphome/lilygo-gas-ert.yaml` uses all three.

## Signal strength

In continuous OOK mode the SX1276 has no per-packet RSSI, only a live `RegRssiValue` (0x11 on the FSK/OOK page, dBm = −value / 2). ESPHome's `sx127x` component doesn't read it, so `ert_scm` does.
- **Sampling:** `ert_scm` asks for a high-frequency main loop and reads the register on every pass, at most every 250 µs. It keeps the last 256 samples (`rssi_tracker.h`).
- **A frame's RSSI:** a frame lasts about 6 ms and is over before the decoder sees it. Its RSSI is the strongest sample in the 60 ms before decoding, but never from before the previous capture. Every capture, decoded or not, starts a new window, so a stronger unrelated transmission just before the frame isn't counted as the frame's level. For that to hold, `ert_scm` has to see every capture: it refuses to build if `remote_receiver` sets `filter_symbols`, and drops short noise captures itself instead. If the main loop was blocked for the whole frame, the value falls back toward the noise floor.
- **Several frames in one capture** (e.g. two meters colliding within the 250 µs idle) get **no** RSSI: logged as `rssi=n/a`, with no sensor update and no `rssi` publish. Samples are timed by main-loop passes and can't be matched to a frame's position inside a capture. Without this rule every frame would get the strongest one's level.
- **Noise floor:** a slow running average of all samples. Frames are rare, so it tracks the background level. `snr` = frame RSSI − noise.
- **Cost:** one 2-byte SPI read every ≥250 µs, plus a main loop that never sleeps. That's fine on mains power.

## Implementation notes

1. Validate decodes against the previous collector's `rtl_433` ERT-SCM hits: same meter ID, close timestamp, same consumption.
2. If `remote_receiver` drops bits under Wi‑Fi load, consider a dedicated RMT capture path on DIO2.
3. Prefer smallest viable decoder (SCM type 12 first); SCM+/IDM only if needed later.
