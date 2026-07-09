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

### Routing discoverability (Part A of the routing follow-up) — DONE, gated
- **Reported root cause of "can't see it":** NOT a stale binary (verified: shipped
  binary contained the routing strings + current mtime). It was **hidden-in-plain-sight**
  — the only entry point was a small "Inputs" text button stacked with Random/Save/Load
  in the Global corner. Compounded by one **broken sub-path**: the dialog offered QWERTY
  routing but QWERTY was hardwired to LIVE (reroute lands in Part B via zones).
- **Build provenance:** startup banner now prints git hash + `__DATE__ __TIME__` +
  build type + parts count, so "old binary" is a one-line check against
  `git rev-parse --short HEAD`. (`CMakeLists.txt` VASYNTH_GIT_HASH/BUILD_TYPE.)
- **Visibility fix (minimal, not the deferred GUI overhaul):** always-visible **PARTS
  strip** across the editor top (`Source/UI/PartsStrip.h`) — cells P0 LIVE / P1-P3
  (`--` unused, preset name if locked) with per-part note-activity flicker
  (`partHits` atomic array + `partActivity()`), and a prominent teal **INPUTS** button
  at the right. The small Global-corner Inputs button was removed.
- **Function audit:** integration test drives the dialog's OWN action handler
  (`InputsDialog::applyRouting`, shared by both combos' onChange) — assign a surface to
  Part 1 + preset → a note from that surface renders with Part 1 params + the strip
  flickers (`test_parts.cpp [dialog]`). Standalone real-device listing verified: the
  dialog renders a named row per present MIDI input (a real Launchkey Mini MK3 appears)
  → `docs/inputs-dialog.png` (screenshot test).
- **Docs:** README gained a numbered click-path (PARTS strip → INPUTS → route → preset →
  activity dot → play) + the build-provenance check. `docs/editor.png` shows the strip;
  `docs/inputs-dialog.png` shows a configured dialog.

### Key-range zones + routing lifecycle (Part B) — DONE, gated
- **Lifecycle rule 2 (the reported bug):** routing/zones now RESET to default on
  relaunch — ordinary plugin state persists SOUND only (removed the 7C PARTS child
  from get/setStateInformation). The standalone persists via savePluginState()/reload,
  which use exactly those calls, so app-close/reopen now resets. See
  [[vasynth-routing-lifecycle]] (the user reversed my earlier misread — they want RESET).
- **Zones engine:** per-surface ordered, gapless zone list `{lo,hi,part,transpose}` tiling
  [0,127] (default = one full-range LIVE zone). `routeSurfaceMessage(surface,msg)` resolves
  a note's zone -> part + transpose (clamped), records a note-off **ledger** so a note-off
  releases exactly what its note-on triggered even after a re-split. All resolution is off
  the audio thread (FIFO delivers resolved events -> no new audio-thread lock).
  setSurfaceZones normalises any input to a contiguous tiling. addSurfaceSplit /
  removeSurfaceSplit / resetSurfaceZones / resetAllRouting.
- **QWERTY** is now a routed surface too (routeSurfaceMessage("QWERTY",...)); removed the
  qwertyKeyboardState merge. Splittable/transposable like any controller. Bug B focus logic
  untouched; QWERTY-feeding tests migrated.
- **MULTI save/load:** named XML layout (AppInfo::multiDir()) of parts + surface zones,
  applied only on explicit load; a zone on a missing-preset part falls back to LIVE
  (setPartPreset returns ok). captureMultiState/applyMultiState shared format.
- **Zone UI:** INPUTS dialog rebuilt — per-surface SPLIT expander with a part-coloured
  segmented bar, per-zone part/preset/transpose/remove, + Split, Split by play (arm ->
  next note seams via lastNoteForSurface), Reset surface; bottom bar Reset all routing +
  Save/Load MULTI (labelled "includes the whole layout"). Viewport-scrolled; all controls
  refuse focus (focus test green). docs/inputs-dialog.png shows a QWERTY bottom-octave
  bass split open.
- Tests [partsB]: reset-on-round-trip, default=LIVE, key-range split, transpose+clamp,
  note-off ledger across a re-split, normalisation, add/remove split, split-by-play last
  note, QWERTY split, MULTI round-trip + missing-preset fallback + clears-unnamed. Full
  plugin suite green (98 cases). README routing section rewritten around the lifecycle
  contract + zones + MULTI.

### Sub-phase 1 — KIT PARTS — DONE, gated
- **Engine:** a part can be a KIT (Kit.h POD: 16 pads {trigger, soundNote[1..4], numSound,
  chokeGroup} + baked VoiceParams per pad). Specialises the paramsFor(part, slot) seam —
  voices carry a soundSlot (pad index); non-kit slot 0. Double-buffered publish like a
  locked part; per-block snapshotted read index (no tearing). kitNoteOn expands trigger ->
  sounding notes (decoupled pitch; 2..4 = chord pad) with a note-off ledger; choke groups
  quick-release the group (self-choke = retrigger); unmapped trigger = silence. Pad level
  folds into VoiceParams.gain (default 1.0 -> goldens bit-identical). ADSR quickRelease now
  carries a choke flag so a choke stays fast despite the per-render setParameters (fixes a
  long-tailed sound not being cut fast).
- **Processor:** KitDefinition (source presets per pad) -> bake via shared bakePresetParams
  -> setPartKit publishes. Dispatch routes a kit part's notes to kitNoteOn/Off. Kit presets
  (kitToTree/kitFromTree; user *.kit under kitDir; factory "808 Basics" + "Stab Board").
  MULTI serialises a kit part as a KIT child. Missing source -> Init, logged.
- **UI:** Kit Editor modal (open by clicking a locked part cell on the PARTS strip): 4x4
  pad grid with live flicker, per-pad trigger/source/sounding(learn-by-play)/level/choke,
  audition, load/save. Refuses focus (QWERTY drives learn-by-play). docs/kit-editor.png.
- **Tests:** dsp/test_kit (per-pad params, unmapped=silence, pitch decoupling, chord pad
  on/off, choke in/cross/same-pad, gain math, choke torture click-free, clearPartKit);
  plugin [kitpart] (dispatch, missing-source fallback, RT-alloc + glitch-free publish,
  factory kits, kit-preset + kit-in-MULTI round-trips, focus, screenshot). Goldens
  bit-identical (kit off / gain 1.0).
- **Docs:** README kit paragraph + docs/presets.md Kits section (model, choke, factory
  kits, seam note).
- **Bench/CPU:** kit adds no new per-block audio cost. dsp_bench "kit (4 live + 12 pads =
  16 voices) + ALL FX" p99 0.358 ms vs plain 16-voice 0.345 ms — statistically identical.
  Governor = **powersave** (memory: ~2x inflated, invalid gate); ThinkPad-derate shows
  ~47% budget at powersave, so ~23% at the performance governor. Same already-accepted
  16-voice worst case as 6B/7C — kit introduces no new cost. Real gate stays on-ThinkPad.

