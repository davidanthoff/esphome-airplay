# Click test: ESP vs a reference AirPlay speaker

This measures the constant offset and the drift of the ESP speaker against another AirPlay 2 speaker (HomePod, Apple TV) with one microphone. The results are in HANDOFF.md, milestone M2.

## Recording

1. **Two separate sound systems.** The ESP's output and the reference must come out of different speakers.
2. **With an Apple TV**, first run *Settings → Video and Audio → Calibration → Wireless Audio Sync*.
3. On the iPhone, group the reference and the ESP, and play a plain click track such as "Metronome 60 BPM" from Apple Music.
4. **Record with a second device,** not the streaming iPhone.
   - Put it clearly closer to the ESP than to the reference, and measure both distances (in feet).
   - Both clicks should be clearly audible above the room. Normal household noise is fine.
5. Record for **10–30 minutes** without moving anything.
6. **At the end, turn the ESP's amp all the way down for ~10 s,** so only the reference plays. Then stop the recording.
   - Use the amp's knob, not the iPhone's volume slider for the ESP.
   - The tool finds these reference-only beats automatically.

## Analysis

```bash
python -m venv .venv && .venv/bin/pip install -r requirements.txt   # or: uv venv / uv pip install
.venv/bin/python analyze.py "New Recording.m4a" result --dist-esp 3 --dist-ref 11
```

It prints the median offset (negative = the ESP is early), its spread and the drift in ms/hour, and writes `result.png`: the offset per beat and 1-minute medians over time.

**Reading it:**
- A **sawtooth** of a few ms is the position servo correcting the ESP's crystal against the group clock, and it's expected. A **slope** would be real drift.
- If a constant offset remains, use `output_delay:`: positive values make the ESP play earlier, negative values later.
- The distance correction assumes 343 m/s. An error of ±0.5 ft in a distance is about ±0.45 ms.

## How it works

- Every ~1 s click cluster becomes a beat window.
- The reference-only beats at the end give an averaged reference click template.
- **In each beat:**
  1. The reference click is located by cross-correlation (band-passed 1.5–8 kHz, sub-sample interpolation) and subtracted.
  2. The ESP click is located in what remains, using an ESP template built the same way from the first minutes.
  3. The offset is the difference between the two positions.
