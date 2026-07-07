#!/usr/bin/env bash
# ===========================================================================
# VA Synth CI gate.
#
#   ./run-all-checks.sh              # Release: build both artefacts, ctest, pluginval
#   ./run-all-checks.sh --sanitize   # ASan+LSan and UBSan: build tests+soak, ctest,
#                                      soak — the memory-leak / UB monitoring gate
#
# Exits non-zero on ANY failure. Run before declaring any task complete.
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
JOBS="$(nproc)"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

SANITIZE=0
for arg in "$@"; do case "$arg" in --sanitize) SANITIZE=1 ;; esac; done

# Optional: honour FETCHCONTENT_BASE_DIR (set by CI to a cached JUCE/Catch2 dir).
# Empty/unset -> no extra flag, so local behaviour is unchanged.
FCBASE="${FETCHCONTENT_BASE_DIR:+-DFETCHCONTENT_BASE_DIR=$FETCHCONTENT_BASE_DIR}"

# Ensure a display for GUI/editor tests (pluginval, ScopedJuceInitialiser).
if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
    step "No DISPLAY; re-executing under xvfb-run"
    exec xvfb-run -a "$0" "$@"
fi

# ---------------------------------------------------------------------------
if [[ $SANITIZE -eq 1 ]]; then
    # Sanitizer suite: unit + plugin tests + soak under ASan/LSan then UBSan.
    # pluginval is a prebuilt (uninstrumented) binary and the VST3 isn't built
    # here, so it's excluded; correctness is covered by the normal gate.
    export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1"
    export LSAN_OPTIONS="suppressions=$ROOT/tests/lsan.supp:print_suppressions=0"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

    for SAN in ASAN UBSAN; do
        DIR="build-$(echo "$SAN" | tr '[:upper:]' '[:lower:]')"
        step "[$SAN] configure"
        cmake -B "$DIR" -DCMAKE_BUILD_TYPE=Debug -DVASYNTH_BUILD_TESTS=ON \
              -DVASYNTH_ENABLE_LTO=OFF -DVASYNTH_$SAN=ON $FCBASE >/dev/null \
              || fail "$SAN configure"
        step "[$SAN] build tests + soak (-j$JOBS)"
        cmake --build "$DIR" --target dsp_tests plugin_tests soak -j"$JOBS" \
              || fail "$SAN build"
        step "[$SAN] ctest (unit + plugin, excluding pluginval)"
        ( cd "$DIR" && ctest --output-on-failure -j"$JOBS" -E pluginval ) \
              || fail "$SAN ctest"
        step "[$SAN] soak (60 audio-seconds)"
        "$DIR/tests/soak" 60 || fail "$SAN soak"
    done

    step "ALL SANITIZER CHECKS PASSED"
    exit 0
fi

# ---------------------------------------------------------------------------
# Normal Release gate.
BUILD_DIR="build"

# pluginval
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

step "Configuring (Release, tests ON)"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DVASYNTH_BUILD_TESTS=ON $FCBASE || fail "configure failed"

step "Building Standalone + VST3 + tests (-j$JOBS)"
cmake --build "$BUILD_DIR" -j"$JOBS" || fail "build failed"

[[ -x "$BUILD_DIR/VASynth_artefacts/Release/Standalone/synth" ]] || fail "Standalone artefact missing"
[[ -e "$BUILD_DIR/VASynth_artefacts/Release/VST3/synth.vst3" ]]   || fail "VST3 artefact missing"

step "Running CTest"
( cd "$BUILD_DIR" && ctest --output-on-failure -j"$JOBS" ) || fail "ctest reported failures"

step "ALL CHECKS PASSED"
