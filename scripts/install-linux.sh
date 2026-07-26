#!/usr/bin/env bash
# ============================================================================
# synth — Linux install script
#
# Builds the Release binaries from source and installs:
#   * the VST3 plugin  -> ~/.vst3           (any DAW that scans it finds "synth")
#   * the standalone   -> ~/.local/bin/synth
#   * a desktop launcher + menu entry
#
# Usage:  ./scripts/install-linux.sh [options]
#   --no-deps        skip the apt dependency install (assume they're present)
#   --no-desktop     don't create the .desktop launcher / menu entry
#   --vst3-dir DIR   install the VST3 here instead of ~/.vst3
#   --prefix DIR     install the standalone under DIR/bin (default ~/.local)
#   --jobs N         parallel build jobs (default: all cores)
#   --build-dir DIR  cmake build directory (default: build)
#   -h, --help       this help
#
# Re-runnable: it reconfigures, rebuilds incrementally, and overwrites the
# installed copies. Nothing here needs root — everything lands in your $HOME.
# ============================================================================
set -euo pipefail

# --- resolve the repo root (this script lives in <repo>/scripts) ------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"

# --- defaults ---------------------------------------------------------------
DEPS=1; DESKTOP=1
VST3_DIR="${HOME}/.vst3"
PREFIX="${HOME}/.local"
BUILD_DIR="build"
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-deps)     DEPS=0; shift;;
    --no-desktop)  DESKTOP=0; shift;;
    --vst3-dir)    VST3_DIR="$2"; shift 2;;
    --prefix)      PREFIX="$2"; shift 2;;
    --jobs)        JOBS="$2"; shift 2;;
    --build-dir)   BUILD_DIR="$2"; shift 2;;
    -h|--help)     sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "Unknown option: $1 (try --help)"; exit 2;;
  esac
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warning:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }

cd "$REPO"

# --- 1. dependencies (Debian/Ubuntu) ----------------------------------------
# JUCE is fetched by CMake (FetchContent) — these are the system libs it links.
# The web-browser + curl modules are OFF in this build, so libwebkit/libcurl are
# NOT required; we try them anyway (harmless) but never fail the install on them.
REQUIRED_PKGS=(build-essential cmake git pkg-config
  libasound2-dev libjack-jackd2-dev
  libfreetype-dev libfontconfig1-dev
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev
  libgl1-mesa-dev)
OPTIONAL_PKGS=(libcurl4-openssl-dev libwebkit2gtk-4.1-dev)

if [[ "$DEPS" == 1 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    say "Installing build dependencies (sudo apt-get)…"
    sudo apt-get update -qq
    sudo apt-get install -y "${REQUIRED_PKGS[@]}"
    # optional set: don't abort if a distro names them differently
    sudo apt-get install -y "${OPTIONAL_PKGS[@]}" 2>/dev/null \
      || warn "optional libs (webkit/curl) not installed — not needed for this build, continuing."
  else
    warn "Not a Debian/apt system. Install the equivalents of:"
    printf '    %s\n' "${REQUIRED_PKGS[*]}"
    warn "then re-run with --no-deps."
  fi
else
  say "Skipping dependency install (--no-deps)."
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found. Install it (or drop --no-deps)."

# --- 2. configure + build (Release) -----------------------------------------
say "Configuring (Release) in $BUILD_DIR/ …"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
say "Building with $JOBS jobs (first build fetches JUCE — a few minutes)…"
cmake --build "$BUILD_DIR" --target VASynth_Standalone VASynth_VST3 -j "$JOBS"

ART="$BUILD_DIR/VASynth_artefacts/Release"
STANDALONE_BIN="$ART/Standalone/synth"
VST3_BUNDLE="$ART/VST3/synth.vst3"
[[ -x "$STANDALONE_BIN" ]] || die "standalone not built at $STANDALONE_BIN"
[[ -d "$VST3_BUNDLE"     ]] || die "VST3 not built at $VST3_BUNDLE"

# --- 3. install the VST3 ----------------------------------------------------
say "Installing VST3 -> $VST3_DIR/"
mkdir -p "$VST3_DIR"
rm -rf "$VST3_DIR/synth.vst3"
cp -a "$VST3_BUNDLE" "$VST3_DIR/"

# --- 4. install the standalone ----------------------------------------------
BIN_DIR="$PREFIX/bin"
say "Installing standalone -> $BIN_DIR/synth"
mkdir -p "$BIN_DIR"
install -m 0755 "$STANDALONE_BIN" "$BIN_DIR/synth"

# --- 5. desktop launcher ----------------------------------------------------
if [[ "$DESKTOP" == 1 ]]; then
  APPS_DIR="$HOME/.local/share/applications"
  mkdir -p "$APPS_DIR"
  cat > "$APPS_DIR/synth.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=synth
Comment=Virtual-analog polysynth (standalone)
Exec=$BIN_DIR/synth
Terminal=false
Categories=AudioVideo;Audio;Music;
DESKTOP
  update-desktop-database "$APPS_DIR" 2>/dev/null || true
  say "Menu entry installed (synth.desktop)."
fi

# --- 6. PATH hint -----------------------------------------------------------
case ":$PATH:" in
  *":$BIN_DIR:"*) : ;;
  *) warn "$BIN_DIR is not on your PATH. Add this to ~/.bashrc to run 'synth' from a terminal:"
     printf '    export PATH="%s:$PATH"\n' "$BIN_DIR";;
esac

say "Done."
echo "  Standalone : $BIN_DIR/synth   (or the 'synth' menu entry)"
echo "  VST3       : $VST3_DIR/synth.vst3   (rescan plugins in your DAW)"
echo "  Uninstall  : scripts/uninstall-linux.sh"
