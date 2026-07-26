#!/usr/bin/env bash
# ============================================================================
# synth — build a distributable release package (Linux x86_64).
#
# Produces  dist/synth-<version>-linux-x86_64.tar.gz  containing the PREBUILT
# standalone + VST3 + docs + a copy-only installer (no build, no deps needed on
# the target machine), plus a .sha256 checksum.
#
# This does NOT tag, push, or alter the CHANGELOG — it only packages. See
# docs/RELEASING.md for the full tag-time runbook.
#
# Usage:  ./scripts/package-release.sh [--version X.Y.Z] [--no-build] [--jobs N]
#   --version   override the version string (default: read from CMakeLists.txt)
#   --no-build  package whatever is already in build/ (skip configure+build)
#   --jobs N    parallel build jobs (default: all cores)
# ============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"; cd "$REPO"

VERSION=""; NO_BUILD=0; JOBS="$(nproc 2>/dev/null || echo 4)"; BUILD_DIR="build"
while [[ $# -gt 0 ]]; do case "$1" in
  --version) VERSION="$2"; shift 2;;
  --no-build) NO_BUILD=1; shift;;
  --jobs) JOBS="$2"; shift 2;;
  --build-dir) BUILD_DIR="$2"; shift 2;;
  -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "Unknown option: $1"; exit 2;; esac; done

say(){ printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }

# version from CMakeLists.txt unless overridden
if [[ -z "$VERSION" ]]; then
  VERSION="$(grep -oE 'project\([A-Za-z0-9_]+ VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt \
             | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  [[ -n "$VERSION" ]] || die "could not read version from CMakeLists.txt (pass --version)"
fi
HASH="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
NAME="synth-${VERSION}-linux-x86_64"
say "Packaging $NAME  (git $HASH)"

# 1. build Release
if [[ "$NO_BUILD" == 0 ]]; then
  say "Building Release…"
  cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target VASynth_Standalone VASynth_VST3 -j "$JOBS"
fi
ART="$BUILD_DIR/VASynth_artefacts/Release"
[[ -x "$ART/Standalone/synth" ]] || die "standalone not built (drop --no-build)"
[[ -d "$ART/VST3/synth.vst3"   ]] || die "VST3 not built (drop --no-build)"

# 2. assemble the staging tree
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"; mkdir -p "$PKG/bin" "$PKG/vst3"
install -m0755 "$ART/Standalone/synth" "$PKG/bin/synth"
cp -a "$ART/VST3/synth.vst3" "$PKG/vst3/"
for f in README.md LICENSE CHANGELOG.md docs/INSTALL.md; do [[ -f "$f" ]] && cp -a "$f" "$PKG/"; done
printf 'synth %s\ngit %s\nbuilt %s\n' "$VERSION" "$HASH" "$(git show -s --format=%cd --date=short HEAD 2>/dev/null)" > "$PKG/VERSION.txt"

# 3. a copy-only installer for the prebuilt binaries (no build, no deps)
cat > "$PKG/install.sh" <<'INST'
#!/usr/bin/env bash
# Install these PREBUILT binaries (no build). VST3 -> ~/.vst3, standalone -> ~/.local/bin.
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VST3_DIR="${VST3_DIR:-$HOME/.vst3}"; BIN_DIR="${BIN_DIR:-$HOME/.local/bin}"
mkdir -p "$VST3_DIR" "$BIN_DIR"
rm -rf "$VST3_DIR/synth.vst3"; cp -a "$HERE/vst3/synth.vst3" "$VST3_DIR/"
install -m0755 "$HERE/bin/synth" "$BIN_DIR/synth"
APPS="$HOME/.local/share/applications"; mkdir -p "$APPS"
cat > "$APPS/synth.desktop" <<D
[Desktop Entry]
Type=Application
Name=synth
Comment=Virtual-analog polysynth (standalone)
Exec=$BIN_DIR/synth
Terminal=false
Categories=AudioVideo;Audio;Music;
D
echo "Installed: $BIN_DIR/synth  and  $VST3_DIR/synth.vst3"
echo "If 'synth' isn't found in a terminal, add ~/.local/bin to PATH."
INST
chmod +x "$PKG/install.sh"

# 4. tar + checksum
mkdir -p dist
TARBALL="dist/${NAME}.tar.gz"
say "Writing $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" "$NAME"
( cd dist && sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256" )
say "Done."
ls -lh "$TARBALL"; cat "dist/${NAME}.tar.gz.sha256"
echo "Contents:"; tar -tzf "$TARBALL" | sed 's/^/  /' | head -20
