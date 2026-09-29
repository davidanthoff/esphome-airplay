#!/usr/bin/env bash
# Vendor airplay-esp32 into airplay_core/upstream at a pinned commit.
#
#   scripts/sync-upstream.sh            # uses UPSTREAM_REF below
#   scripts/sync-upstream.sh <commit>   # bump to another commit (full SHA)
#
# The pin is a commit on upstream's `staging` branch: upstream takes all PRs
# there and moves `main` only when it tags a release (see UPSTREAM.md).
#
# Copies upstream main/ and components/dac/ unmodified (build files included,
# they are inert here), plus LICENSE and LICENSE-EXCEPTION.
# Never edit files under airplay_core/upstream by hand: put ESPHome-specific
# code in airplay_core/src, or send the change upstream. After a bump, update
# UPSTREAM_REF here and the commit in airplay_core/UPSTREAM.md, then diff
# upstream/main/network/mdns_airplay.c against src/mdns_airplay_esphome.c.
set -euo pipefail

UPSTREAM_URL="https://github.com/rbouteiller/airplay-esp32.git"
UPSTREAM_REF="${1:-764ffb6d2cc9019a0373d2bab81f444e6cf937dd}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${REPO_ROOT}/airplay_core/upstream"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# Fetch only the pinned commit. (A --filter=blob:none clone failed to read
# its lazily fetched blobs with Git for Windows 2.55.)
git init --quiet "${TMP}/src"
git -C "${TMP}/src" fetch --quiet --depth 1 "${UPSTREAM_URL}" "${UPSTREAM_REF}"
git -C "${TMP}/src" checkout --quiet FETCH_HEAD -- main components/dac LICENSE LICENSE-EXCEPTION
COMMIT="$(git -C "${TMP}/src" rev-parse FETCH_HEAD)"

mkdir -p "${DEST}"
rm -rf "${DEST}/main" "${DEST}/components"
mkdir -p "${DEST}/components"
cp -R "${TMP}/src/main" "${DEST}/main"
cp -R "${TMP}/src/components/dac" "${DEST}/components/dac"
cp "${TMP}/src/LICENSE" "${DEST}/LICENSE"
cp "${TMP}/src/LICENSE-EXCEPTION" "${DEST}/LICENSE-EXCEPTION"

# Upstream's own CMakeLists.txt / idf_component.yml come along unmodified.
# They are inert: IDF only builds component roots it is told about, and
# airplay_core/CMakeLists.txt lists the upstream sources itself.

echo "${COMMIT}" > "${DEST}/COMMIT"
echo "Vendored airplay-esp32 ${COMMIT} into ${DEST}"
