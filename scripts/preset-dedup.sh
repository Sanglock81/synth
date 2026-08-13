#!/usr/bin/env bash
# ===========================================================================
# Rerunnable render-similarity dedup pass. Builds the offline preset_dedup tool
# (EXCLUDE_FROM_ALL, so it is NOT part of the CI gate build), renders every
# factory preset through the real processor with one fixed phrase, and rewrites
# docs/preset-similarity.md with the top-30 most-similar pairs. No WAVs written.
#
#   ./scripts/preset-dedup.sh
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
JOBS="$(nproc)"
BUILD_DIR="build"

# A display is needed for JUCE's ScopedJuceInitialiser_GUI (same as the plugin tests).
if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
    exec xvfb-run -a "$0" "$@"
fi

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DVASYNTH_BUILD_TESTS=ON >/dev/null
cmake --build "$BUILD_DIR" --target preset_dedup -j"$JOBS"

BIN="$BUILD_DIR/tests/preset_dedup"
[[ -x "$BIN" ]] || { echo "preset_dedup binary missing"; exit 1; }
"$BIN" "$ROOT/docs/preset-similarity.md"
echo "Done -> docs/preset-similarity.md"
