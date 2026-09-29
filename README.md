# esphome-airplay

An **AirPlay 2 receiver for ESPHome**, as a `media_source` platform. An ESP32 speaker that already plays Sendspin from Music Assistant also shows up in the iPhone's AirPlay picker. You can multi-select it with HomePods and other AirPlay 2 speakers, and it plays in sync with them.

The protocol stack is [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by Rémi Bouteiller, vendored unmodified. This repo adds an ESPHome output backend and the media source glue.

> **Status: work in progress.** Builds with ESPHome 2026.9.0, not yet tested on hardware. See [HANDOFF.md](HANDOFF.md).

```yaml
external_components:
  - source: github://davidanthoff/esphome-airplay@main
    components: [airplay]

media_source:
  - platform: airplay
    id: airplay_media_source
    # name: Living Room          # defaults to the node's friendly_name
    # output_delay: 0ms          # latency added after the ESP32 (TOSLINK receiver, AVR); negative = play later

media_player:
  - platform: speaker_source
    # ...
    media_pipeline:
      sources:
        - sendspin_media_source
        - airplay_media_source
```

Requirements: ESP32 (S3 with PSRAM recommended), ESP-IDF framework, ESPHome ≥ 2026.9.

## Licence

The vendored upstream code is under its author's licence; see [airplay_core/UPSTREAM.md](airplay_core/UPSTREAM.md). At the pinned commit that is a non-commercial licence. A move to GPL-3.0-or-later is pending upstream. This project's own licence will be chosen once that settles.
