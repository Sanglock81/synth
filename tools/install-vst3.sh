#!/usr/bin/env bash
# install-vst3.sh — point the host's VST3 folder at the FRESH build output so the plugin a DAW
# loads can never silently freeze again.
#
# Background: on 2026-08 the DAW kept loading a stale "~/.vst3/VA Synth.vst3" — an orphan left
# behind when PRODUCT_NAME was renamed "VA Synth" -> "synth". Every build had been shipping fresh
# code to a NEW filename (synth.vst3) while the host rescanned the frozen old one. This script
# removes such orphans and SYMLINKS the host bundle to the build tree, so a symlink is always the
# latest Release build — there is no copied file left to go stale.
#
# Linux : symlink ~/.vst3/synth.vst3 -> build tree, and delete known stale orphans.
# Others: print where to drop the CI-built VST3 (we build locally on Linux only).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="$ROOT/build/VASynth_artefacts/Release/VST3/synth.vst3"
DEST_DIR="${VST3_DIR:-$HOME/.vst3}"

case "$(uname -s)" in
  Linux)
    if [[ ! -e "$BUNDLE" ]]; then
      echo "No build-tree VST3 at:" >&2
      echo "  $BUNDLE" >&2
      echo "Run ./run-all-checks.sh (or a Release build) first, then re-run this script." >&2
      exit 1
    fi
    mkdir -p "$DEST_DIR"

    # Remove stale orphans from an earlier PRODUCT_NAME so the DAW cannot rescan the frozen bundle.
    shopt -s nullglob
    for orphan in "$DEST_DIR/VA Synth.vst3"; do
      echo "removing stale orphan bundle: $orphan"
      rm -rf "$orphan"
    done
    shopt -u nullglob

    # Replace whatever is there (an old symlink, or a real copy left by COPY_PLUGIN_AFTER_BUILD)
    # with a fresh symlink. rm -rf first, else `ln -s` into an existing real dir nests inside it.
    rm -rf "$DEST_DIR/synth.vst3"
    ln -s "$BUNDLE" "$DEST_DIR/synth.vst3"
    echo "linked: $DEST_DIR/synth.vst3 -> $BUNDLE"
    echo
    echo "Rescan VST3 plugins in your DAW. Every future Release build is now live with no copy step."
    echo "Tip: build with -DVASYNTH_COPY_PLUGIN=OFF to skip the (now redundant) post-build copy."
    ;;
  *)
    cat <<'EOF'
This repo builds locally on Linux only. On Windows, take the VST3 from CI (built from the gated
commit by .github/workflows/build-test.yml):

  GitHub -> Actions -> "build-test" -> pick the green run -> Artifacts -> "synth-Windows"

Unzip its VST3/synth.vst3 bundle into your user VST3 folder, e.g.:

  %LOCALAPPDATA%\Programs\Common\VST3\        (per-user)
  C:\Program Files\Common Files\VST3\         (system, needs admin)

Then rescan in the DAW. Delete any older "VA Synth.vst3" so the host cannot load the stale orphan.
EOF
    ;;
esac
