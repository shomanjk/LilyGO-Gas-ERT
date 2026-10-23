# Changelog

## [1.0.0] — 2026-10-22

First tagged release of the LilyGO SX1276 ERT-SCM collector.

- ESPHome `sx127x` OOK continuous RX + `remote_receiver` on DIO2 (GPIO32 on T3 v1.6.1)
- `ert_scm` external component: SCM decode, allowlist, scale (default CCF), optional HA sensors / MQTT topics
- Examples: Home Assistant native API ([`examples/ha-api.yaml`](examples/ha-api.yaml)) and MQTT-only ([`examples/mqtt-only.yaml`](examples/mqtt-only.yaml))
- Apache License 2.0; GitHub Sponsors via `.github/FUNDING.yml`
- Docs aimed at greenfield users (Austin TX / Texas Gas Service tested; other 915 MHz ERT-SCM sites may work)

Pin ESPHome configs with `ref: v1.0.0`.
