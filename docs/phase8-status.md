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

Gate: **first CI run green on both OSes.** ← DEFERRED by the user ("hold on
pushing to GitHub … I want the system further along and 7 finished"). The 8A
commit is on local `master` (`29ab1ab`) and locally green (clean-build
run-all-checks 118/118 + pluginval strictness 8). `origin` is configured
(https://github.com/Sanglock81/synth.git). To resume the push later:
`gh auth refresh -h github.com -s workflow` (the token lacks the `workflow` scope
needed for `.github/workflows/*`), then `git push -u origin master`, then watch CI.
Windows compile/test breakage, if any, will surface only on that first CI run.

Open follow-ups (backlog, not 8A):
- Curated audio-device *selector UI* (README roadmap) — the just-works default +
  logging landed in the bug queue; the raw JUCE settings dialog is still the
  advanced view.

## Phase 7 (revised) — in progress (runs before 8B–8F)

Plan: `~/.claude/plans/ancient-sparking-crescent.md`. Cadence (user-chosen): gate +
commit + PAUSE for review after EACH sub-phase. Bug B → 7A → 7B → 7C → wrap-up.

### Bug B (step zero) — DONE, gated, committed
"Play (input) not restored after startup / settings open-close / preset switch /
focus loss-regain" — for BOTH QWERTY and MIDI controllers (user clarified: any input).
- QWERTY: the 30 Hz editor watchdog now RECLAIMS keyboard focus (free predicate
  `qwertyShouldReclaimFocus`, `PluginEditor.h`) whenever a transient thief (Load combo /
  settings dialog / Alt-Tab) took it — guarded against modal dialogs + text fields.
  Load combo `onChange` also restores focus.
- MIDI: `VASynthMidiHotplug` is now a `ChangeListener` on the AudioDeviceManager and
  re-asserts every present input on any setup change (`ensureAllInputsEnabled` via pure
  `AudioDeviceCuration::inputsNeedingEnable`). Consequence (documented): the input
  contract wins — a settings-dialog MIDI disable is overridden; every present controller
  plays.
- Tests: `tests/plugin/test_input_reliability.cpp` (focus-decision table, MIDI re-enable
  set logic, preset-switch keeps play path). Real focus/device behaviour → hands-on.
- Gate: release 121/121 + pluginval s8 green; sanitizers green. Committed locally.

### Pre-7A polish (user-requested) — DONE, committed
- master_gain excluded from all preset ops (Init/factory/user load + save) — it's a
  performance control the player owns (`PresetPolicy` in Parameters.h). `08e24ca`.
- Factory loudness pass: hot sustained patches (Dark Drone, Full Organ, Square Lead,
  Reese Bass) trimmed via osc levels to ~-33 dBFS RMS; sustained set now within ~4 dB.
  Percussive/evolving patches (Pluck, Bell, Riser, E-Piano) deliberately left (RMS is
  the wrong metric). `9e23da4`. NOTE: user may still want Dark Drone restored to
  "biggest" and/or E-Piano nudged up — open for their ear.

### 7A — env→pitch + drums — DONE (gate in progress), then PAUSE
- New `fltenv_to_pitch` param (±48 st, default 0): the filter/mod env drives pitch
  (summed with LFO pitch mod in SynthVoice; control-rate via `ADSREnvelope::getLevel`).
  Default 0 → engine golden bit-identical. Filter-Env UI bank relabeled **MOD ENV** +
  a Pitch fader (screenshot regenerated).
- 6 drum presets (new **Drums** category): Kick 808 / Kick Punchy / Snare / Hat Closed
  / Hat Open / Tom, all using the pitch-env drop. Recipes in presets.md + README.
- Tests: `[envpitch]` (held +12 = 880 Hz octave, decays to note; default 0 inert),
  `[drums]` (kick low+descending+percussive, hat highpassed+short), factory count → 22.
- Gate: release 125/125 + pluginval s8, sanitizer ASan+LSan+UBSan + soak — all green.
  Committed locally; PAUSED for user review before 7B.

### 7B — diatonic chord engine — DONE, gated (release 144/144 + sanitizers green), then PAUSE
- `Source/DSP/ChordEngine.h` (JUCE-free, POD): diatonic triads (Major/Natural Minor,
  any root), forcers MAJ/MIN/SUS4/SUS2/DIM/DOM7 (latest-held wins via a stack), 7TH
  (diatonic seventh, or follows the forcer), out-of-scale + chord-OFF passthrough,
  and a note-indexed LEDGER (note-off replays the note-on tones; re-press releases
  only the changed tones — no stuck notes).
- `Source/ModifierLearnManager.h` (parallel to MidiLearnManager): learn a modifier
  from a CC (>=64) or a consumed note; persisted as a MODIFIERLEARN state child.
- Params: chord_enabled/chord_root/chord_scale (added to randomize exclusions).
- Processor integration (dispatch loop): note-on expands to the chord, note-off
  replays the ledger; modifier CC/note intercepted before MIDI-learn; chord FORCES
  poly; sustain holds chord tones (expansion precedes sustain); QWERTY modifiers via
  an atomic mask diffed on the audio thread into the forcer stack. Modifier handling
  gated on chord-ON. Chord OFF = passthrough (golden-safe; all existing tests green).
- UI: compact CHORD section (enable/root/scale + 7 modifier indicators with
  learn badges + QWERTY hint), between GLOBAL and FX. User signed off (tightened
  grid). Screenshot regenerated.
- QWERTY reserved row: C=MAJ V=MIN B=7TH N=DOM7 M=SUS4 ,=SUS2 .=DIM (/ spare).
- Tests: dsp/test_chord (grammar tables C maj / A min / E maj / F min, forcers,
  latest-wins, ledger, passthrough); plugin/test_chord_plugin (expansion,
  forces-poly, modifier mask, learn from CC + consumed note, persistence, sustain,
  RT-alloc). NOTE (voice cap): a 4-key held chord = up to 16 voices -> the pool
  saturates and oldest-steal is exercised hard; report at 7C bench, cap unchanged.

### 7C — multi-surface routing + parts — DONE, gated (release 152/152 + sanitizers green), then PAUSE
- Engine: up to 4 parts. SynthVoice carries a `part` index; `SynthEngine::render`
  selects per-voice params via the `paramsFor(part, note)` SEAM (note unused in v1 —
  the forward-compat seam a future Kit part specializes per-note, per the user's
  amendment). Part 0 = smoothed LIVE; parts 1-3 = baked, published lock-free via a
  double buffer (`setLockedPartParams`). One shared 16-voice pool, global steal.
  Golden bit-identical (single-part default path unchanged).
- Bake: `snapshotParams` refactored to a static `buildVoiceParams(const APVTS&)`;
  `setPartPreset` bakes a factory/user/Init preset via a throwaway BakeProcessor
  (scratch APVTS) — reuses the kill-fold so a locked part is bit-identical to loading
  live. Missing preset -> Init + logged warning.
- Routing: surface->part table + locked-part preset names persist as a PARTS state
  child. Routed-MIDI FIFO (raw <=3-byte msg + part), multi-producer push under a
  SpinLock, lock-free audio-thread drain; note/CC/pitch-bend handling shared with the
  host `midi` path (`handleControlMessage`). Standalone: per-input capture replaces
  the holder player's all-device merge (no double-trigger); QWERTY stays LIVE (reroute
  deferred). INPUTS modal dialog (routing + preset picker + activity dots) + button.
- Tests: dsp `[7c][parts]` (locked part renders with its own params); plugin
  `[7c]` (bake equivalence, 3-source contract, voice-level isolation, persistence
  round-trip, missing-preset fallback, glitch-free reassignment, routed-path RT-alloc).
- CPU: 7C adds NO new cost — 4-part worst case = the existing 16-voice + shared-FX
  case (shared FX/LFO; O(1) paramsFor). Bench blocked at powersave governor (invalid
  per [[vasynth-bench-governor]]); escalated to the user — on-ThinkPad remains the gate.

## 8B–8F — not started (blocked on Phase 7)

(To be filled in as each sub-phase runs.)
