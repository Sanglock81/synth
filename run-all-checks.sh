#!/usr/bin/env bash
# ===========================================================================
# VA Synth CI gate. Builds both artefacts (Standalone + VST3) and the test
# suite, runs the full CTest set (DSP unit tests, plugin-layer tests, and the
# pluginval integration test). Exits non-zero on ANY failure.
#
#   ./run-all-checks.sh            # Release build + all checks
#
# Run this before declaring any task complete. No task is done with failing or
# skipped tests.
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
BUILD_DIR="build"
JOBS="$(nproc)"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

# --- 1. Ensure pluginval is available --------------------------------------
PLUGINVAL="$ROOT/tools/pluginval"
if [[ ! -x "$PLUGINVAL" ]]; then
    step "Fetching pluginval (Linux prebuilt)"
    mkdir -p "$ROOT/tools"
    curl -sSL -o "$ROOT/tools/pluginval.zip" \
        https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Linux.zip \
        || fail "could not download pluginval"
    (cd "$ROOT/tools" && unzip -o pluginval.zip >/dev/null && chmod +x pluginval)
fi
"$PLUGINVAL" --version >/dev/null 2>&1 || fail "pluginval not runnable"

# --- 2. Display for GUI/editor tests ---------------------------------------
if [[ -z "${DISPLAY:-}" ]]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        step "No DISPLAY; re-executing under xvfb-run"
        exec xvfb-run -a "$0" "$@"
    else
        printf '\033[1;33mWARNING: no DISPLAY and no xvfb-run; pluginval/editor tests may fail.\033[0m\n'
    fi
fi

# --- 3. Configure (tests ON) ------------------------------------------------
step "Configuring (Release, tests ON)"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DVASYNTH_BUILD_TESTS=ON \
    || fail "configure failed"

# --- 4. Build both artefacts + tests ---------------------------------------
step "Building Standalone + VST3 + tests (-j$JOBS)"
cmake --build "$BUILD_DIR" -j"$JOBS" || fail "build failed"

# Confirm both artefacts exist.
[[ -x "$BUILD_DIR/VASynth_artefacts/Release/Standalone/VA Synth" ]] \
    || fail "Standalone artefact missing"
[[ -e "$BUILD_DIR/VASynth_artefacts/Release/VST3/VA Synth.vst3" ]] \
    || fail "VST3 artefact missing"

# --- 5. Run the whole CTest set (unit + plugin + pluginval) -----------------
step "Running CTest"
( cd "$BUILD_DIR" && ctest --output-on-failure -j"$JOBS" ) || fail "ctest reported failures"

step "ALL CHECKS PASSED"
