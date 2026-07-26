#!/usr/bin/env bash
# ============================================================================
# synth — Linux uninstaller. Removes the installed standalone, VST3, and menu
# entry. Leaves your build tree and your user data (presets/kits/samples/logs
# under ~/.config/synth) untouched.
#
# Usage:  ./scripts/uninstall-linux.sh [--vst3-dir DIR] [--prefix DIR] [--purge]
#   --vst3-dir DIR   where the VST3 was installed (default ~/.vst3)
#   --prefix DIR     where the standalone was installed (default ~/.local)
#   --purge          ALSO delete your user data (~/.config/synth) — presets etc.
# ============================================================================
set -euo pipefail
VST3_DIR="${HOME}/.vst3"; PREFIX="${HOME}/.local"; PURGE=0
while [[ $# -gt 0 ]]; do case "$1" in
  --vst3-dir) VST3_DIR="$2"; shift 2;;
  --prefix)   PREFIX="$2"; shift 2;;
  --purge)    PURGE=1; shift;;
  -h|--help)  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "Unknown option: $1"; exit 2;; esac; done

say(){ printf '\033[1;36m==>\033[0m %s\n' "$*"; }
rm -f  "$PREFIX/bin/synth"                        && say "removed $PREFIX/bin/synth"
rm -rf "$VST3_DIR/synth.vst3"                     && say "removed $VST3_DIR/synth.vst3"
rm -f  "$HOME/.local/share/applications/synth.desktop" && say "removed menu entry"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

if [[ "$PURGE" == 1 ]]; then
  rm -rf "$HOME/.config/synth" && say "purged user data (~/.config/synth)"
else
  say "kept your user data (~/.config/synth). Use --purge to remove it too."
fi
say "Done."
