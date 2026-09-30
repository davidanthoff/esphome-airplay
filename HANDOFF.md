# esphome-airplay: handoff

**Goal:** make a cheap ESP32 Sendspin speaker also show up as a **native AirPlay 2 receiver** on the iPhone. It should be multi-selectable and play in sync with HomePods and other AirPlay 2 speakers, while staying a normal Sendspin player for Music Assistant. It lives entirely in a repo David controls, as an ESPHome **external component**. No upstream PRs to ESPHome are needed.

**State on 2026-09-28:** running on David's living-room board, installed from ESPHome Builder with the `github://` source.
- **M1 done:**
  - AirPlay 2 (buffered AAC) plays, and so does AirPlay 1 (realtime ALAC).
  - Sendspin still works, and switching between Music Assistant and AirPlay works in both directions.
  - A 45-minute session ran with no gaps.
- **M2 sync verified against an Apple TV:** −1.5 ms offset and no drift, measured with `tools/click-test` (§8).
- **Still open:** two-board sync (M2.4), M3 robustness, M4 polish.
- **Upstream moved to `staging` @ `764ffb6` (2026-09-29):** GPL-3.0-or-later and the new "engine v2" timing engine (§6). The M1/M2 results above were measured on the previous pin (release v0.2.1, `811d5f8`) and must be re-checked on engine v2 (§8).

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
- **Second device:** Seeed Studio **XIAO ESP32S3** (8 MB flash, 8 MB octal PSRAM, **WiFi only**, U.FL antenna), TOSLINK from D3 (GPIO4), node `dining-room-speakers`. Config: [`examples/dining-room-speakers.yaml`](examples/dining-room-speakers.yaml). It is the living-room config with the board-specific parts changed.

---

## 3. Architecture

```
 iPhone ──RTSP/HAP/FairPlay──► ┌──────────────── airplay_core (ESP-IDF component, C) ─────────────────┐
        ──PTP (319/320)──────► │ upstream/ (airplay-esp32 staging @ 764ffb6, unmodified)            │
        ──audio (UDP/TCP)────► │   rtsp/ hap/ plist/ ptp_clock  audio_receiver → decoder           │
                               │   engine v2: RTP timeline → scheduler (drift servo)               │
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
3. **Volume.** Upstream calls `dac_set_volume(dB)` through a registered `dac_ops_t`. We register ops that forward to the host.
   - The host **only applies the sender's volume while AirPlay is the active source** (PLAYING/PAUSED). A value that arrives earlier is kept and applied when the session starts.
   - **Why:** senders push their stored volume as soon as they *connect*, even without playing. On 2026-09-28 both the Apple TV and Music Assistant's AirPlay provider (which probes the receiver) set the speaker to 0% while Sendspin was playing.
4. **Session state.** `playback_events_register()` gives CONNECTED/PLAYING/PAUSED/DISCONNECTED/METADATA (§6: transitions only).

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
│   ├── idf_component.yml          espressif/mdns, esp_audio_codec (libsodium: see airplay_sodium/)
│   ├── Kconfig                    upstream symbol names (ENABLE_AIRPLAY_ARTWORK)
│   ├── UPSTREAM.md                pinned commit + licence notes
│   ├── include/airplay_core.h     the C API between core and ESPHome
│   ├── src/                       our glue (see §3)
│   └── upstream/                  vendored airplay-esp32 (main/, components/dac/, LICENSE*) – never edit
├── airplay_sodium/                ESP-IDF component: libsodium modules ESPHome's port leaves out (§5, noise)
├── examples/
│   ├── living-room-sendspin-airplay.yaml
│   ├── dining-room-speakers.yaml   XIAO ESP32S3, WiFi only
│   └── secrets.yaml.example
├── scripts/sync-upstream.sh       re-vendor upstream at a (staging) commit
└── tools/click-test/              measure offset/drift vs another AirPlay speaker (one mic)
```

