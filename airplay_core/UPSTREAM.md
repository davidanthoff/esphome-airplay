# Vendored upstream: airplay-esp32

- Repository: https://github.com/rbouteiller/airplay-esp32
- Commit: `811d5f8af750096683dcec7c95d1cbc08c2f7601` (2026-09-21), also recorded in `upstream/COMMIT`
- Copied unmodified: `main/`, `components/dac/`, `LICENSE`. Upstream's own `CMakeLists.txt` / `idf_component.yml` come along but are inert: `airplay_core/CMakeLists.txt` lists the sources itself.
- Refresh with `scripts/sync-upstream.sh [commit]`.

## Licence

At this commit the upstream licence is the author's **Non-Commercial License** (see `upstream/LICENSE`). It allows use, copying, modification and distribution for non-commercial purposes, and the licence notice must be kept.

[PR #162](https://github.com/rbouteiller/airplay-esp32/pull/162) relicenses the project to **GPL-3.0-or-later**. It adds an additional permission (`LICENSE-EXCEPTION`) for linking Espressif's closed binary components. All contributors have agreed to the relicensing. As of 2026-09-28 it is **not merged**; `main` still carries the non-commercial licence. Once it merges, bump the vendored commit past the merge and update this note.

`src/mdns_airplay_esphome.c` is derived from upstream `main/network/mdns_airplay.c`, so the same licence applies to it.

## Rules

- Never hand-edit files under `upstream/`. Glue goes in `src/`. Genuine fixes go upstream as PRs, and we re-vendor after they merge.
- Files we replace instead of compiling are listed at the bottom of the `UPSTREAM_SRCS` block in `CMakeLists.txt`.
