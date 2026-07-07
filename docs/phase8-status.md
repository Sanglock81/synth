# Phase 8 status — living handoff

Update this at every sub-phase boundary so work can resume cleanly in a fresh
session. Phase 8 runs in six gated sub-phases, strictly in order.

## Ordering note (important)

Phase 8 was started **out of the normal order** at the user's explicit direction:
**8A (infrastructure) was done first, before Phase 7**, because 8A is
Phase-7-independent and the user wanted CI running before the big feature work.
The agreed sequence from here:

1. ✅ **8A — GitHub + CI** (this doc's current state).
2. ⏳ **Phase 7** — env→pitch + drums, diatonic chord engine, input routing +
   parts (multitimbral-lite). The user holds the Phase 7 prompt; it runs next, in
   strict order, under the CI that 8A establishes.
3. ⏳ **8B–8F** — full multitimbral, wavetable, rhythm engine, recording, Windows
   bring-up. **Blocked on Phase 7** (8B upgrades Phase 7C's parts; 8D chains the
   chord engine → rhythm engine).

## 8A — GitHub + CI  — STATUS: complete pending first green CI run

Decisions made:
- **Project renamed `VA Synth` → `synth`** (user's call: "just name it synth").
  - User-facing only where it matters: `PRODUCT_NAME`/`JucePlugin_Name` → `synth`
    (VST3 is `synth.vst3`, standalone is `synth`, window title `synth`),
    `getName()` → `synth`, config dirs → `~/.config/synth`.
  - **Internal CMake identifiers kept** (`VASynth` target, `VASynthData`,
    `VASynth_artefacts/` build path, `VASYNTH_*` options) — invisible to users,
    renaming them is pure churn/risk. `Source/` filenames and C++ class names
    (`VASynthProcessor`, etc.) also unchanged for the same reason.
  - **One-time data migration** (`Source/AppInfo.h`): on first run, presets +
    MIDI profiles under `~/.config/VASynth` are copied to `~/.config/synth`
    (per-subdir, idempotent); the standalone's `VA Synth.settings` → `synth.settings`.
    Verified on the dev box (user's real presets carried over) and unit-tested
    with temp dirs (`test_migration.cpp` "[8a][migration]").
- **License: AGPLv3** (not GPLv3) — matches JUCE 8's open-source terms. `LICENSE`
  is the canonical FSF AGPL-3.0 text; README has a Licensing section explaining why.
- **CI** (`.github/workflows/`):
  - `build-test.yml`: matrix {ubuntu-latest, windows-latest}, Release build of both
    artefacts + full ctest; Linux also runs pluginval strictness 8. FetchContent
    (JUCE/Catch2) cached via `FETCHCONTENT_BASE_DIR=.deps`. Uploads zipped
    VST3+Standalone per OS on the default branch (`main`/`master`).
    - LTO is **OFF in CI** for speed/reliability (`-DVASYNTH_ENABLE_LTO=OFF`); still
      a full Release build. Local `run-all-checks.sh` keeps LTO on.
    - `-DVASYNTH_COPY_PLUGIN=OFF` in CI (new option, default ON) so the after-build
      VST3 copy doesn't fail on the Windows runner's protected system VST3 dir.
    - Linux ctest runs under `xvfb-run` (headless GUI).
  - `sanitize.yml`: Linux ASan+LSan+UBSan + soak via `run-all-checks.sh --sanitize`,
    **nightly + on-demand** (too slow for every push). `run-all-checks.sh` now
    honours `FETCHCONTENT_BASE_DIR`.
- **Windows portability audit**: no hardcoded Unix paths (the one `~/.config`
  literal is `#if JUCE_LINUX`-guarded); all config paths via `juce::File` special
  locations; no `-Werror`/`/WX` (MSVC warnings won't fail the build); the
  Linux-only `test_audio_device` ALSA assertion is now per-platform (ALSA / Windows
  Audio / CoreAudio). Real Windows compile/test issues (if any) are for CI to
  surface on the first run — fix them there.

Gate: **first CI run green on both OSes.** ← in progress / to confirm after push.

Open follow-ups (backlog, not 8A):
- Curated audio-device *selector UI* (README roadmap) — the just-works default +
  logging landed in the bug queue; the raw JUCE settings dialog is still the
  advanced view.

## 8B–8F — not started (blocked on Phase 7)

(To be filled in as each sub-phase runs.)
