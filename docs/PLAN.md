# Plan: ESPHome SX127x + ERT-SCM decode

User-facing overview, board pins, MQTT contract, and troubleshooting: [`README.md`](../README.md).

## Goal

LilyGO LoRa32 **915 MHz (SX1276)** receives Itron **ERT-SCM** and reports consumption via Home Assistant native API and/or retained MQTT topics.

Examples: [`examples/ha-api.yaml`](../examples/ha-api.yaml) (default) and [`examples/mqtt-only.yaml`](../examples/mqtt-only.yaml).

## External documentation

| Resource | URL | Use |
|----------|-----|-----|
| **ESPHome SX127x** | https://esphome.io/components/sx127x/ | Phase 1 radio: OOK continuous RX, `rx_floor`, DIO2 → `remote_receiver` |
| ESPHome remote_receiver | https://esphome.io/components/remote_receiver/ | Pulse capture from DIO2 |
| rtl_433 / rtlamr | protocol descriptions | SCM frame layout, preamble, CRC — description only (do not copy GPL/AGPL code) |

## Chosen stack

**ESPHome**, not PlatformIO-first:

1. **Radio bring-up** via native [`sx127x`](https://esphome.io/components/sx127x/) OOK continuous mode + `remote_receiver` on **DIO2**.
2. **Protocol / app logic** via local external component `ert_scm` — SCM decode, allowlist, scale, MQTT and/or HA API sensors.

```text
SX1276 OOK continuous RX (ESPHome sx127x)
  → DIO2 / GPIO32 demodulated bitstream
  → remote_receiver pulse timings
  → ert_scm external component (SCM decode + filter + scale)
  → MQTT retained meters/gas/{id}/…  and/or HA API sensors
```

## Phase 0 — Scaffold (this repo)

- [x] ESPHome YAML skeleton with LilyGO T3 v1.6.1 pins
- [x] `secrets.example.yaml`, `.gitignore`
- [x] Stub `components/ert_scm/` (decode not implemented yet)
- [x] This plan + gap list

## Phase 1 — Prove the radio (sx127x only)

Reference: [ESPHome SX127x — FSK/OOK Continuous Mode](https://esphome.io/components/sx127x/).

1. Confirm board variant (assume **T3 / LoRa32 V2.1_1.6.1 915 MHz** until measured otherwise).
2. Flash YAML with `modulation: OOK`, `packet_mode: false`, `remote_receiver` on **GPIO32 (DIO2)**, `dump: raw` (uncomment for bring-up; off once `ert_scm` decodes, since printing long captures stalls the loop).
3. Tune:
   - `frequency` — start at **914.825 MHz** (house channel B), then try **914.735 MHz** to cover channels A (~914.65) and B (~914.83). Each meter runs a fixed 12.5 min hop cycle, so one channel gives ~4.8 hits/hr and both give ~9.6 (the Pi's measured rate)
   - `bandwidth` — start wide (e.g. `250_0kHz`) then narrow if noisy
   - `rx_floor` — start `-90`; too high = silence, too low = noise
   - `bitrate` / `bitsync` — try recommended continuous-mode settings from docs
4. Success criteria: serial shows pulse trains correlated with known ERT activity (compare to Pi `rtl_433` timestamps). Not yet decoded.

**Board pins (T3 v1.6.1 SX1276)**

| Function | GPIO |
|----------|------|
| SCK | 5 |
| MISO | 19 |
| MOSI | 27 |
| CS | 18 |
| RST | 23 |
| DIO0 | 26 |
| DIO1 | 33 |
| **DIO2 (OOK data)** | **32** |
| OLED SDA/SCL | 21 / 22 |
| LED | 25 |

## Phase 2 — `ert_scm` decode + publish

| Piece | Notes |
|-------|-------|
| **ERT-SCM frame decode** | Manchester → 96-bit SCM, CRC-16 poly 0x6F63; from public protocol description |
| **Allowlist / multi-meter** | Meter IDs in device YAML |
| **Scale factor** | Default **100**: raw counts ft³ → **CCF** (confirm against a utility bill) |
| **Frequency / `rx_floor` tuning** | Site-specific knobs on `sx127x` |
| **Rolling diagnostics** | Minute `stats:` log line; optional sensors |
| **MQTT** | Fixed topics `meters/gas/{id}/…` (not discovery) |
| **HA native API** | Optional per-meter `consumption:` / `rssi:` sensors |

## MQTT contract (unchanged)

Publish **retained**, QoS 0, only on valid allowlisted hits:

| Topic | Payload |
|-------|---------|
| `meters/gas/{id}/consumption` | CCF with the default scale, 2 decimals (`consumption_data / scale`) |
| `meters/gas/{id}/raw` | int `consumption_data` |
| `meters/gas/{id}/rssi` | float if available |
| `meters/gas/{id}/last_seen` | ISO-ish timestamp |

Client id e.g. `lilygo-gas-ert` (use a new id so it does not collide with any previous collector).

## Optional — replacing an existing MQTT collector

If another process already publishes `meters/gas/{id}/…`, run the LilyGO under a temporary prefix (e.g. `meters/gas-lilygo`) until readings match, then switch `mqtt_topic_prefix` to `meters/gas` and stop the old publisher in the same sitting. LilyGO `rssi` is in **dBm**; rtl_433-style collectors often use a different relative scale.

Greenfield installs can ignore this section.

---

## Missing / challenging pieces

### Likely solvable (medium)

1. **SCM decoder** — **Done:** written from the public protocol description; host unit tests under `tests/`.

2. **`rx_floor` / bitrate calibration**  
   Docs are clear that OOK floor is site-specific. Plan iteration against Pi ground truth.

3. **RSSI on continuous OOK**  
   Packet-mode `on_packet` gives RSSI; continuous mode doesn't. **Done:** `ert_scm` samples the live FSK/OOK RSSI register on every loop pass and publishes each frame's peak in dBm (see `components/ert_scm/README.md`, *Signal strength*).

### Hard / risk (validate early)

4. **Narrowband vs SDR**  
   RTL-SDR @ 1 MHz sees hops; SX1276 OOK BW ≤ 250 kHz. Capture rate may be **lower** than the Pi. Mitigations: park on the hop your meter uses most; optional slow frequency dither later.  
   *Measured 2026-10-13 (Pi log, ~20 days):* hops follow a fixed 12.5 min cycle, not random channels. The house meter uses two channels in the Pi window (~914.65 / ~914.83 MHz), one hit each per cycle, giving a median 9 hits/hr. Parking on one channel still gives ~4.8/hr. Readings are cumulative, so that is plenty.

5. **Capture quality under Wi‑Fi**  
   `remote_receiver` is RMT-based on ESP32 in modern ESPHome. If decode rates are poor with Wi‑Fi up, consider a dedicated RMT capture path on DIO2.

6. **Board variant mismatch**  
   Pin table assumes T3 v1.6.1. Older TTGO revisions sometimes needed DIO1/DIO2 jumpers. Verify silkscreen / continuity before debugging “no pulses.”

### Not missing (already covered by docs / hardware)

- SX1276 **can** do OOK continuous RX in ESPHome (documented).
- LilyGO T3 v1.6.1 **does** route **DIO2 → GPIO32** (required by the docs).
- Wi‑Fi, OTA, MQTT client — stock ESPHome.

### Fallback if Phase 1–2 fail RF-wise

- Keep Pi / RTL-SDR collector as temporary backup.
- Alternate hardware (e.g. ESP32 + 915 MHz CC1101) only if SX1276 path proves inadequate.

---

## Attribution

- ESPHome `sx127x` / `remote_receiver` — ESPHome project.
- SCM protocol description — rtl_433 / rtlamr docs (no code copied).
- MQTT contract / scale validation — Pi-SDR-Lab gas bridge.

## Decision log

| Date | Decision |
|------|----------|
| 2026-10-13 | Prefer ESPHome `sx127x` continuous OOK as radio foundation |
| 2026-10-13 | Implement SCM decode from public protocol description (not a third-party radio stack) |
| 2026-10-13 | Keep fixed `meters/gas/…` topics (no HA entity rewrite) |
