# Changelog

## Unreleased

## [1.1.1] — 2026-10-25

Patch for ESPHome 2026.10 and OLED footer clipping.

- Fix ESPHome 2026.10 build: register `ert_scm` via `remote_base.add_dumper` so codegen reserves a slot (still falls back on older ESPHome)
- Nudge Today OLED age/number/CCF up 2px and shorten the sparkline so `ago` descenders are not clipped

Pin ESPHome configs with `ref: v1.1.1`.

## [1.1.0] — 2026-10-23

OLED polish and ESP-IDF as the recommended framework.

- Default ESP32 framework: **esp-idf** with `sram1_as_iram: true` (examples + scaffold)
- Optional OLED Today / 24h pages: cumulative sparkline, full-width `seen Xm ago` / `WAITING`, title-row fault icons; README mocks
- Silence GPIO5 strapping warning; `image:` `platform: file` for MDI icons
- GitHub Actions CI: host unit tests + `esphome config` on both examples (local `ert_scm`)

Pin ESPHome configs with `ref: v1.1.0`.

## [1.0.0] — 2026-10-22

First tagged release of the LilyGO SX1276 ERT-SCM collector.

- ESPHome `sx127x` OOK continuous RX + `remote_receiver` on DIO2 (GPIO32 on T3 v1.6.1)
- `ert_scm` external component: SCM decode, allowlist, scale (default CCF), optional HA sensors / MQTT topics
- Examples: Home Assistant native API ([`examples/ha-api.yaml`](examples/ha-api.yaml)) and MQTT-only ([`examples/mqtt-only.yaml`](examples/mqtt-only.yaml))
- Apache License 2.0; GitHub Sponsors via `.github/FUNDING.yml`
- Docs aimed at greenfield users (Austin TX / Texas Gas Service tested; other 915 MHz ERT-SCM sites may work)

Pin ESPHome configs with `ref: v1.0.0`.