Why vendoring and not a git submodule: ESPHome clones git external components with `--depth=1` and does **not** initialise submodules. `external_components` calls `git.clone_or_update()` without `init_submodules`, which defaults to false (checked in 2026.9.0's `esphome/git.py`). A submodule in this repo would arrive empty on every device build. Vendoring the ~1 MB we need also pins exactly what we compile.

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
- **With `api: encryption:`,** ESPHome adds `esphome/libsodium 1.10021.11`, a port of libsodium 1.0.21 that shows up in the build as a component named **`libsodium`**.
  - **It can't coexist with `espressif/libsodium`.** CMake fails with "Requirement espressif__libsodium and requirement libsodium are both added as project_managed_components. Can't decide which one to pick." This broke David's ESPHome Builder install on 2026-09-28: his Home Assistant YAML has API encryption, the example didn't.
  - **The port ships the full libsodium source tree but compiles only what noise needs** (a `srcFilter` in its `library.json`): ChaCha20-Poly1305, X25519, SHA-256, Salsa/ChaCha streams, codecs. It has **no** SHA-512, HMAC-SHA512, Ed25519 signing or `crypto_box`, all of which HAP pairing needs.
  - **Our solution:**
    - `media_source.py` always adds `esphome/libsodium` (same version as noise, so they dedupe).
    - `airplay_core/idf_component.yml` no longer lists `espressif/libsodium`.
    - **`airplay_sodium/`** compiles exactly the missing modules from the port's own source tree, found via `idf_component_get_property(… libsodium COMPONENT_DIR)`. The port's include paths and `-DCONFIGURED=1` are public.
    - Verified by building with and without API encryption: one libsodium, each `crypto_*` symbol defined once, firmware ~50 KB smaller.
  - **Random numbers (a crash found on hardware 2026-09-28):**
    - The port keeps libsodium's default randombytes backend, `sysrandom`, which reads **`/dev/urandom`**. `getrandom()` isn't enabled in the port, and ESP-IDF has no `/dev/urandom`.
    - So the first randombytes call, already inside `sodium_init()` (called from upstream `hap_init()`), makes libsodium **abort on purpose** (`sodium_misuse()`).
    - noise-c never hits this because it has its own RNG. `espressif/libsodium` avoids it with `port/randombytes_esp32.c` as its compiled-in default.
    - **Symptom:** every OTA of the PR #7/#8 firmware panicked a few seconds after boot, and the bootloader rolled it back. The log showed `OTA rollback detected! Rolled back from partition 'app1'`, `Reset Reason: exception/panic`, and the old "compiled on" timestamp. ESPHome Builder kept offering the update.
    - **Fix:** `airplay_sodium/randombytes_esp32.c` (hardware RNG via `esp_random` / `esp_fill_random`), installed with `randombytes_set_implementation()` in `airplay_core_init()`, before `sodium_init()`.
    - The rest of `sodium_init()` is fine on this port: it has stubs for the argon2/blake2b/aegis "pick best implementation" calls, and the alloc init only needs randombytes.
  - **Re-check the version pin when upgrading ESPHome.**

**Framework**
- Only ESP-IDF. The component is `only_on_esp32`, and upstream needs IDF ≥ 5.5.

**Build toolchain (`espidf/toolchain.py`, `espidf/framework.py`)**
- 2026.9.0 builds with a native ESP-IDF 5.5.5 install, not PlatformIO. It lives in `~/.cache/esphome/idf` (framework, xtensa toolchain, and a Python env in `penvs/`).
- ESPHome creates that Python env with `<its own python> -m venv` (`framework_helpers.create_venv`, interpreter from `$PYTHONEXEPATH` or `sys.executable`). Ubuntu's system Python can't do that without the `python3-venv` apt package. A `uv`-managed CPython (`uv venv --python-preference only-managed`) avoids the need for sudo.

**`psram/__init__.py`: where plain `malloc` goes**
- ESPHome builds with `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` and **not** `CONFIG_SPIRAM_USE_MALLOC`. Plain `malloc`/`calloc` only ever return internal RAM. ESPHome code gets PSRAM explicitly (`RAMAllocator`, `heap_caps_*`).
- Upstream airplay-esp32 assumes the opposite. Its `config/sdkconfig.defaults` has `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` (every `malloc` above 1 KB goes to PSRAM) and `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y`.
- Consequence, seen on hardware: `esp_audio_codec`'s AAC decoder couldn't open ("There is no enough memory for AAC buffer"), so AirPlay played silence. Internal RAM was ~60 KB free with a largest block of 31 KB once the speaker chain was running. Fixed by `src/codec_alloc_psram.c`, which overrides the codec's weak `media_lib_module_malloc/calloc` with upstream's 1 KB rule. Upstream's own plain `malloc`s still go to internal RAM (§9).

**`sendspin/sendspin_hub.cpp`: the Sendspin client id is a MAC**
- `get_client_id_into_buffer()` returns the **Ethernet MAC whenever `ethernet:` is configured** (on the ESP32-S3 that's the base MAC + 3), otherwise the base MAC.
- The code comment says the server "matches client_id against the L2 source MAC of the device's multicast traffic". On a board with `ethernet:` configured that is running on Wi-Fi, even ESPHome's own id doesn't match the traffic.
- David's board: Sendspin reports `…:B5:77` (Ethernet), while upstream AirPlay reported `…:B5:74` (Wi-Fi STA).

**Music Assistant: how it sees this device** (checked 2026-09-28 in music-assistant/server `main`)
- **Merging:** protocol players of one physical device (AirPlay, Chromecast, DLNA, Sendspin, Squeezelite) are merged into a "Universal Player". It matches by MAC first, then UUID, AirPlay or Cast id, and IP only as a last resort. Players of the *same* protocol are never merged. (`controllers/players/README.md`)
- **Before the device-MAC change:** because of the MAC mismatch above, David's Music Assistant showed **two players**:
  1. "Sendspin TOSLINK, Apple / HomePod Mini": our AirPlay receiver, discovered by Music Assistant's AirPlay provider, plus **"Sendspin (over AirPlay)"**, which is Music Assistant's own Sendspin *bridge* that plays to AirPlay devices.
  2. "Sendspin TOSLINK 54b574": the ESP's native Sendspin.
- **Preferred output protocol:** Music Assistant defaulted the merged player to **AirPlay**. With AirPlay and native Sendspin merged, set it to **Sendspin**. Otherwise Music Assistant plays to the ESP *via AirPlay*.
- **External sources:** Music Assistant only surfaces them (state, metadata, forwarded pause/next/seek) from **Chromecast and DLNA** protocol players, and only for sources in a fixed list of streaming services (`EXTERNAL_SOURCE_PROTOCOLS`, `EXTERNAL_SOURCES` in `models/protocol_backed_player.py` and `constants.py`). Sendspin's `external_source` and AirPlay aren't among them.

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

## 6. Verified facts about upstream (airplay-esp32 `staging` @ `764ffb6`, 2026-09-29)

- **Branches:** upstream takes every PR on `staging` and moves `main` only when it tags a release (`CONTRIBUTING.md`). From the v0.2.1 release (`811d5f8`, our previous pin, 2026-09-20) to `764ffb6`, `main` got only docs and release commits. We pin a `staging` commit (`airplay_core/UPSTREAM.md`).
- **Bring-up** (`main.c: start_airplay_services`): `ptp_clock_init → hap_init → audio_receiver_init → audio_output_init → mdns_airplay_init → audio_output_start → rtsp_server_start → playback_control_set_source(AIRPLAY)`. Before that, `settings_init` and `playback_control_init` run. Mirrored in `src/airplay_core.c`. Unchanged from `811d5f8`.
- **Compiled set:** listed in `airplay_core/CMakeLists.txt`. It references only `audio_output_*` from the output API, so there are no dependencies on led/wifi/display/web code. Checked by grep.
  - **Upstream's own Sendspin player, USB audio and Bluetooth** (`sendspin/`, `usb/`, `audio/usb_audio_sink.c`, `audio/a2dp_sink.c`) aren't compiled. Every call into them from the compiled files is behind `CONFIG_SENDSPIN_ENABLE` / `CONFIG_USB_AUDIO_SINK` / `CONFIG_BT_A2DP_ENABLE`, which stay undefined.
  - The compiled set calls the same `crypto_*`/`sodium_*` functions as at `811d5f8`, so `airplay_sodium/` is unchanged.
- **Replaced:** `main.c`, `network/mdns_airplay.c` (it calls `mdns_init`), `audio/audio_output*.c`, `audio/audio_resample.c` (ESPHome's resampler speaker does this job).
- **Device identity:** upstream uses `esp_read_mac(ESP_MAC_WIFI_STA)` everywhere: `rtsp_handlers.c` (twice), `hap_pair_verify.c`, `settings.c` and `mdns_airplay.c`.
  - **In our build,** those calls are redirected (`CMakeLists.txt`: `esp_read_mac=airplay_core_read_mac` for upstream sources only, implemented in `src/device_mac.c`). They return the **same MAC ESPHome's Sendspin hub reports**, so Music Assistant can merge both into one player (see §5, "Music Assistant").
  - Other MAC types, and all non-upstream code, still get the real values. `nm` confirms that only `device_mac.c.obj` references `esp_read_mac` directly.
  - **Changing the identity** makes iOS and Music Assistant see a "new" AirPlay device once: the old entry (e.g. Music Assistant's `ap28848554b574`) goes stale and can be removed.
- **Advertised model:** upstream advertises `AudioAccessory5,1`, the **HomePod mini** identifier, for the iOS speaker icon (`AirPort4,107` in AirPlay 1 mode). It is also hard-coded in its `/info` reply.
  - **What that did to Music Assistant** (`providers/airplay/helpers.py`): it maps the code to "Apple / HomePod Mini", and `is_apple_device()` makes it a **native** player (`PlayerType.PLAYER`), with Sendspin attached as a protocol.
  - **The consequence:** for volume and mute, Music Assistant prefers the native player *"even while a protocol renders the audio"*, so **every Music Assistant volume change went to its AirPlay provider** while Sendspin played, and never reached the device (2026-09-29).
  - **Our mDNS now advertises `model` = the YAML `model:` option,** default `esphome-airplay`, like shairport-sync's `ShairportSync`.
  - **Plus `manufacturer=ESPHome`.** This is the part that fixes Music Assistant. `get_model_info()` returns manufacturer and model verbatim when both TXT keys are present. `is_apple_device()` needs the manufacturer to start with "apple", so the receiver is a plain AirPlay protocol player even with `model: AudioAccessory5,1`. Checked against MA `dev`, 2026-09-29.
  - **Untested:** the `/info` model stays upstream's, and whether iOS behaves differently with a non-Apple model beyond the icon.
- **Persistence:** NVS namespace `"airplay"` holds the device name, volume, HAP pairing keys and so on. It doesn't collide with ESPHome, which already ran `nvs_flash_init`. `rtsp_handlers.c` `/info` reads the name from settings, so `airplay_core_init()` writes the configured name there when it changed.
- **Output timing contract:** `FRAME_SAMPLES = 352`, also engine v2's render block. The playback task priority `AUDIO_PLAYBACK_TASK_PRIORITY = 9` must outrank the receiver tasks (8/7/5, and engine v2's `audio_decode` task at 6, pinned to core 0), otherwise playback drifts late (upstream issue #122). **Task stacks must stay in internal RAM** (`spiram_task.h`: flash ops disable the cache). The decode task adds a 6 KB internal stack.
- **Engine v2** (`audio_engine_v2.c`, `audio_timeline.c`, `audio_scheduler.c`, `audio_clock_map.c`, `audio_epoch.c`, `audio_decode_worker.c`) replaces the old `audio_timing` position servo and its early/late thresholds. `audio_timing.c` is still compiled, for anchors and latency bookkeeping.
  - **The output backend still pulls.** The playback task calls `audio_output_read_source()`, which is `audio_receiver_read()` unless upstream's Sendspin installs another source.
  - **Each render is placed on the RTP timeline** at `audio_output_get_next_playout_time_ns()`: `get_pipeline_us()` plus a fixed 5 ms (`OUTPUT_PIPELINE_LATENCY_US`, `audio_output_common.c`, not weak). That's the same 5 ms that `src/audio_output_esphome.c` subtracts (`UPSTREAM_PIPELINE_LATENCY_US`, PR #5).
  - **Drift servo:** `audio_scheduler.c` drops or repeats single samples at the quietest point of a block. The rate is proportional to the filtered error, with a ~5 s time constant and ~2 s of warm-up after each start. Upstream measured it parking within ~5 samples. The old engine trimmed ~166 samples every few minutes (§8 M2).
  - **Memory:** the PCM timeline (~790 KB) and the decode jobs are allocated explicitly in PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`), once and never freed.
  - **Status log:** tag `audio_v2`, a `playout: raw=… filt=… drift=… ppm trims=…/s buffered=… concealed=…` line, plus `start decision:` at each start.
  - The `CONFIG_AIRPLAY_*TIMING_THRESHOLD_MS` symbols are still declared upstream, but nothing reads them. The `timing_threshold` YAML options were removed.
  - The `rtsp_handlers.c` comment still holds: **do not** advertise output latency to the sender, because the engine compensates internally and advertising would apply it twice.
- **Session events** (`playback_events.c`, replaces `rtsp/rtsp_events.c`) are an aggregate over upstream's inputs, and listeners get **transitions only**:
  - repeats are dropped;
  - PLAYING from idle arrives as CONNECTED then PLAYING;
  - PAUSED from idle arrives as CONNECTED.

  Every way an RTSP client goes away emits DISCONNECTED (`rtsp_server.c`), including our stop+start in `airplay_core_disconnect_client()`, so the aggregate resets for the next session.
- **AirPlay 1 vs 2** is a runtime setting, `settings_airplay_v1()`, stored in NVS. The build option `CONFIG_AIRPLAY_FORCE_V1` is gone.
  - It is applied only inside `settings_init()` (a set only stores it), so `airplay_core_init()` stores the YAML value when it differs and calls `settings_init()` again.
  - **In v1 mode the RTSP server listens on port 5000**, not 7000 (`airplay_rtsp_port()`), and the classic `_raop` TXT follows shairport-sync's classic record (`vn=65537`, `da`, `fv`, `pw`, `sf`, …); `src/mdns_airplay_esphome.c` follows both.
- **Remote control:** play/pause/next from the device works only over **DACP (AirPlay 1)**. AirPlay 2 needs MRP, which isn't implemented, so HA→iPhone transport and volume sync won't work for AirPlay 2 sessions.
- **Stopping a session:** client slots are static in `rtsp_server.c`, and there's no public "disconnect". We currently do `rtsp_server_stop()` + `rtsp_server_start()`.
  - **AirPlay 2 senders** notice and let go.
  - **AirPlay 1 senders don't:** they stream over UDP, and the iPhone kept showing "playing" after a Sendspin takeover (2026-09-29). So for a *playing* AirPlay 1 session, `airplay_core_disconnect_client()` first sends DACP `playpause`. The iPhone then pauses and tears the session down itself. The server restart is kept as a fallback after 3 s.
  - Upstream's DACP client only has the `playpause` toggle, hence "only while playing". Its worker skips requests once the session is cleared, hence waiting in a separate task instead of stopping right away.
- **AirPlay 1 pause/resume is slow** (2026-09-29, `764ffb6`):
  - The iPhone pauses an AirPlay 1 session with FLUSH + TEARDOWN and resumes with a full reconnect.
  - Each reconnect restarts upstream's NTP timing client. It polls every 3 s and locks after 3 replies (`ntp_clock.c`: `TIMING_INTERVAL_MS`, `MIN_MEASUREMENTS`), and engine v2 starts only once locked. So resume takes ~6 s: RECORD → NTP locked → `start decision` measured at +5.7 s and +5.8 s. The first start is the same.
  - The offset measured on each reconnect was identical, which it would be for the same sender.
  - A possible upstream fix: burst the first timing requests, or keep the offset for a client that reconnects within the DACP grace period. Not pursued; AirPlay 1 is low priority.
- **Licence:** GPL-3.0-or-later plus `LICENSE-EXCEPTION`, which permits linking Espressif's binary-only components such as `esp_audio_codec` (PR #162, on `staging` since 2026-09-20).

---

## 7. What's done / status per file

| File | State |
|---|---|
| `components/airplay/media_source.py` | ✅ `esphome config` passes. `airplay_1_only` is now passed at runtime; `timing_threshold` / `realtime_timing_threshold` fail validation with a "removed" message (2026-09-29) |
| `components/airplay/airplay_media_source.{h,cpp}` | ✅ on hardware (M1) at `811d5f8`. Compiles and links against `764ffb6`; not yet run on it |
| `airplay_core/include/airplay_core.h` | ✅ compiles as C and C++ in the firmware build. New field `airplay_v1` |
| `airplay_core/src/airplay_core.c` | ✅ on hardware at `764ffb6` (AirPlay 2 and AirPlay 1, 2026-09-29). 🟡 The DACP pause before dropping an AirPlay 1 session (§6) compiles; not yet on hardware |
| `airplay_core/src/audio_output_esphome.c` | 🟡 M2 verified vs an Apple TV at `811d5f8`. Reads via `audio_output_read_source()` now; re-measure on engine v2 (§8 M2) |
| `airplay_core/src/mdns_airplay_esphome.c` | 🟡 re-derived from upstream `764ffb6`: `airplay_features()`, `airplay_rtsp_port()` (5000 in v1 mode), new classic TXT set. Compiles; not yet on hardware |
| `airplay_core/src/device_mac.c` | ✅ on hardware: AirPlay device id = the Sendspin hub's MAC (§6) |
| `airplay_core/src/codec_alloc_psram.c` | ✅ on hardware: AAC plays (§5) |
| `airplay_core/CMakeLists.txt`, `idf_component.yml`, `Kconfig` | ✅ resolve and build on 2026.9.0 with upstream `764ffb6`: 7 engine-v2 / event sources added, `rtsp_events.c` dropped, `esp_app_format` required |
| `airplay_core/upstream/` | ✅ `staging` @ `764ffb6`, vendored by `scripts/sync-upstream.sh`. All 41 listed files compile without warnings |
| `examples/living-room-sendspin-airplay.yaml` | ✅ compiles on 2026.9.0 (1.59 MB image, RAM 37.7%). Has debug sensors (heap/PSRAM) and IDF `log_level: INFO` |
| `examples/dining-room-speakers.yaml` | ✅ compiles on 2026.9.0 (1.54 MB image of a 3.75 MB app partition, RAM 37.2%). Not yet run on hardware |

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
- **Re-check after the move to upstream `staging` (`764ffb6`):**
  - AirPlay 2 and AirPlay 1 (now on port 5000) both play;
  - switching between Sendspin and AirPlay works in both directions;
  - internal heap and largest block while streaming AAC: engine v2 adds a 6 KB task stack and ~7 KB of timeline descriptors in internal RAM.

### M2: sync
1. Log `audio_output_get_pipeline_us()` and the timing engine's servo stats once a second. Check that pipeline_us is stable and plausible, i.e. roughly the resampler+mixer+DMA buffering.
   - Up to `811d5f8`, upstream logged `audio_time: Playout: err=… depth=…` once a second and the servo engage/disengage lines. Engine v2 (since `764ffb6`) logs `audio_v2: playout: raw=… filt=… drift=… ppm trims=…/s …` instead (§6). `airplay_out` logs `pipeline=… ms submitted=… played=…` every 10 s.
   - Next to it, `airplay_core` logs `clock: master=… locked=… syncs=+N followups=+N rate=±x ppm raw-filt=… us …`: the PTP clock the engine follows, as changes since the previous line (`airplay_core_log_clock_status()`).
     - While playing, `syncs=` should be about +80 per 10 s (PTP sends 8 SYNCs a second).
     - `rate=` is the ESP crystal against the master: a steady value of tens of ppm; negative means the ESP runs fast.
     - `syncs=+0` together with `rate=+0.0` means the offset is frozen, and the ESP plays on its own crystal.
   - `err` is measured against the engine's own pipeline estimate. A wrong estimate is therefore invisible in `err` and shows up only as a constant offset against another speaker.
   - 2026-09-28: David once heard the ESP clearly out of sync with an Apple TV after switching songs. The likely cause was the cursor reset on pause/resume described in §5, fixed in the `output-cursor-fix` PR. Confirm with the microphone test.
2. Multi-select this device + a **HomePod** on the iPhone. Measure the offset: record both with one phone mic and cross-correlate a click track (e.g. Audacity), or listen for flanging with both close together. Set `output_delay` to the TOSLINK receiver's/AVR's latency; many AVRs have a known "audio delay" in their menus.
   - **Measured 2026-09-28** against an **Apple TV**, which has no HomePod; its Wireless Audio Sync was calibrated first. Setup: 60 BPM metronome from Apple Music, one phone recording 23 min, ESP 3 ft and Apple TV speaker 11 ft from the mic, ESP amp muted for the last ~7 beats to identify the clicks.
   - **Method:** Apple-TV-only click template from the muted beats. In every beat, locate and subtract the Apple TV click, then locate the ESP click in the residual (band-passed cross-correlation, 1.5–8 kHz).
   - **Drift:** none. −0.4 ms/hour over 23 min, and every 1-minute median was within ±0.5 ms of the mean. The ESP crystal runs ~25 ppm fast; the position servo corrects it about once a minute, a ~2 ms peak-to-peak sawtooth.
   - **Offset:** ESP −21.3 ms at the mic, **−14.2 ms** after the 7.1 ms distance correction, i.e. the ESP played early. Cause: our pipeline estimate read too long. ESPHome's S/PDIF path reports played frames only every 4 DMA blocks (~17.4 ms), about 8.7 ms on average; upstream also adds a 5 ms `PIPELINE_LATENCY_US` that our measured queue already contains. Fixed in the `sync-offset-fix` PR (#5), which extrapolates `played` between reports (item 3) and subtracts the 5 ms. (`tools/click-test` gives −13.7 ms for this same recording; its reference template uses 5 instead of 7 beats.)
   - **Re-measured after PR #5, 2026-09-28** (same setup, 10.8 min): **−1.5 ms**, which is within the measurement error (±0.5 ft on a distance is ±0.45 ms). Drift +0.6 ms/hour. The ESP moves in a clean sawtooth between −3.5 and +0.3 ms: its crystal gets ~1.5 ms/min ahead, and every ~2.6 min the servo trims ~166 samples (3.8 ms), matching the four servo events in the log. `output_delay` stays at 0.
   - **Long run:** 45 minutes of video audio from an Apple TV to the ESP alone, logged with the `esphome logs` loop (§11): 2,828 one-per-second Playout reports, `gaps=0 under=0`, `err` −2…+5 ms, pipeline estimate 658–696 ms, free internal heap steady at ~54 KB. Earlier reports of dropouts in a 1.5 h session (not logged) did not reproduce.
   - **Tool:** `tools/click-test/` has the analysis script and the recording procedure.
3. ✅ Extrapolate `played` between DMA callbacks: done in `audio_output_get_pipeline_us`, clamped to 25 ms. `output_delay` is now signed (−200…500 ms) for final fine-tuning.
4. Two ESP32 devices + HomePod multi-selected, ~1 h. The servo should hold them without audible drift.

**Done when:** it's within a few ms of a HomePod by ear with both side by side, and stays there over an hour.
- **Status 2026-09-28:** met against an **Apple TV**: −1.5 ms, no drift over 23 + 11 minutes of click tests, plus a clean 45-minute session. All of this was on the old upstream engine (`811d5f8`).
- **After the move to engine v2 (`764ffb6`):** repeat the click test (item 2) and a long logged session. Expect the same offset, since the 5 ms compensation carries over (§6), and a much smaller sawtooth: single-sample trims instead of ~166-sample steps. Check that `audio_v2 … filt=` settles within ~1 ms and `trims=` stays around 1/s.
- **First engine-v2 click test, 2026-09-29: drift, probably not engine v2's fault.**
  - **Setup:** the sender was an *old iPhone* (192.168.1.75), grouped with the Apple TV. Its announced PTP master was `84ab1a79ffb90008`. Every earlier session used `38e13d76…` (David's iPhone) or `c0956d82…` (yesterday's test).
  - **Result:** over the ~7 min before the session was cut, the ESP drifted in a straight line from +3.0 to −4.9 ms against the Apple TV (distance-corrected, 3/11 ft): **−64 ms/hour ≈ 18 ppm**, with no corrections.
  - **What the engine saw:** `filt` within ±0.05 ms, estimated `drift` ~0 ppm, and 2 trims in 8 min (vs ~0.5/s with David's iPhone).
  - **So the engine was not following the group clock.**
  - **Suspected cause:** PTP locked on only 8 samples (`dev=0ns`), then three `SETPEERS` ("clock will re-lock") arrived, and no PTP line followed for the rest of the session.
  - `ptp_clock.c` discards SYNC/FOLLOW_UP from any clock but the expected master. If the group's grandmaster changed with the peers (e.g. to the Apple TV) and nothing re-announced it, the offset froze. The ESP then ran on its own crystal (~18–25 ppm fast, cf. the 2026-09-28 measurement).
  - The asymmetric offset filter alone can't explain it: with the current constants, 18 ppm leaves a constant ~0.6 ms lag, not a growing error.
  - **Next:** the `clock:` diagnostics line (item 1) shows it directly. Repeat with David's iPhone (expect no drift) and with the old iPhone (expect `syncs=+0`, `rate=+0.0`).
  - The analysis needed an explicit split, because the ESP dropped out mid-recording instead of being muted at the end (the session was cut; see §9 #13). `analyze.py` expects the reference-only beats at the end.
- **Still open:** item 4, two ESP speakers plus the reference for about an hour. This needs a second board.

### M3: robustness
- **Flush** (seek/skip/pause, `audio_output_flush`). We can't drop audio already queued in ESPHome's speaker chain. Options:
  - (a) accept stale audio; the timing engine drops the late frames that follow;
  - (b) on flush, set state IDLE→PLAYING so the player calls `speaker->stop()`, but check that's not visible as a stop in HA/MA;
  - (c) keep the chain shallow; the resampler/mixer `buffer_duration` is configurable.
  Measure first, pick after.
- **Pause:** currently PAUSED + output inactive. Compare that with "keep PLAYING, write silence" for resume latency.
- **Sendspin takes over mid-AirPlay:** STOP → `airplay_core_disconnect_client()`. AirPlay 2: the iPhone lets go (checked 2026-09-29). AirPlay 1: it didn't; it now gets a DACP pause first (§6), still to be checked on hardware. Replace stop+start with a real disconnect: add `rtsp_server_disconnect_clients()` as a small upstream PR, or as a wrapper that is still compiled from upstream.
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
| 1 | **libsodium duplicate** if API encryption is enabled | **Hit and resolved 2026-09-28:** we always use ESPHome's `esphome/libsodium`, plus `airplay_sodium/` for the modules its port leaves out (§5, noise). Builds with and without API encryption. Re-check on ESPHome upgrades: the version pin in `media_source.py`, and whether the port's `srcFilter` changed. |
| 2 | **CPU starvation**: playback task at priority 9 | Upstream needs it above its receiver tasks. ESPHome's loop runs at priority 1, and i2s/mixer/resampler tasks have their own priorities. Watch for ESPHome loop lag and task WDT. The task mostly blocks in `write_output`. If needed, lower all of airplay's tasks together rather than just this one. |
| 3 | **Memory** | PSRAM is not the problem; upstream's jitter buffer asks for PSRAM explicitly. **Internal RAM is.** First hardware run (Wi-Fi, speaker chain started): ~62 KB free, 31 KB largest block. Upstream expects every `malloc` above 1 KB to go to PSRAM, but under ESPHome they all go to internal RAM (§5). The codec is redirected by `codec_alloc_psram.c`. If other upstream allocations fail (pairing, plist, the realtime path), options are: redirect more of them the same way; set `CONFIG_SPIRAM_USE_MALLOC` + `ALWAYSINTERNAL=1024` globally (changes ESPHome's behaviour, test Sendspin too); or shrink the speaker chain. |
| 4 | **Flush / stale audio** | See M3. |
| 5 | **Two sources of truth for volume** | The iPhone slider, the HA slider, and the player's `volume_min/max` (0.4–0.9 in this config). Mapping is linear dB→0..1, then the player's range. It may feel odd; tune in M4. |
| 6 | **`esp_audio_codec` licence**: binary-only, "exclusively with Espressif products" | Fine for personal builds. It conflicts with plain GPL for *distributed binaries*, which is why upstream's GPL PR adds a linking exception. Don't publish binaries until that's sorted. |
| 7 | **Upstream licence** | Resolved by vendoring `staging` @ `764ffb6`: GPL-3.0-or-later plus a `LICENSE-EXCEPTION` that explicitly permits linking Espressif binary parts such as `esp_audio_codec` (PR #162). Upstream `main` / v0.2.1 is still non-commercial. **Open: pick this repo's own licence.** GPL-3.0-or-later is the natural fit, and `src/mdns_airplay_esphome.c` is derived from upstream anyway. |
| 8 | **ESPHome API churn** | `media_source` and Sendspin are new and marked experimental. Pin the ESPHome version you build with, and re-check §5 on upgrades. |
| 9 | **Sockets estimate** | Verify with lwIP stats in M1; `media_source.py` reserves TCP 5 / UDP 6 / listen 3. |
| 10 | **`output_delay` on this TOSLINK chain** | Resolved for this setup. Before PR #5 the ESP was 14 ms early (our own estimate, not the receiver); after it, −1.5 ms, within measurement error, so `output_delay: 0`. Other receivers or AVRs may need a value; measure with `tools/click-test`. |
| 11 | **Stale PTP lock at AirPlay 1 start** | Seen 2026-09-28. An AirPlay 2 client (192.168.1.39) connected and left. `ptp_clock` then reported LOCKED with an absurd offset. The next AirPlay 1 session's first anchors used PTP (`ptp_locked=1`, frames "13 years early") until the lock dropped after ~6 s and NTP took over. This is upstream behaviour, harmless once NTP takes over, but it may delay the start of AirPlay 1 playback. Revisit in M3 if it's audible. |
| 12 | **Tracking upstream `staging`** | We pin a pre-release commit. Engine v2 and the source-handover logic are weeks old and got race fixes as late as 2026-09-03. Pin exact commits, re-run the §8 M1/M2 checks after each bump, and prefer bumping to a commit that is also in a release when one exists. |
| 13 | **Any new AirPlay connection ends the current session** | Upstream's `rtsp_server.c` calls `signal_old_client_stop()` on every `accept()`, before the new client asks for anything. **Seen:** Music Assistant's AirPlay provider (Home Assistant host, 192.168.1.39) connects now and then (15 times on 2026-09-29, often ~8 s after an ESP boot); at 20:22 it cut a running click test, then left after 28 s without playing. **Workaround:** disable the AirPlay protocol player for this device in Music Assistant, which should use Sendspin anyway. **Proper fix (upstream):** preempt only when the new client sets up a stream; shairport-sync refuses a second session while one plays unless configured otherwise. |

---

## 10. Decisions log

| Decision | Alternatives considered | Reason |
|---|---|---|
| Receiver on the device, not in MA | MA dedup; MA timestamp bridge | Native iOS multi-select + HomePod sync with zero server plumbing |
| Use airplay-esp32, not a fresh port of shairport-sync | shairport-sync + NQPTP port; openairplay Python receiver | ESP-IDF native, already ESP32-S3 tuned, has PTP + timing servo + pluggable output |
| External component, no upstream PRs | Upstream ESPHome component | David's constraint; the ESPHome media-source API has enough hooks |
| Core as a local IDF component via `add_idf_component(path=…)` | Flatten all C into `components/airplay/` | Loader copies only top-level files; keeps upstream layout + include paths intact |
| Vendor upstream (script) | git submodule | ESPHome doesn't initialise submodules of external components (§4); exact pin |
| Pin upstream `staging`, not `main` | Stay on release v0.2.1 until the next release | All upstream code work and PRs go to `staging`; the GPL relicensing and engine v2 exist only there. Moved before the remaining M2 work, so it isn't spent on a retired engine (2026-09-29) |
| New output backend + `get_pipeline_us` from `notify_audio_played` | Port Sendspin's sync task; patch the ESPHome speaker API | Upstream's servo already does the work; the ESPHome feedback hook exists; zero upstream edits |
| Replace `mdns_airplay.c` instead of patching | Patch upstream | Upstream must stay unmodified; the file is small |
| `has_internal_playlist() = true` | false | Same as Sendspin: the player must not advance its own playlist when we go idle |
| STOP ⇒ drop the AirPlay session | Keep the session, mute | Otherwise the iPhone streams into nothing and thinks it's playing |

---

## 11. Working in this repo

- **Never edit `airplay_core/upstream/`.** Glue goes in `airplay_core/src/`, ESPHome code in `components/airplay/`. Re-vendor with `scripts/sync-upstream.sh <full staging commit SHA>`, then work through the checklist in `airplay_core/UPSTREAM.md`.
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
- **Long logs:** the ESPHome Builder log view in Home Assistant stopped collecting after ~16 minutes twice, while the device kept running. For long sessions, run `esphome logs <yaml> --device <ip>` in a loop that reconnects, appending to a file (used for the 45-minute run in §8 M2).
- **"Update available" after every install means the new firmware crashed and was rolled back.** A firmware that panics before `safe_mode` marks the boot successful (60 s) is rolled back by the bootloader. The device then reports the old build, so ESPHome Builder offers the update again.
  - Check the startup log for `OTA rollback detected! Rolled back from partition …`, `Reset Reason: exception/panic` and the `compiled on` timestamp.
  - The crash itself happens before the API log connection is up, so a backtrace needs the USB serial console.
- Logs: the ESPHome C++ side (`airplay.media_source`) follows `logger:`. The C core and upstream (`airplay_core`, `airplay_out`, `rtsp_handlers`, `audio_v2`, …) show up under tag `esp-idf` and are gated by `esp32: framework: log_level:` (§5).
- The device is David's living-room Sendspin speaker, so keep a known-good firmware to fall back to. The config without the `esphome-airplay` bits is his current one.

---

## 12. References

- airplay-esp32: https://github.com/rbouteiller/airplay-esp32 (its `CLAUDE.md` has a good architecture overview), relicensing PR https://github.com/rbouteiller/airplay-esp32/pull/162
- ESPHome media source / speaker source / Sendspin docs: https://esphome.io/components/media_source/ · https://esphome.io/components/media_player/speaker_source/ · https://esphome.io/components/sendspin/
- ESPHome source (read these first): `esphome/components/media_source/media_source.h`, `speaker_source/speaker_source_media_player.cpp`, `sendspin/media_source/sendspin_media_source.{h,cpp}`, `audio_http/media_source.py`
- sendspin-cpp (Apache-2.0; how Sendspin uses `notify_audio_played`: hard/soft sync, frame insert/drop): https://github.com/Sendspin/sendspin-cpp, `src/sync_task.cpp`
- shairport-sync / NQPTP (background, AirPlay 2 timing): https://github.com/mikebrady/shairport-sync · https://github.com/mikebrady/nqptp
- MA AirPlay Receiver plugin (classic shairport-sync): https://www.music-assistant.io/plugins/airplay-receiver/ and `music_assistant/providers/airplay_receiver/` in music-assistant/server
