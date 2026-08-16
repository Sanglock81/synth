#!/usr/bin/env bash
# ============================================================================
# Minimum-spec target validation — run ON the reference target (the i7-8650U ThinkPad, Linux).
# Self-contained: needs only a C++17 compiler (g++/clang++). No JUCE, no cmake.
#
#   ./validate.sh                 # FULL gate: build, bench, 10-min soak @128 & @256, PipeWire
#   ./validate.sh --quick         # QUICK: build + bench only (no soak) — for iteration, NOT the gate
#   SOAK_SECS=60 ./validate.sh    # shorter soak (smoke test)
#   ./validate.sh --allow-nonperf-governor   # opt in to a DELIBERATE non-performance run
#                                            # (numbers marked INVALID/conservative; never the gate)
#
# The governor is a HARD validity precondition: unless it is 'performance', the bench/soak
# numbers are inflated and meaningless as a gate. This script sets 'performance' via sudo and
# restores on exit; if it CANNOT (e.g. no passwordless sudo, or run with no TTY so sudo can't
# prompt), it ABORTS before wasting ~22 minutes producing invalid numbers, and tells you the one
# command to run first. Use --allow-nonperf-governor only for an intentional conservative probe.
#
# Writes ONE report next to this script: target-report.txt. Paste it back to the
# dev box — its MEASURED numbers replace the assumed x3.5 derate and settle every
# provisional CPU decision (see the "report-processing contract" in README.md).
# ============================================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$HERE/target-report.txt"
SOAK_SECS="${SOAK_SECS:-600}"
CXX="${CXX:-g++}"
CXXFLAGS="-O3 -march=native -std=c++17"

QUICK=0
ALLOW_NONPERF="${ALLOW_NONPERF_GOVERNOR:-0}"
for arg in "$@"; do case "$arg" in
  --quick) QUICK=1 ;;
  --allow-nonperf-governor) ALLOW_NONPERF=1 ;;
  -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) echo "unknown arg: $arg (use --quick, --allow-nonperf-governor, or -h)"; exit 2 ;;
esac; done

# ---- governor: set performance, restore on ANY exit (incl. Ctrl-C) ---------
GOV_PATHS=(/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor)
declare -a SAVED_GOV=()
GOV_ARMED=0
restore_governor() {
  [ "$GOV_ARMED" -eq 1 ] || return 0
  GOV_ARMED=0
  echo "Restoring CPU governor..." >&2
  local i=0
  for g in "${GOV_PATHS[@]}"; do
    [ -e "$g" ] && echo "${SAVED_GOV[$i]}" | sudo tee "$g" >/dev/null 2>&1
    i=$((i+1))
  done
}
set_performance() {
  local cur; cur="$(cat "${GOV_PATHS[0]}" 2>/dev/null || echo unknown)"
  for g in "${GOV_PATHS[@]}"; do SAVED_GOV+=("$(cat "$g" 2>/dev/null || echo unknown)"); done
  # Arm the restore trap BEFORE touching anything, so Ctrl-C mid-run always restores.
  trap restore_governor EXIT INT TERM
  GOV_ARMED=1
  if [ "$cur" = "performance" ]; then echo "Governor already 'performance'."; return 0; fi
  echo "CPU governor is '$cur'. Setting 'performance' (needs sudo; restores on exit)..."
  local ok=1
  for g in "${GOV_PATHS[@]}"; do echo performance | sudo tee "$g" >/dev/null 2>&1 || ok=0; done
  local now; now="$(cat "${GOV_PATHS[0]}" 2>/dev/null || echo unknown)"
  if [ "$now" = "performance" ]; then echo "Governor set to 'performance'."; return 0; fi
  echo "!! Could not set 'performance' (ok=$ok)."; return 1
}

section() { echo; echo "======== $* ========"; }
skip()    { echo "SKIPPED: $*"; }        # honest degrade — a scenario that can't run says so, loudly

GOV_OK=1

