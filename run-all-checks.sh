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
# Guard: ctest re-invokes each test BY NAME. A non-ASCII byte in a TEST_CASE name
# (e.g. an em-dash) round-trips fine on Linux but mangles on the Windows CI console
# codepage, so the exe matches no test and ctest calls it a FAILURE. This has bitten
# Windows CI twice (#110) — keep TEST_CASE names ASCII-only.
step "ASCII-only test names (Windows ctest name-filter safety)"
BADNAMES="$(grep -rnP 'TEST_CASE\s*\(\s*"[^"]*[^\x00-\x7F]' tests/ || true)"
if [[ -n "$BADNAMES" ]]; then
    printf '%s\n' "$BADNAMES" >&2
    fail "non-ASCII TEST_CASE name(s) above -> break Windows ctest name matching (keep names ASCII)"
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

STANDALONE="$BUILD_DIR/VASynth_artefacts/Release/Standalone/synth"
VST3_SO="$BUILD_DIR/VASynth_artefacts/Release/VST3/synth.vst3/Contents/x86_64-linux/synth.so"
[[ -x "$STANDALONE" ]]                                            || fail "Standalone artefact missing"
[[ -e "$BUILD_DIR/VASynth_artefacts/Release/VST3/synth.vst3" ]]  || fail "VST3 artefact missing"

# ---------------------------------------------------------------------------------------------
# Honest-gate freshness check. The build above rebuilds both shipping artefacts, but a green
# gate must MEAN "the binaries on disk are the gated code" — so verify each artefact carries the
# build stamp of THIS commit (Source/BuildStamp.cpp -> "VASYNTHBUILD:<hash>"). This catches a
# stale binary from any cause (an incremental miss, a wrong build dir, a rename-orphaned bundle
# a DAW loads). We compare the COMMIT identity and tolerate the "+" dirty suffix a gate build of
# an uncommitted tree legitimately carries.
step "Verifying artefacts were built from HEAD ($(git rev-parse --short HEAD))"
HEAD_HASH="$(git rev-parse --short HEAD)"
verify_stamp() {
    local art="$1" name="$2" emb base
    emb="$(strings "$art" | grep -oE 'VASYNTHBUILD:[0-9a-f]{7,40}\+?' | head -1)"
    [[ -n "$emb" ]] || fail "$name has no VASYNTHBUILD stamp — cannot prove it is the gated code (rebuild clean?)"
    base="${emb#VASYNTHBUILD:}"; base="${base%+}"
    [[ "$base" == "$HEAD_HASH" ]] \
        || fail "STALE ARTEFACT: $name embeds '$base' but HEAD is '$HEAD_HASH' — the binary on disk is NOT the gated code"
    echo "   $name -> $emb"
}
verify_stamp "$STANDALONE" "Standalone"
verify_stamp "$VST3_SO"    "VST3"

step "Running CTest"
( cd "$BUILD_DIR" && ctest --output-on-failure -j"$JOBS" ) || fail "ctest reported failures"

step "ALL CHECKS PASSED"
