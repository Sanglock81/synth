#!/usr/bin/env bash
# ============================================================================
# ThinkPad validation — run ON the ThinkPad X1 Carbon (3rd gen, Linux).
# Self-contained: needs only a C++17 compiler (g++/clang++). No JUCE, no cmake.
#
#   ./validate.sh                 # full run: build, bench, 10-min soak, PipeWire
#   SOAK_SECS=60 ./validate.sh    # shorter soak (smoke test)
#
# Produces thinkpad-report.txt next to this script. Paste it back to the dev box:
# its measured numbers become the REAL derate factor, replacing the assumed x3.5.
# ============================================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$HERE/thinkpad-report.txt"
SOAK_SECS="${SOAK_SECS:-600}"
CXX="${CXX:-g++}"
CXXFLAGS="-O3 -march=native -std=c++17"

# ---- governor: set performance, restore on exit ----------------------------
GOV_PATHS=(/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor)
declare -a SAVED_GOV=()
restore_governor() {
  [ "${#SAVED_GOV[@]}" -eq 0 ] && return
  echo "Restoring CPU governor..." >&2
  local i=0
  for g in "${GOV_PATHS[@]}"; do
    [ -w "$g" ] && echo "${SAVED_GOV[$i]}" | sudo tee "$g" >/dev/null 2>&1
    i=$((i+1))
  done
}
set_performance() {
  local cur; cur="$(cat "${GOV_PATHS[0]}" 2>/dev/null || echo unknown)"
  for g in "${GOV_PATHS[@]}"; do SAVED_GOV+=("$(cat "$g" 2>/dev/null || echo unknown)"); done
  if [ "$cur" = "performance" ]; then echo "Governor already 'performance'."; return 0; fi
  echo "CPU governor is '$cur'. Setting 'performance' (needs sudo; will restore on exit)..."
  local ok=1
  for g in "${GOV_PATHS[@]}"; do echo performance | sudo tee "$g" >/dev/null 2>&1 || ok=0; done
  trap restore_governor EXIT INT TERM
  local now; now="$(cat "${GOV_PATHS[0]}" 2>/dev/null || echo unknown)"
  if [ "$now" = "performance" ]; then echo "Governor set to 'performance'."; return 0; fi
  echo "!! Could not set 'performance' (ok=$ok). Bench numbers will be INVALID — see memory note."; return 1
}

section() { echo; echo "======== $* ========"; }

run_all() {
  echo "synth ThinkPad validation report"
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ) (UTC)"
  [ -f "$HERE/BUNDLE_INFO" ] && { echo "bundle:"; sed 's/^/  /' "$HERE/BUNDLE_INFO"; }

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
    echo "!! '$CXX' not found. Install a compiler, e.g.:  sudo apt-get install -y build-essential"
    echo "   (then re-run). Skipping bench + soak."
    return 0
  fi
  echo "compiler: $($CXX --version | head -1)"
  echo "flags   : $CXXFLAGS"
  set -x
  "$CXX" $CXXFLAGS "$HERE/bench/bench_engine.cpp" -I"$HERE/dsp" -o "$HERE/dsp_bench" -lpthread
  "$CXX" $CXXFLAGS "$HERE/soak_harness.cpp"        -I"$HERE/dsp" -o "$HERE/soak"      -lpthread
  set +x
  echo "built: dsp_bench, soak"

  section "DSP BENCH (measured on THIS machine — the real derate source)"
  "$HERE/dsp_bench" || echo "!! bench failed"

  section "10-MINUTE SOAK (compute-overrun proxy + thermal stress, ALL FX)"
  echo "duration: ${SOAK_SECS}s at block=128 (progress on stderr every 30s)"
  "$HERE/soak" "$SOAK_SECS" 128 || echo "!! soak failed"

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
    command -v pw-top >/dev/null 2>&1 && { echo "-- pw-top snapshot:"; timeout 3 pw-top -b -n 2 2>/dev/null | tail -n +1 | sed 's/^/   /'; }
  elif command -v pactl >/dev/null 2>&1; then
    echo "pw-metadata not found; PulseAudio/pipewire-pulse sink info:"; pactl list sinks short 2>/dev/null | sed 's/^/   /'
  else
    echo "No PipeWire/PulseAudio tools found. ALSA cards:"; cat /proc/asound/cards 2>/dev/null | sed 's/^/   /'
  fi

  section "DONE"
  echo "Paste $REPORT back to the dev box. These numbers replace the assumed x3.5 derate."
}

echo "== synth ThinkPad validation =="
set_performance || true
run_all 2>&1 | tee "$REPORT"
# governor restored by the EXIT trap