### Sub-phase 2 — FULL MULTITIMBRAL (per-part FX + LFO) — code complete, at CPU gate
- **Per-part FX (increment 1):** engine renders each part into its own buffer -> its own
  FXChain -> master sum. New stereo path split begin/renderParts/mixParts (host MIDI stays
  sample-accurate). Silent parts with idle FX are skipped (partsProcessed counter; a
  decaying tail keeps processing until silent). Mono render() untouched -> goldens exact.
- **Per-part LFOs (increment 2):** lfo2_*/lfo3_* params; 3 LFOs x maxParts; each part's
  three LFOs sum per destination and modulate only that part. Shared bend + mod-wheel
  vibrato still global. LFOs advance only for parts with active voices.
- **Bake (increment 3):** locked parts bake FX + LFOs from their source preset; published
  WITH the voice params in one LockedSlot buffer (race-free). Kit parts dry in v1.
- **UI (increment 4):** LFO 1/2/3 sections on the panel (space-tight -> sign-off).
- **Mixer:** per-part level/pan DEFERRED (see below); parts sum at unity/centre.
- **Tests:** dsp/test_multitimbral (delay isolation, silent-part skip, FX-tail keep-alive,
  per-part LFO independence); plugin [partsB2] (locked bake carries the preset's reverb
  tail). Full suites green: dsp 81 (goldens bit-identical), plugin 107. RT-safe.
- **CPU GATE (hard stop — FLAGGED, exceeds target at the assumed x3.5 derate):**
  Measured at the **performance** governor (valid), ThinkPad-derated x3.5, load-robust
  **p50** (p99 inflated by a running browser):
    - 16 voices Efficient, no FX ...... ~33% budget
    - 1 part, 16 voices + ALL FX ...... ~46%
    - 4 parts x4v + 4x ALL FX (worst) . ~52%   (p99 ~85%)
  Over the ~30% target. Notes: (1) the DEV BOX raw is comfortable (4-part p50 0.40 ms =
  15% of the 2.667 ms budget) — the overage is entirely the assumed x3.5 derate; (2) NOT a
  Sub-phase 2 regression in kind — per-part FX adds only ~7 pts over the pre-existing
  1-chain-FX case (silent-skip works); the bulk (voices + FX) predates this since 6B;
  (3) the powersave-vs-performance comparison was load-confounded (perf ~ powersave here),
  so the assumed x3.5 derate itself is unverified. The **ThinkPad report** (validate.sh)
  is the authoritative arbiter of the real derate. FLAGGED to the user with the pre-agreed
  options: (a) part-count / voice default for the live target, (b) shared-reverb mode
  (reverb is the priciest FX; sharing it cuts the per-part multiplication), (c) documented
  guidance. Decision deferred to the user + the ThinkPad report — not absorbed.

### Deferred / future features
- **Per-part mixer** (`partN_level`/`partN_pan` + MIX strip to balance each part's volume
  and pan) — deferred by the user (2026-07-08) after kit hands-on. Sub-phase 2 proceeds
  WITHOUT it: per-part FX + per-part LFOs only; parts sum at unity/centre (the safety
  clipper handles multi-part peaks). Revisit later; frozen IDs allow adding it then.

## 8B–8F — not started (blocked on Phase 7)

(To be filled in as each sub-phase runs.)
