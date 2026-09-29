# esphome-airplay

An **AirPlay 2 receiver for ESPHome**, as a `media_source` platform. An ESP32 speaker that already plays Sendspin from Music Assistant also shows up in the iPhone's AirPlay picker. You can multi-select it with HomePods and other AirPlay 2 speakers, and it plays in sync with them.

The protocol stack is [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by Rémi Bouteiller, vendored unmodified. This repo adds an ESPHome output backend and the media source glue.

> **Status: work in progress.** Builds with ESPHome 2026.9.0 and plays on hardware. Sync was verified against an Apple TV on the previous upstream pin and is being re-checked after the move to upstream's `staging` branch. See [HANDOFF.md](HANDOFF.md).

```yaml
external_components:
  - source: github://davidanthoff/esphome-airplay@main
    components: [airplay]

media_source:
  - platform: airplay
    id: airplay_media_source
    # name: Living Room          # defaults to the node's friendly_name
    # output_delay: 0ms          # latency added after the ESP32 (TOSLINK receiver, AVR); negative = play later
    # model: esphome-airplay     # advertised over mDNS; see below
    # airplay_1_only: false      # classic AirPlay 1 only (RTSP on port 5000)

media_player:
  - platform: speaker_source
    # ...
    media_pipeline:
      sources:
        - sendspin_media_source
        - airplay_media_source
```

Requirements: ESP32 (S3 with PSRAM recommended), ESP-IDF framework, ESPHome ≥ 2026.9.

**`model`:** cosmetic. `model: "AudioAccessory5,1"` makes iOS show a HomePod mini icon. The receiver always advertises `manufacturer=ESPHome` as well, so Music Assistant never mistakes it for a real HomePod. If it did, MA would send all volume changes over AirPlay, even while Sendspin is playing.

## Licence

The vendored upstream code is GPL-3.0-or-later, with an additional permission for linking Espressif's binary-only components (`upstream/LICENSE-EXCEPTION`); see [airplay_core/UPSTREAM.md](airplay_core/UPSTREAM.md). This project's own licence is not chosen yet.