run_all() {
  echo "synth minimum-spec target validation report"
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ) (UTC)"
  if [ "$QUICK" -eq 1 ]; then
    echo "MODE: QUICK  (build + bench only, NO soak) -- this is NOT the ship gate, do not treat it as one."
  else
    echo "MODE: FULL   (build + bench + ${SOAK_SECS}s soak @128 and @256 + PipeWire)"
  fi
  [ -f "$HERE/BUNDLE_INFO" ] && { echo "bundle:"; sed 's/^/  /' "$HERE/BUNDLE_INFO"; }
  [ "$GOV_OK" -eq 1 ] || echo "WARNING: governor is NOT 'performance' -- bench/soak numbers are INVALID (see bench-governor note). Re-run with governor set."

  section "CPU / CLOCK / GOVERNOR CONTEXT"
  echo "governor (cpu0): $(cat "${GOV_PATHS[0]}" 2>/dev/null || echo n/a)"
  echo "-- per-core scaling_cur_freq (kHz):"
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
    [ -r "$f" ] && printf "   %s: %s\n" "$(echo "$f" | grep -o 'cpu[0-9]*' | head -1)" "$(cat "$f")"
  done
  echo "-- freq limits (cpu0, kHz): min=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null) max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null)"
  echo "-- turbo (intel no_turbo, 1=disabled): $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
  if command -v lscpu >/dev/null 2>&1; then echo "-- lscpu:"; lscpu | sed 's/^/   /'; else
    echo "-- /proc/cpuinfo (model + MHz):"; grep -E "model name|cpu MHz" /proc/cpuinfo | sed 's/^/   /'; fi
  if command -v cpupower >/dev/null 2>&1; then echo "-- cpupower frequency-info:"; cpupower frequency-info 2>/dev/null | sed 's/^/   /'; fi
  echo "-- thermal zones (millidegC):"
  for t in /sys/class/thermal/thermal_zone*/temp; do [ -r "$t" ] && printf "   %s: %s\n" "$t" "$(cat "$t")"; done
  echo "-- kernel: $(uname -a)"

  section "BUILD (bare compiler, no JUCE/cmake)"
  if ! command -v "$CXX" >/dev/null 2>&1; then
    skip "compiler '$CXX' not found -- install one (sudo apt-get install -y build-essential) and re-run. Bench + soak cannot run."
    return 0
  fi
  echo "compiler: $($CXX --version | head -1)"
  echo "flags   : $CXXFLAGS"
  local built_bench=0 built_soak=0
  if "$CXX" $CXXFLAGS "$HERE/bench/bench_engine.cpp" -I"$HERE/dsp" -o "$HERE/dsp_bench" -lpthread 2>"$HERE/.bench_build.log"; then
    built_bench=1; echo "built: dsp_bench"
  else
    skip "dsp_bench did not compile -- see error below (report is INCOMPLETE):"; sed 's/^/   /' "$HERE/.bench_build.log"
  fi
  if "$CXX" $CXXFLAGS "$HERE/soak_harness.cpp" -I"$HERE/dsp" -o "$HERE/soak" -lpthread 2>"$HERE/.soak_build.log"; then
    built_soak=1; echo "built: soak"
  else
    skip "soak did not compile -- see error below:"; sed 's/^/   /' "$HERE/.soak_build.log"
  fi

  section "DSP BENCH (measured on THIS machine -- the real derate source)"
  if [ "$built_bench" -eq 1 ]; then "$HERE/dsp_bench" || skip "dsp_bench crashed at runtime."; else skip "no dsp_bench binary (build failed above)."; fi

  if [ "$QUICK" -eq 1 ]; then
    section "SOAK"
    skip "QUICK mode -- soak not run. Re-run WITHOUT --quick for the ship gate."
  else
    for blk in 128 256; do
      section "${SOAK_SECS}s SOAK @ block=$blk (compute-overrun proxy + thermal stress, ALL FX)"
      if [ "$built_soak" -eq 1 ]; then
        echo "duration: ${SOAK_SECS}s at block=$blk (progress on stderr every 30s)"
        "$HERE/soak" "$SOAK_SECS" "$blk" || skip "soak @${blk} crashed at runtime."
      else
        skip "no soak binary (build failed above) -- cannot report overruns @${blk}."
      fi
    done
  fi

  section "PIPEWIRE / AUDIO CONFIG (quantum + rate at 128 and 256)"
  if command -v pw-metadata >/dev/null 2>&1; then
    echo "-- current settings:"; pw-metadata -n settings 2>/dev/null | grep -Ei "clock.(rate|quantum|force)" | sed 's/^/   /'
    for q in 128 256; do
      pw-metadata -n settings 0 clock.force-quantum "$q" >/dev/null 2>&1
      sleep 1
      echo "-- forced quantum=$q:"; pw-metadata -n settings 2>/dev/null | grep -Ei "clock.(rate|quantum|force)" | sed 's/^/   /'
    done
    pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1   # back to auto
    echo "(restored clock.force-quantum=0)"
    command -v pw-top >/dev/null 2>&1 && { echo "-- pw-top snapshot:"; timeout 3 pw-top -b -n 2 2>/dev/null | sed 's/^/   /'; }
  elif command -v pactl >/dev/null 2>&1; then
    skip "pw-metadata not found; PulseAudio/pipewire-pulse sink info instead:"; pactl list sinks short 2>/dev/null | sed 's/^/   /'
  else
    skip "no PipeWire/PulseAudio tools found. ALSA cards:"; cat /proc/asound/cards 2>/dev/null | sed 's/^/   /'
  fi

  section "DONE"
  [ "$QUICK" -eq 1 ] && echo "(QUICK run -- soak was skipped; this is NOT the gate.)"
  echo "DONE -- paste target-report.txt back"
}

echo "== synth minimum-spec target validation =="
set_performance && GOV_OK=1 || GOV_OK=0
# Validity gate: the governor MUST be 'performance' or the numbers are meaningless. Refuse to run
# the (expensive) bench/soak at the wrong governor unless the caller explicitly opted in. This is
# the fix for the harness silently spending ~22 min stamping "INVALID" numbers.
if [ "$GOV_OK" -ne 1 ] && [ "$ALLOW_NONPERF" -ne 1 ]; then
  cur="$(cat "${GOV_PATHS[0]}" 2>/dev/null || echo unknown)"
  cat >&2 <<EOF

ABORTING before the bench/soak: CPU governor is '$cur', not 'performance', and this script could
not set it (sudo needs a password and there may be no TTY to enter it — e.g. a background run).
Bench/soak numbers at '$cur' are INVALID for the gate, so nothing is measured.

Do ONE of these, then re-run ./validate.sh:
  1) sudo cpupower frequency-set -g performance      # then run this script in the same session
  2) run this script from an interactive terminal so its own 'sudo' can prompt for your password
  3) ./validate.sh --allow-nonperf-governor          # ONLY for a deliberate conservative probe
                                                      # (numbers stay marked INVALID; never the gate)
EOF
  exit 3
fi
run_all 2>&1 | tee "$REPORT"
# governor restored by the EXIT trap
