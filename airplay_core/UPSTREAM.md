# Vendored upstream: airplay-esp32

- Repository: https://github.com/rbouteiller/airplay-esp32
- Commit: `764ffb6d2cc9019a0373d2bab81f444e6cf937dd` (2026-09-24), on upstream's **`staging`** branch. Also recorded in `upstream/COMMIT`.
- Copied unmodified: `main/`, `components/dac/`, `LICENSE`, `LICENSE-EXCEPTION`. Upstream's own `CMakeLists.txt` / `idf_component.yml` come along but are inert: `airplay_core/CMakeLists.txt` lists the sources itself.
- Refresh with `scripts/sync-upstream.sh [full commit SHA]`.

## Why `staging`, not `main`

Upstream takes every pull request on `staging` and moves `main` only when it tags a release (its `CONTRIBUTING.md`). Between the last release (v0.2.1, `811d5f8`, which we vendored before) and this commit, `main` got only the docs site, a version bump and a toolchain pin. All code work landed on `staging`:
- the GPL relicensing;
- the timing engine we build on ("engine v2");
- the fixes any upstream PR of ours would build on.

So we pin a specific `staging` commit, never the branch tip, and re-verify on hardware after each bump (HANDOFF.md §8).

## Licence

From this commit, upstream is **GPL-3.0-or-later** (`upstream/LICENSE`). It adds an additional permission, `upstream/LICENSE-EXCEPTION`: works that link Espressif's binary-only components, such as `esp_audio_codec`, may be conveyed without those components' source. The relicensing is [PR #162](https://github.com/rbouteiller/airplay-esp32/pull/162), merged into `staging` on 2026-09-20; all contributors agreed to it. Upstream `main` (v0.2.1) still carries the earlier Non-Commercial License.

`src/mdns_airplay_esphome.c` is derived from upstream `main/network/mdns_airplay.c`, so the same licence applies to it.

## Rules

- Never hand-edit files under `upstream/`. Glue goes in `src/`. Genuine fixes go upstream as PRs against `staging`, and we re-vendor after they merge.
- Files we replace instead of compiling are listed at the bottom of the `UPSTREAM_SRCS` block in `CMakeLists.txt`.
- After a bump:
  - diff upstream `main/network/mdns_airplay.c` against `src/mdns_airplay_esphome.c`;
  - compare `start_airplay_services()` in upstream `main/main.c` with `airplay_core_start()`;
  - check the value of `OUTPUT_PIPELINE_LATENCY_US` against `UPSTREAM_PIPELINE_LATENCY_US` in `src/audio_output_esphome.c`.
