#!/usr/bin/env bash
# bootstrap-linux.sh — one command from a clean checkout to a built Synth (VST3 + standalone).
# Installs the build dependencies, configures, and builds Release. Then run tools/install-vst3.sh
# to link the VST3 into your DAW.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> Installing build dependencies (sudo apt-get)"
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build git unzip \
    libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev \
    libcurl4-openssl-dev libwebkit2gtk-4.1-dev

echo "==> Configuring (Release)"
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo "==> Building (the first run fetches JUCE; expect a few minutes)"
cmake --build build -j"$(nproc)"

VST3="build/VASynth_artefacts/Release/VST3/Synth.vst3"
APP="build/VASynth_artefacts/Release/Standalone/Synth"
echo
echo "==> Done."
echo "    VST3:       $ROOT/$VST3"
echo "    Standalone: $ROOT/$APP"
echo
echo "Link the VST3 into your DAW's plugin folder:  ./tools/install-vst3.sh"
echo "Or run the standalone directly:               ./$APP"
