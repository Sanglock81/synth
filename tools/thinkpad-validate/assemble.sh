#!/usr/bin/env bash
# Assemble the self-contained ThinkPad validation bundle (run on the DEV box).
# Snapshots the JUCE-free DSP sources + the bench source into this folder so the
# whole tools/thinkpad-validate/ directory can be copied to the ThinkPad and built
# there with nothing but g++. Re-run after any DSP/bench change (e.g. Sub-phase 2).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

echo "Snapshotting DSP + bench from $repo ..."
rm -rf "$here/dsp" "$here/bench"
mkdir -p "$here/dsp" "$here/bench"
cp "$repo"/Source/DSP/*.h "$here/dsp/"
cp "$repo"/tests/bench/bench_engine.cpp "$here/bench/"

# Record provenance so the report can tie numbers to a commit.
{
  echo "assembled_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_commit=$(cd "$repo" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "git_describe=$(cd "$repo" && git describe --always --dirty 2>/dev/null || echo unknown)"
} > "$here/BUNDLE_INFO"

echo "Done. Bundle ready at: $here"
echo "  dsp/    $(ls "$here/dsp" | wc -l) headers"
echo "  bench/  bench_engine.cpp"
cat "$here/BUNDLE_INFO"
