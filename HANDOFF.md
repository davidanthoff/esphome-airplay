# esphome-airplay: handoff

**Goal:** make a cheap ESP32 Sendspin speaker also show up as a **native AirPlay 2 receiver** on the iPhone. It should be multi-selectable and play in sync with HomePods and other AirPlay 2 speakers, while staying a normal Sendspin player for Music Assistant. It lives entirely in a repo David controls, as an ESPHome **external component**. No upstream PRs to ESPHome are needed.

**State on 2026-09-28:** builds, not yet run on hardware. The example config compiles and links with ESPHome 2026.9.0 (ESP-IDF 5.5.5). That covers the local checkout and the `github://` source that ESPHome Builder in Home Assistant uses. All 38 `airplay_core` objects are linked into the firmware, and the AirPlay sources compile without warnings. The image is 1.6 MB, about 20% of the 16 MB layout's app partition. Next: flash it and work through milestone M1's hardware checks (§8).

This document is written for whoever picks this up next (most likely Claude Code on David's machine). It records what was decided and why, and which facts were verified, so none of that needs to be rediscovered.

---

## 1. How we got here

Everything started with one question: *can an iPhone multi-select several cheap speakers over AirPlay 2 and route them through Music Assistant (MA)?*

| Idea | Verdict | Why |
|---|---|---|
| MA's **AirPlay Receiver plugin** as-is | ❌ | It runs **shairport-sync in classic AirPlay 1 mode**: the config template uses embedded `tinysvcmdns` (classic-only), there's no NQPTP, and the format is fixed at ALAC 44.1/16. iOS only multi-selects AirPlay 2 targets. Its "7000–7999 ports" are just MA's port-allocation scheme and say nothing about AirPlay 2. |
| Several AirPlay 2 shairport-sync instances feeding MA, with MA deduping the streams and grouping Sendspin players | ⚠️ | Works for all-Sendspin groups, but the timing is lost at shairport-sync's pipe output (raw PCM, no timestamps). Output then sits at MA's ~5 s buffer delay: no sync with real HomePods, and lip-sync breaks. It also needs group detection (SETUP group UUID / SETPEERS), which shairport-sync doesn't expose. |
| Pass the AirPlay timestamps through to Sendspin in MA | ✅ in theory | Both protocols are "play sample N at time T", so this is a timestamp converter. It needs a forked receiver that outputs timed frames, and NQPTP plumbing on the MA host. |
| Share one NQPTP between MA's AirPlay sender and the receivers | ❌ | NQPTP **only monitors** a PTP clock on 319/320 ("not a PTP clock") and tracks **one clock at a time**. It can't be the clock source MA's sender needs, and both want exclusive 319/320 on one IP. |
| **AirPlay 2 receiver directly on the ESPHome Sendspin device** | ✅ **chosen** | Every speaker runs its own PTP follower, and the iPhone does the syncing, as with a HomePod. No server-side plumbing. The hard protocol work already exists in **rbouteiller/airplay-esp32** (ESP-IDF C: RTSP, HomeKit pairing, FairPlay, PTP, ALAC/AAC, timing engine). |

Constraint from David: **no upstream PRs required.** Everything must work from a repo he controls, pulled in by ESPHome.

---

## 2. Target hardware

- **Board:** Waveshare **ESP32-S3-POE-ETH**: 16 MB flash, 8 MB octal PSRAM, onboard **W5500 Ethernet** (wired PTP is much better than WiFi).
- **Output:** **TOSLINK** from GPIO17, using ESPHome's `i2s_audio` speaker in `spdif_mode`, at **44.1 kHz**. That is AirPlay's native rate, so there's no resampling on the AirPlay path.
- **Current role:** Sendspin player ("Sendspin TOSLINK", HA entity `media_player.sendspin_toslink_54b574`, node `living-room-sendspin`), based on RealDeco's SendspinZero config.
- **ESPHome:** config has `min_version: 2026.8.0`. Everything here was checked against **2026.9.0** (tag) and `dev` @ `30dc453` (2026-09-29).
- Full config with AirPlay added: [`examples/living-room-sendspin-airplay.yaml`](examples/living-room-sendspin-airplay.yaml). Every addition is marked `esphome-airplay`.

---

## 3. Architecture

```
 iPhone ──RTSP/HAP/FairPlay──► ┌──────────────── airplay_core (ESP-IDF component, C) ─────────────────┐
        ──PTP (319/320)──────► │ upstream/ (airplay-esp32 @ 811d5f8, unmodified)                    │
        ──audio (UDP/TCP)────► │   rtsp/ hap/ plist/ ptp_clock  audio_receiver → decoder → buffer   │
                               │   audio_timing (anchor + position servo)                          │
                               │ src/ (our glue)                                                   │
                               │   airplay_core.c          bring-up (replaces main.c), events, vol │
                               │   mdns_airplay_esphome.c  mDNS TXT records w/o mdns_init()        │
                               │   audio_output_esphome.c  output backend: playback task ──┐       │
                               └───────────────────────────────────────────────────────────┼──────┘
                                        host.write(pcm, frames, rate)                          │
                               ┌───────────────── components/airplay (ESPHome, C++) ───────▼──────┐
                               │ AirPlayMediaSource : media_source::MediaSource                    │
                               │   write_output() ─► speaker_source media player                   │
                               │   request_play_uri_("airplay://current") on session start         │
                               └──────────┬───────────────────────────────────────────────────────┘
                                          ▼
                     resampler speaker ─► mixer speaker ─► i2s_audio (spdif_mode) ─► TOSLINK
                                          │
          notify_audio_played(frames, ts) ◄┘ (from the I2S DMA completion callback)
                     └──► airplay_core_notify_played() ─► queued = submitted − played
                                                        ─► audio_output_get_pipeline_us()
                                                        ─► timing engine's servo keeps the iPhone's schedule
```

### Why the core is a separate ESP-IDF component and not files in `components/airplay/`

ESPHome's loader copies **only the top-level files** of an external component (`esphome/loader.py`, `ComponentManifest.resources`; `recursive_sources` is used for core only). The upstream code relies on its `rtsp/ hap/ plist/ audio/ network/` directories and include paths. So `components/airplay/media_source.py` calls:

```python
esp32.add_idf_component(name="airplay_core", path=str(AIRPLAY_CORE_DIR))
```

ESPHome writes that into the build's `idf_component.yml` as a `path:` dependency and adds it to the main component's REQUIRES. This was checked with `esphome compile --only-generate` on 2026.9.0: the generated `src/idf_component.yml` contains `airplay_core: {path: …}`. `AIRPLAY_CORE_DIR` is computed from `__file__`, which works because ESPHome clones the whole repo for git external components.

### The seam that makes this port small

Upstream is already built around pluggable parts, and the port only plugs into those. No upstream file is edited.

1. **Output backend.** Upstream picks one of `audio_output.c` (I2S), `audio_output_spdif.c` or `audio_output_usb.c` at build time and keeps weak defaults in `audio_output_common.c`. `src/audio_output_esphome.c` is simply another backend.
2. **Latency feedback.** Upstream's timing engine asks `audio_output_get_pipeline_us()` "how long until the next sample I write is heard". The I2S backend answers with "frames handed to DMA − frames DMA sent". We answer with "frames ESPHome accepted − frames ESPHome reported played". ESPHome's `speaker_source` forwards the I2S speaker's DMA-completion callback to the active source as `notify_audio_played(frames, esp_timer_timestamp)`, so **the resampler, mixer, ring buffers and DMA are all inside the measurement.** Only delay *after* the ESP32 (TOSLINK receiver, AVR DSP) is not, and the `output_delay` option covers that.
3. **Volume.** Upstream calls `dac_set_volume(dB)` through a registered `dac_ops_t`. We register ops that forward to the host, which calls `request_volume_()`.
4. **Session state.** `rtsp_events_register()` gives CONNECTED/PLAYING/PAUSED/DISCONNECTED/METADATA.

---

## 4. Repository layout

```
esphome-airplay/
├── HANDOFF.md                     ← this file
├── CLAUDE.md                      short rules for Claude Code; points here
├── README.md
├── components/airplay/            ESPHome external component (only top-level files are copied!)
│   ├── __init__.py                namespace
│   ├── media_source.py            schema + to_code: add_idf_component, Kconfig, sockets, IDF includes
│   ├── airplay_media_source.h     MediaSource subclass
│   └── airplay_media_source.cpp   state machine, write path, volume, trampolines
├── airplay_core/                  ESP-IDF component
│   ├── CMakeLists.txt             which upstream files are compiled / replaced
│   ├── idf_component.yml          espressif/mdns, libsodium, esp_audio_codec
│   ├── Kconfig                    upstream symbol names (AIRPLAY_FORCE_V1, …)
│   ├── UPSTREAM.md                pinned commit + licence notes
│   ├── include/airplay_core.h     the C API between core and ESPHome
│   ├── src/                       our glue (see §3)
│   └── upstream/                  vendored airplay-esp32 (main/, components/dac/, LICENSE) – never edit
├── examples/
│   ├── living-room-sendspin-airplay.yaml
│   └── secrets.yaml.example
└── scripts/sync-upstream.sh       re-vendor upstream at a commit
```

Why vendoring and not a git submodule: ESPHome does init submodules recursively for git external components, but upstream itself has nested submodules (u8g2 and friends). Every device build would then drag those in. Vendoring the ~1 MB we need is simpler and pins exactly what we compile.

---

## 5. Verified facts about ESPHome (2026.9.0, same on dev)

Paths are in the ESPHome repo. These are the facts the design depends on.

**`media_source/media_source.h`: the interface**
- A source implements `play_uri`, `handle_command`, `can_handle`, and optionally `has_internal_playlist`, `notify_volume_changed`, `notify_mute_changed`, `notify_audio_played(frames, timestamp)`.
- A source calls `write_output(data, len, timeout_ms, AudioStreamInfo)`, `set_state_()` (**main loop only**), and the "smart source" helpers `request_play_uri_()`, `request_volume_()`, `request_mute_()`.

**`speaker_source/speaker_source_media_player.cpp`: the orchestrator**
- `SourceBinding::request_play_uri/volume/mute` **`defer()` to the main loop**, so calling them from any task is safe. `request_play_uri` becomes `make_call().set_media_url(uri).perform()`.
- On play: if another source is active it gets **STOP** and `speaker->stop()`, then the player waits for it to go IDLE and for the speaker to stop. After that it calls `play_uri()` on the first source whose `can_handle()` matches, and **resets `pending_frames` to 0**.
- A source becomes the active source when it reports PLAYING. `write_audio` from a non-active source just sleeps for `timeout_ms` and returns 0.
- **The first write after a stream-format change returns 0**: the speaker is reconfigured and the call sleeps `timeout_ms`. The writer has to retry the same data.
- `notify_audio_played` is called only on the **active** source, only for frames that source wrote (CAS on `pending_frames`), from the **speaker callback task**.
- **Order matters for latency accounting.** `set_state_(PLAYING)` inside `play_uri()` makes the source active immediately, but `pending_frames` is reset to 0 only *after* `play_uri()` returns (`try_execute_play_uri_`). Frames written in between are never reported as played. So the AirPlay source starts writing, and resets its own cursor, in the next `loop()`.
- **PAUSED does not stop the speaker chain.** The source stays active, so played-frame reports keep arriving while the chain drains. The chain stops by itself about 1 s after the last write. Resetting our submitted/played cursor on a quick pause → resume would count still-playing old audio against a zeroed "submitted": the queue then reads too short, and everything plays late for the rest of the session. `audio_output_esphome.c` resets on resume only after ≥1.5 s without played frames.
- When a source goes IDLE *without* the player having stopped it, the player advances its playlist. That's why smart sources return `has_internal_playlist() = true`.
- Sendspin's source (`sendspin/media_source/sendspin_media_source.cpp`) is the **reference implementation** for everything here, including the `pending_start_` guard ("the orchestrator may send a stop command before play_uri").

**`i2s_audio/speaker/i2s_audio_speaker.cpp`**
- Its timestamp is `esp_timer_get_time()` taken in the I2S `on_sent` ISR, i.e. when a DMA buffer finished clocking out. Played frames therefore advance in DMA-block steps.

**`mdns/mdns_esp32.cpp`**
- ESPHome calls `mdns_init()` + `mdns_hostname_set()` once in `setup()` (priority `AFTER_CONNECTION`). A second `mdns_init()` returns `ESP_ERR_INVALID_STATE`, and upstream `ESP_ERROR_CHECK`s it, which would put the device in a reboot loop. Adding services later with the IDF `mdns_service_add()` is fine. ESPHome pins `espressif/mdns` **1.12.0**, which satisfies upstream's `^1.11.1`.

**`esp32/__init__.py`**
- `add_idf_component(name, repo, ref, path)`: `path` becomes a component-manager `path:` dependency.
- `add_idf_sdkconfig_option()` works for our own Kconfig symbols; they showed up in the generated sdkconfig.
- `DEFAULT_EXCLUDED_IDF_COMPONENTS` includes `esp_http_client`, `esp-tls`, `esp_http_server` and `json`. We re-include the first two (`dacp_client.c`).

**`socket/__init__.py`**
- `consume_sockets(n, name, SocketType.TCP|UDP|TCP_LISTEN)`. With our estimates, ESPHome computed `CONFIG_LWIP_MAX_SOCKETS=27` for David's config.

**`noise/__init__.py`**
- With `api: encryption:`, ESPHome links `esphome/libsodium 1.10021.11`. David's config has no API encryption. See the risks section (§9).

**Framework**
- Only ESP-IDF. The component is `only_on_esp32`, and upstream needs IDF ≥ 5.5.

**Build toolchain (`espidf/toolchain.py`, `espidf/framework.py`)**
- 2026.9.0 builds with a native ESP-IDF 5.5.5 install, not PlatformIO. It lives in `~/.cache/esphome/idf` (framework, xtensa toolchain, and a Python env in `penvs/`).
- ESPHome creates that Python env with `<its own python> -m venv` (`framework_helpers.create_venv`, interpreter from `$PYTHONEXEPATH` or `sys.executable`). Ubuntu's system Python can't do that without the `python3-venv` apt package. A `uv`-managed CPython (`uv venv --python-preference only-managed`) avoids the need for sudo.

**`psram/__init__.py`: where plain `malloc` goes**
- ESPHome builds with `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` and **not** `CONFIG_SPIRAM_USE_MALLOC`. Plain `malloc`/`calloc` only ever return internal RAM. ESPHome code gets PSRAM explicitly (`RAMAllocator`, `heap_caps_*`).
- Upstream airplay-esp32 assumes the opposite. Its `config/sdkconfig.defaults` has `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` (every `malloc` above 1 KB goes to PSRAM) and `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y`.
- Consequence, seen on hardware: `esp_audio_codec`'s AAC decoder couldn't open ("There is no enough memory for AAC buffer"), so AirPlay played silence. Internal RAM was ~60 KB free with a largest block of 31 KB once the speaker chain was running. Fixed by `src/codec_alloc_psram.c`, which overrides the codec's weak `media_lib_module_malloc/calloc` with upstream's 1 KB rule. Upstream's own plain `malloc`s still go to internal RAM (§9).

**`wifi/`: power save while streaming**
- With the default `power_save_mode`, the station sits in `WIFI_PS_MIN_MODEM` (the log shows `Set ps type: 1`). Streaming components switch it off at runtime:
  - Python: `wifi.enable_runtime_power_save_control()` and `wifi.enable_runtime_roaming_suppression()`.
  - C++: `wifi::global_wifi_component->request_high_performance()` / `release_high_performance()` and `request_roaming_suppression()` / `release_roaming_suppression()`.
  - These are counted and thread-safe; the switch is applied on the main loop. Sendspin does this in `SendspinHub::on_request/release_high_performance()`.
- The AirPlay source now holds both from client connect (or first PLAYING) until disconnect. TCP (buffered AirPlay 2) tolerates modem sleep. UDP (realtime audio, PTP/NTP timing) doesn't.

**`logger/` and `esp32/__init__.py`: ESP-IDF log output**
- The logger hooks `esp_log_set_vprintf()`. Everything logged with `ESP_LOGx` from plain IDF C code (our core and upstream) is re-logged at the logger's own level under the tag **`esp-idf`**. So `logger: level:` and `logs:` per-tag settings do **not** filter it.
- What filters it is `esp32: framework: log_level:` (default **ERROR**), which sets `CONFIG_LOG_DEFAULT_LEVEL_*`. The example sets `INFO` for bring-up.

---

## 6. Verified facts about upstream (airplay-esp32 @ `811d5f8`, 2026-09-21)

- **Bring-up** (`main.c: start_airplay_services`): `ptp_clock_init → hap_init → audio_receiver_init → audio_output_init → mdns_airplay_init → audio_output_start → rtsp_server_start`. Before that, `settings_init` and `playback_control_init` run. Mirrored in `src/airplay_core.c`.
- **Compiled set:** listed in `airplay_core/CMakeLists.txt`. It references only `audio_output_*` from the output API, so there are no dependencies on led/wifi/display/web code. Checked by grep.
- **Replaced:** `main.c`, `network/mdns_airplay.c` (it calls `mdns_init`), `audio/audio_output*.c`, `audio/audio_resample.c` (ESPHome's resampler speaker does this job).
- **Device identity:** `esp_read_mac(ESP_MAC_WIFI_STA)` is used everywhere (rtsp_handlers, hap_pair_verify, mdns). That's fine on Ethernet too, because it just needs to be stable and consistent.
- **Persistence:** NVS namespace `"airplay"` holds the device name, volume, HAP pairing keys and so on. It doesn't collide with ESPHome, which already ran `nvs_flash_init`. `rtsp_handlers.c` `/info` reads the name from settings, so `airplay_core_init()` writes the configured name there when it changed.
- **Output timing contract:** `FRAME_SAMPLES = 352`. The playback task priority `AUDIO_PLAYBACK_TASK_PRIORITY = 9` must outrank the receiver tasks (8/7/5), otherwise playback drifts late (upstream issue #122). **Task stacks must stay in internal RAM** (`spiram_task.h`: flash ops disable the cache).
- **Timing engine knobs:** `CONFIG_AIRPLAY_TIMING_THRESHOLD_MS` (buffered AAC, default 25) and `CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS` (realtime ALAC, default 50). The `rtsp_handlers.c` comment says: **do not** advertise output latency to the sender, because the engine compensates internally and advertising would apply it twice.
- **Remote control:** play/pause/next from the device works only over **DACP (AirPlay 1)**. AirPlay 2 needs MRP, which isn't implemented, so HA→iPhone transport and volume sync won't work for AirPlay 2 sessions.
- **Stopping a session:** client slots are static in `rtsp_server.c`, and there's no public "disconnect". We currently do `rtsp_server_stop()` + `rtsp_server_start()`.

---

## 7. What's done / status per file

| File | State |
|---|---|
| `components/airplay/media_source.py` | ✅ validated: `esphome config` passes, and `compile --only-generate` produces the IDF path dependency, sdkconfig options and socket reservations |
| `components/airplay/airplay_media_source.{h,cpp}` | 🟡 compiles and links for esp32s3 on 2026.9.0. Not yet run on hardware |
| `airplay_core/include/airplay_core.h` | ✅ compiles as C and C++ in the firmware build |
| `airplay_core/src/airplay_core.c` | 🟡 compiles and links, no warnings. Not yet run on hardware |
| `airplay_core/src/audio_output_esphome.c` | 🟡 same. **This is where M2 happens** |
| `airplay_core/src/mdns_airplay_esphome.c` | ✅ on hardware: the iPhone lists the device and pairs. TXT records copied 1:1 from upstream |
| `airplay_core/src/codec_alloc_psram.c` | 🟡 puts the AAC/ALAC decoders' memory in PSRAM (§5). Linked, as `nm` confirms; not yet tried on hardware |
| `airplay_core/CMakeLists.txt`, `idf_component.yml`, `Kconfig` | ✅ CMake and the component manager resolve them on 2026.9.0 with no changes needed. `espressif/mdns` is deduplicated with ESPHome's 1.12.0 |
| `airplay_core/upstream/` | ✅ vendored by `scripts/sync-upstream.sh`. All 35 listed files compile without warnings |
| `examples/living-room-sendspin-airplay.yaml` | ✅ compiles on 2026.9.0. Has debug sensors (heap/PSRAM) and IDF `log_level: INFO` for M1 |

`TODO(Mx)` markers in the code point to the milestone that owns each item.

---

## 8. Milestones

### M1: builds, advertises, plays (no sync claims yet)
1. ✅ **Done 2026-09-28:** it compiled and linked on the first real build with no changes, so none of the problems anticipated below came up. `esphome compile examples/living-room-sendspin-airplay.yaml` (needs `examples/secrets.yaml`). Fix build errors. Expected ones:
   - component-manager resolution of `airplay_core` deps (mdns, libsodium, esp_audio_codec);
   - warnings-as-errors in upstream C: loosen per file in `CMakeLists.txt`, don't edit `upstream/`;
   - missing IDF components (anything else in `DEFAULT_EXCLUDED_IDF_COMPONENTS` → `esp32.include_builtin_idf_component`);
   - `CONFIG_*` symbols referenced by upstream code but not declared in our `Kconfig`: add them with upstream's defaults. Build-time `#ifdef`s like `CONFIG_BT_A2DP_ENABLE` should simply stay undefined.
2. Flash with `esphome run …`, then check `esphome logs`. The component must reach `airplay_core_start()` once Ethernet is up.
   - **First hardware run (2026-09-28):** Sendspin still works. The iPhone lists the device, pairs, and sends SETUP, SETRATEANCHORTIME and metadata. PTP locks, and the player makes the AirPlay source active (`State changed to PLAYING`). There was no sound because the AAC decoder failed to allocate (§5, fixed by `codec_alloc_psram.c`). Note: in that run Ethernet never came up (`ethernet: Connecting failed`) and the device was on Wi-Fi.
3. The iPhone shows "Sendspin TOSLINK" in the AirPlay picker with a speaker icon. Selecting it pairs (transient HAP) and plays.
4. Music plays through TOSLINK for both **buffered AAC** and **realtime ALAC**. The sending app decides which type is used, not where the speaker is picked. Against this receiver, iOS sent buffered (`stream_type=103`) for everything tried, including Apple Music, YouTube and Safari video. To exercise ALAC/realtime, set `airplay_1_only: true`: AirPlay 1 is always realtime ALAC (`stream_type=96`) over UDP with NTP timing.
   - **2026-09-28:** buffered AAC plays after the PSRAM fix (PR #2). YouTube is in lip sync by eye.
   - AirPlay 1 connected and decoded ALAC, but it stayed silent. The receive buffer never filled (`buffered` ≤ 17 frames), and every ~0.5 s `Skipped N stale start frames` discarded 40–60 frames at the gaps in the stream. That points to UDP loss. The device was on Wi-Fi in modem power save (see §5), and the fix is to hold high-performance Wi-Fi during a session. Still to be confirmed on hardware.
5. Sendspin still works afterwards, and switching MA→AirPlay→MA works.
6. Record the heap/PSRAM headroom (`debug:` component) while streaming. ESPHome's free-heap sensor is enough.

**Done when:** a 10-minute AirPlay session plays with no drop-outs and no reboots, and Sendspin is unaffected.

### M2: sync
1. Log `audio_output_get_pipeline_us()` and the timing engine's servo stats (`audio_timing.c`: `pos_err_filtered_us`, `servo_trims`, late drops) once a second. Check that pipeline_us is stable and plausible, i.e. roughly the resampler+mixer+DMA buffering.
   - Upstream already logs `audio_time: Playout: err=… depth=…` once a second and the servo engage/disengage lines. `airplay_out` now logs `pipeline=… ms submitted=… played=…` every 10 s.
   - `err` is measured against the engine's own pipeline estimate. A wrong estimate is therefore invisible in `err` and shows up only as a constant offset against another speaker.
   - 2026-09-28: David once heard the ESP clearly out of sync with an Apple TV after switching songs. The likely cause was the cursor reset on pause/resume described in §5, fixed in the `output-cursor-fix` PR. Confirm with the microphone test.
2. Multi-select this device + a **HomePod** on the iPhone. Measure the offset: record both with one phone mic and cross-correlate a click track (e.g. Audacity), or listen for flanging with both close together. Set `output_delay` to the TOSLINK receiver's/AVR's latency; many AVRs have a known "audio delay" in their menus.
3. Optional refinement: extrapolate `played` between DMA callbacks (TODO in `audio_output_get_pipeline_us`).
4. Two ESP32 devices + HomePod multi-selected, ~1 h. The servo should hold them without audible drift.

**Done when:** it's within a few ms of a HomePod by ear with both side by side, and stays there over an hour.

### M3: robustness
- **Flush** (seek/skip/pause, `audio_output_flush`). We can't drop audio already queued in ESPHome's speaker chain. Options:
  - (a) accept stale audio; the timing engine drops the late frames that follow;
  - (b) on flush, set state IDLE→PLAYING so the player calls `speaker->stop()`, but check that's not visible as a stop in HA/MA;
  - (c) keep the chain shallow; the resampler/mixer `buffer_duration` is configurable.
  Measure first, pick after.
- **Pause:** currently PAUSED + output inactive. Compare that with "keep PLAYING, write silence" for resume latency.
- **Sendspin takes over mid-AirPlay:** STOP → `airplay_core_disconnect_client()`. Check the iPhone's UI behaviour. Replace stop+start with a real disconnect: add `rtsp_server_disconnect_clients()` as a small upstream PR, or as a wrapper that is still compiled from upstream.
- **AirPlay takes over mid-Sendspin:** request_play_uri → the player stops Sendspin. Check that MA shows the player as busy/external and doesn't fight back.
- **Announcements** (HA TTS via the announcement pipeline) during AirPlay: the mixer ducks. Sync is lost while ducking, but it must recover.
- **Network:** Ethernet unplug/replug, DHCP renew, and HA API reconnect. Do mDNS and the sockets survive? Restart the core if not (`TODO(M4)` in `loop()`).
- A second iPhone taking over the session.

### M4: polish
- Metadata as `text_sensor` (title/artist/album), like Sendspin's text sensors. Copy the struct under a mutex in `on_core_event_` and publish in `loop()`.
- HA volume → iPhone: only possible for AirPlay 1 via DACP (`dacp_send_volume`). Document the AirPlay 2 limitation.
- `airplay_1_only` path tested (DACP remote buttons).
- Optional: HomeKit / Home app add (upstream supports non-transient pairing; untested here).
- README with a user-facing config; a CI job running `esphome config` (and `compile` if a runner has the toolchain cached).

---

## 9. Risks and open questions

| # | Risk | Notes / mitigation |
|---|---|---|
| 1 | **libsodium duplicate** if API encryption is enabled | Remove `espressif/libsodium` from `airplay_core/idf_component.yml` and rely on ESPHome's; both are 1.0.21. Not an issue for the example config. |
| 2 | **CPU starvation**: playback task at priority 9 | Upstream needs it above its receiver tasks. ESPHome's loop runs at priority 1, and i2s/mixer/resampler tasks have their own priorities. Watch for ESPHome loop lag and task WDT. The task mostly blocks in `write_output`. If needed, lower all of airplay's tasks together rather than just this one. |
| 3 | **Memory** | PSRAM is not the problem; upstream's jitter buffer asks for PSRAM explicitly. **Internal RAM is.** First hardware run (Wi-Fi, speaker chain started): ~62 KB free, 31 KB largest block. Upstream expects every `malloc` above 1 KB to go to PSRAM, but under ESPHome they all go to internal RAM (§5). The codec is redirected by `codec_alloc_psram.c`. If other upstream allocations fail (pairing, plist, the realtime path), options are: redirect more of them the same way; set `CONFIG_SPIRAM_USE_MALLOC` + `ALWAYSINTERNAL=1024` globally (changes ESPHome's behaviour, test Sendspin too); or shrink the speaker chain. |
| 4 | **Flush / stale audio** | See M3. |
| 5 | **Two sources of truth for volume** | The iPhone slider, the HA slider, and the player's `volume_min/max` (0.4–0.9 in this config). Mapping is linear dB→0..1, then the player's range. It may feel odd; tune in M4. |
| 6 | **`esp_audio_codec` licence**: binary-only, "exclusively with Espressif products" | Fine for personal builds. It conflicts with plain GPL for *distributed binaries*, which is why upstream's GPL PR adds a linking exception. Don't publish binaries until that's sorted. |
| 7 | **Upstream licence** | Currently non-commercial. PR #162 → GPL-3.0-or-later + exception (not merged as of 2026-09-28). Personal use and a public non-commercial repo are fine now. Pick our own licence (GPL-3.0-or-later is the natural fit) once #162 lands. |
| 8 | **ESPHome API churn** | `media_source` and Sendspin are new and marked experimental. Pin the ESPHome version you build with, and re-check §5 on upgrades. |
| 9 | **Sockets estimate** | Verify with lwIP stats in M1; `media_source.py` reserves TCP 5 / UDP 6 / listen 3. |
| 10 | **`output_delay` on this TOSLINK chain** | Unknown until measured (M2). |
| 11 | **Stale PTP lock at AirPlay 1 start** | Seen 2026-09-28. An AirPlay 2 client (192.168.1.39) connected and left. `ptp_clock` then reported LOCKED with an absurd offset. The next AirPlay 1 session's first anchors used PTP (`ptp_locked=1`, frames "13 years early") until the lock dropped after ~6 s and NTP took over. This is upstream behaviour, harmless once NTP takes over, but it may delay the start of AirPlay 1 playback. Revisit in M3 if it's audible. |

---

## 10. Decisions log

| Decision | Alternatives considered | Reason |
|---|---|---|
| Receiver on the device, not in MA | MA dedup; MA timestamp bridge | Native iOS multi-select + HomePod sync with zero server plumbing |
| Use airplay-esp32, not a fresh port of shairport-sync | shairport-sync + NQPTP port; openairplay Python receiver | ESP-IDF native, already ESP32-S3 tuned, has PTP + timing servo + pluggable output |
| External component, no upstream PRs | Upstream ESPHome component | David's constraint; the ESPHome media-source API has enough hooks |
| Core as a local IDF component via `add_idf_component(path=…)` | Flatten all C into `components/airplay/` | Loader copies only top-level files; keeps upstream layout + include paths intact |
| Vendor upstream (script) | git submodule | Avoids nested submodules on every device build; exact pin |
| New output backend + `get_pipeline_us` from `notify_audio_played` | Port Sendspin's sync task; patch the ESPHome speaker API | Upstream's servo already does the work; the ESPHome feedback hook exists; zero upstream edits |
| Replace `mdns_airplay.c` instead of patching | Patch upstream | Upstream must stay unmodified; the file is small |
| `has_internal_playlist() = true` | false | Same as Sendspin: the player must not advance its own playlist when we go idle |
| STOP ⇒ drop the AirPlay session | Keep the session, mute | Otherwise the iPhone streams into nothing and thinks it's playing |

---

## 11. Working in this repo

- **Never edit `airplay_core/upstream/`.** Glue goes in `airplay_core/src/`, ESPHome code in `components/airplay/`. Re-vendor with `scripts/sync-upstream.sh <commit>`, then diff upstream `network/mdns_airplay.c` against `src/mdns_airplay_esphome.c`.
- Build and test:
  ```bash
  pip install esphome==2026.9.0        # or the version you pin; Python ≥ 3.12
  cd examples && cp secrets.yaml.example secrets.yaml   # fill in
  esphome config living-room-sendspin-airplay.yaml
  esphome compile living-room-sendspin-airplay.yaml
  esphome run living-room-sendspin-airplay.yaml         # USB first time, OTA after
  esphome logs living-room-sendspin-airplay.yaml
  ```
- The example config uses the component from the local checkout (`type: local, path: ../components`). When switching to `github://…@ref`, the whole repo (including `airplay_core/`) must be in that repo.
- **ESPHome Builder (Home Assistant add-on)** can't see a local checkout. Paste the example with `external_components` switched to `github://davidanthoff/esphome-airplay@<ref>`, `components: [airplay]`, `refresh: 0s`. That exact variant was compiled on 2026.9.0, and `AIRPLAY_CORE_DIR` resolves inside the cloned repo. Use an add-on on 2026.9.x.
- Logs: the ESPHome C++ side (`airplay.media_source`) follows `logger:`. The C core and upstream (`airplay_core`, `airplay_out`, `rtsp_handlers`, `audio_timing`, …) show up under tag `esp-idf` and are gated by `esp32: framework: log_level:` (§5).
- The device is David's living-room Sendspin speaker, so keep a known-good firmware to fall back to. The config without the `esphome-airplay` bits is his current one.

---

## 12. References

- airplay-esp32: https://github.com/rbouteiller/airplay-esp32 (its `CLAUDE.md` has a good architecture overview), relicensing PR https://github.com/rbouteiller/airplay-esp32/pull/162
- ESPHome media source / speaker source / Sendspin docs: https://esphome.io/components/media_source/ · https://esphome.io/components/media_player/speaker_source/ · https://esphome.io/components/sendspin/
- ESPHome source (read these first): `esphome/components/media_source/media_source.h`, `speaker_source/speaker_source_media_player.cpp`, `sendspin/media_source/sendspin_media_source.{h,cpp}`, `audio_http/media_source.py`
- sendspin-cpp (Apache-2.0; how Sendspin uses `notify_audio_played`: hard/soft sync, frame insert/drop): https://github.com/Sendspin/sendspin-cpp, `src/sync_task.cpp`
- shairport-sync / NQPTP (background, AirPlay 2 timing): https://github.com/mikebrady/shairport-sync · https://github.com/mikebrady/nqptp
- MA AirPlay Receiver plugin (classic shairport-sync): https://www.music-assistant.io/plugins/airplay-receiver/ and `music_assistant/providers/airplay_receiver/` in music-assistant/server
