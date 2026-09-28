# CLAUDE.md

This repo adds an AirPlay 2 receiver to an ESPHome Sendspin speaker, as an ESPHome external component. Read **HANDOFF.md** before changing anything: it records the architecture, the verified ESPHome and upstream facts, and the milestone plan (M1–M4).

## Rules

- Never edit `airplay_core/upstream/` (vendored airplay-esp32). Re-vendor with `scripts/sync-upstream.sh <commit>`.
  - C glue goes in `airplay_core/src/`.
  - ESPHome C++/Python goes in `components/airplay/`.
- ESPHome copies only the top-level files of `components/airplay/`, so don't add subdirectories there.
- Threading: `set_state_()` only on the main loop. Core callbacks arrive on FreeRTOS tasks and are marshalled through `pending_events_`, the same pattern as ESPHome's Sendspin media source, which is the reference implementation.
- Keep the upstream AirPlay task stacks in internal RAM.
- Keep the playback task above upstream's receiver task priorities (see `audio_output.h`).
- When an ESPHome behaviour matters, read the ESPHome source for the pinned version rather than guessing, and add the fact to HANDOFF.md §5.

## Commands

```bash
pip install esphome==2026.9.0      # Python >= 3.12
cd examples && cp secrets.yaml.example secrets.yaml
esphome config  living-room-sendspin-airplay.yaml
esphome compile living-room-sendspin-airplay.yaml
esphome run     living-room-sendspin-airplay.yaml
esphome logs    living-room-sendspin-airplay.yaml
```

There are no unit tests. Verification happens on hardware; the acceptance criteria for each milestone are in HANDOFF.md §8. Keep the per-file status table in HANDOFF.md §7 current.
