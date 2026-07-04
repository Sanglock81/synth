# VA Synth — Phase 0 Gap Analysis

_Date: 2026-07-04. Scope: every file in `Source/` and `tests/`, plus `CMakeLists.txt`
and `README.md`'s v1 roadmap. This is a review-only pass; no code was changed._

Legend for the task checkboxes at the bottom: `[ ]` open, `[x]` done, `[~]` in progress.

---

## 1. Per-file assessment

### `Source/DSP/PolyBlepOscillator.h` — mostly good, triangle is weak
- **Works:** Saw and Square PolyBLEP are the standard Martin-Finke/Välimäki 2-point
  residual and look correct. Square gets PWM by placing the second BLEP at the
  pulse-width edge. Sine is a plain `sin`. `polyBlep` guards against `dt==0`
  implicitly (no division taken when `phaseInc==0`).
- **Suspect:**
  - **Triangle amplitude is pitch- and sample-rate-dependent.** The triangle is a
    leaky integrator of the BLEP'd square: `triState = 0.995*triState + 4*phaseInc*sq`.
    The `0.995` leak is a fixed per-sample coefficient not scaled by sample rate, and
    at low fundamentals the half-cycle spans thousands of samples, so `0.995^N`
    bleeds most of the signal away. At 20 Hz / 48 kHz the half-cycle is ~1200 samples
    → `0.995^1200 ≈ 0.0025`: the triangle nearly vanishes and distorts. Bounded, but
    quiet/wrong. **Correctness/quality risk.**
  - **Osc phase is reset to 0 on every `noteOn`** (see SynthVoice) → a hard waveform
    discontinuity on retrigger and on voice-steal, which defeats the click-free
    envelope design. Tracked under SynthVoice/ADSR below.
  - Very high `phaseInc` (dt > 0.5) would make the two BLEP regions overlap, but v1's
    8 kHz max at 48 kHz gives dt≈0.17 — fine. Noted for completeness.

### `Source/DSP/SVFilter.h` — solid
- **Works:** Cytomic TPT SVF, the correct stable topology. Cutoff clamped to
  `[20, sr*0.49]`, resonance to `[0, 0.98]` — no blow-up at extremes. LP/HP/BP/Notch
  from shared state.
- **Suspect:**
  - **No denormal protection in the DSP itself.** `ic1eq/ic2eq` decay toward
    denormals when input goes silent. `processBlock` wraps rendering in
    `ScopedNoDenormals`, so the *plugin* is protected — but the **JUCE-free DSP tests
    are not**, so a "10 s of noise then silence" test can hit denormal slowdown (a
    perf issue, not a correctness one). Consider a tiny denormal-flush in `process`
    or the test, so `Source/DSP/` is self-protecting.
  - `setCutoff` is called **per sample** from the voice (with `std::tan`) — correct
    but the single most expensive thing in the inner loop. The voice already flags
    `TODO(perf): recompute every 8–16 samples`. Not a bug.

### `Source/DSP/ADSREnvelope.h` — timing calibration is off; retrigger caveat
- **Works:** One-pole exponential segments with an overshoot target (1.3) so attack
  reaches 1.0 in finite time. `noteOn` retriggers from the current level (no envelope
  jump). Stage machine is clean.
- **Suspect — timing does not match nominal seconds (test-failure risk):**
  The segment time constant is `tau = seconds * 0.3`. Consequences vs. the Phase 2
  required tests:
  - **Release to −80 dB (1e-4):** `t = 0.3*R*ln(level0/1e-4)`. For a sustain of 0.8,
    `t ≈ 2.7*R` — **exceeds the `release-time × 2` bound.** Test will fail for any
    normal sustain level.
  - **`steal()`/`quickRelease()` "within 10 ms":** sets `tau = 0.005*0.3 = 1.5 ms`,
    runs to level ≤ 1e-5 → `t ≈ 0.0015*ln(0.8/1e-5) ≈ 17 ms` — **exceeds 10 ms.**
  - Attack is the opposite — reaches 1.0 in ~`0.44*attackTime`, comfortably inside
    the `×1.5` bound, but shows the calibration is loose.
  Fix direction: re-derive `coefForTime` so a segment's nominal seconds correspond to
  a defined convergence (e.g. reach within −60/−80 dB in `seconds`), and make
  `quickRelease` fast enough (or terminate on a higher floor) to complete < 10 ms.
- **Retrigger discontinuity is not in the envelope but in the voice:** the envelope
  is click-free by itself; the click comes from `SynthVoice::noteOn` calling
  `osc.reset()`/`filter.reset()`. See below.

### `Source/DSP/LFO.h` — fine for v1
- **Works:** Tri/Sine/Square/S&H all bounded correctly ([-1,1]); phase-accurate rate;
  `std::minstd_rand` + `uniform_real_distribution` are allocation-free after
  construction (RT-safe). Deterministic (seeded).
- **Suspect:**
  - **S&H "once per cycle" can miss** when `phaseInc*numSamples > 1.0` (LFO period
    shorter than the render chunk): the `phase < oldPhase` wrap test fires at most once
    per call, so multiple wraps in one chunk yield one new value. Only matters for very
    fast LFOs / large blocks; acceptable for v1 but note it for the S&H test (drive it
    with small chunks).
  - Value is sampled at the **end** phase of each chunk (control rate). Fine for v1.

### `Source/DSP/SynthVoice.h` — the click risk lives here
- **Works:** Clean POD `VoiceParams` snapshot design; voices hold no param state.
  xorshift noise is allocation-free. Cutoff modulation (env amount + keytrack + LFO)
  in octaves is sensible.
- **Suspect:**
  - **`noteOn` resets oscillator phase and filter state every time.** On retrigger of
    a held note and on voice-steal, this produces a hard discontinuity → click. The
    "click-free retrigger" and "seamless steal" claims are undermined. For steal
    specifically, the engine calls `steal()` (arm quick-release) then **immediately**
    `noteOn()` (which resets phase and restarts attack), so the fade never actually
    plays. Needs rework (steal should fade to silence *before* the voice is reused, or
    use a separate steal-fade gain).
  - `midiNote - 60` keytrack pivot and `-69` tuning pivot are inconsistent constants
    but both intentional (keytrack pivots at middle C, tuning at A440). OK.
  - **No glide/portamento** (`TODO`), **no mono/legato** — v1 roadmap.
  - Filter `setCutoff` per sample (perf `TODO`, not a bug).

### `Source/DSP/SynthEngine.h` — correct allocation policy, steal caveat
- **Works:** Fixed `std::array<SynthVoice,16>`, no growth. Oldest-note stealing by
  timestamp. Retrigger reuse of a voice already playing the note. `render` takes
  `VoiceParams` **by value** (POD copy on the stack — no allocation). LFO routing to
  pitch/cutoff/PW. Determinism holds for fresh engine instances.
- **Suspect:**
  - Steal path calls `steal()` then `noteOn()` back-to-back (see SynthVoice) — the
    quick-release is immediately overwritten by the retrigger. **Click risk on the
    17-voices-on-16 test.**
  - **`allNotesOff` / sustain interaction:** no sustain-pedal state here; when we add
    CC64 hold, note-offs must be deferred at this layer (or the voice layer). Design
    now so pedal-held notes release on pedal-up.
  - LFO advanced per render-chunk; with sample-accurate MIDI splitting a block into
    several `render` calls, the LFO steps at chunk boundaries. Acceptable v1.

### `Source/Parameters.h` — good; IDs frozen
- **Works:** Single source of truth; every param in the APVTS; musical skews on
  time/cutoff/rate ranges; `ParameterID{..., 1}` version hints present. IDs are the
  contract — **add, never rename.**
- **Note:** `glide_time` and `poly_mode` params exist but are **not wired** to the
  engine yet (Phase 3 mono/glide). `filter_env_amt` default 0.3 etc. all reasonable.

### `Source/MidiLearnManager.h` — **real-time safety violation**
- **Works:** Try-lock on the audio thread (non-blocking, drops a CC under contention —
  acceptable). Default Launchkey map (CC 21–28). `setValueNotifyingHost` keeps
  GUI/host in sync.
- **Suspect (RT-safety):**
  - **`std::map` allocates on the audio thread.** In learn mode,
    `mappings[ccNumber] = learnTarget` inserts a node → heap allocation on the audio
    thread. `juce::String` assignment/`clear()` is ref-counted and can allocate/free.
    Both are reachable from `processBlock`. **Must be made lock-free / allocation-free**
    (e.g. a preallocated `std::array<std::atomic<int>,128>` of CC→param-index, and pass
    learn arming/results via atomics or an SPSC FIFO).
  - **Persistence TODO not implemented** — custom maps don't survive a state
    round-trip (v1 roadmap item; also a Phase 2 test requirement).
  - `setValueNotifyingHost` from the audio thread is a known JUCE gray area (can
    trigger async host/GUI work); acceptable for v1 but keep in mind.

### `Source/PluginProcessor.{h,cpp}` — **RT-safety + missing MIDI**
- **Works:** Correct sample-accurate MIDI dispatch (render up to each event, then
  handle it). `ScopedNoDenormals`. APVTS atomic reads via `getRawParameterValue`.
  State save/load via XML. Mono→stereo with master gain.
- **Suspect:**
  - **`monoScratch.setSize(...)` is called in `processBlock`.** With
    `avoidReallocating=true` it won't realloc *as long as* the block never exceeds
    `samplesPerBlock` from `prepareToPlay` — but that's not guaranteed by all hosts,
    and calling `setSize` at all on the audio thread is fragile. **Preallocate at max
    block size in `prepareToPlay`; never resize in `processBlock`.**
  - **Missing MIDI:** no **pitch bend**, no **mod wheel (CC1)**, no **sustain pedal
    (CC64)** handling. CC1/CC64 currently fall into `midiLearn.handleCC`, aren't in the
    default map, and are silently dropped → **sustain pedal does nothing** (critical:
    the Korg B2's damper is the primary expression control), mod wheel does nothing,
    pitch bend does nothing. All three are Phase 3.
  - **Master gain / oscMix / cutoff / resonance are stepped per block** (no smoothing)
    → **zipper noise** on knob moves and automation.
  - **Perf (not a violation):** `getRawParameterValue(id)` does a string-keyed lookup
    ~30× per block. Cache the `std::atomic<float>*` pointers once. Optional.
  - `isBusesLayoutSupported` not implemented — pluginval at high strictness may probe
    odd layouts; may need a guard to reject non-stereo-out. Verify during Phase 2.

### `Source/PluginEditor.{h,cpp}` — fine for v1
- Thin wrapper over `GenericAudioProcessorEditor`. Header-only editor with a stub
  `.cpp` so CMake has a TU. No issues; custom GUI is v2.

### `tests/dsp_smoke_test.cpp` + `tests/README.md` — starting point only
- A single hand-run `main()` that renders 1 s and checks peak/finite bounds. No
  framework, not wired to CTest. **Port into Catch2** (Phase 2) and keep the intent as
  one smoke case.

### `CMakeLists.txt` — builds the plugin, no tests yet
- **Works:** `juce_add_plugin` with `FORMATS VST3 Standalone`, JUCE 8.0.4 pinned,
  recommended warning/LTO flags enabled (we must **not** suppress). `JUCE_WEB_BROWSER=0`
  / `JUCE_USE_CURL=0` → webkit/curl deps effectively optional.
- **Gaps:** no test target, no CTest, no Catch2, no pluginval. Added in Phase 2.
  DSP tests must compile **without JUCE** — keep them in a separate target that only
  includes `Source/DSP/`.

---

## 2. Cross-cutting findings by category

### A. DSP correctness risks
1. **Triangle amplitude droop** at low frequency + sample-rate-dependent leak (osc). _[med]_
2. **ADSR segment timing** doesn't match nominal seconds; release-to-−80 dB and
   `steal()<10 ms` will fail the Phase 2 spec as written. _[high — blocks tests]_
3. **Oscillator/filter reset on `noteOn`** → click on retrigger and steal. _[high]_
4. **No denormal protection inside `Source/DSP/`** (plugin is covered by
   `ScopedNoDenormals`; the JUCE-free tests are not). _[low/med]_
5. **S&H may skip** for LFO period shorter than the render chunk. _[low]_

### B. Real-time-safety violations (reachable from `processBlock`)
1. **`MidiLearnManager::handleCC`**: `std::map::operator[]` insert + `juce::String`
   assignment allocate on the audio thread (learn path). _[high]_
2. **`monoScratch.setSize` in `processBlock`**: potential realloc if block >
   prepared size; resizing on the audio thread at all is fragile. _[high]_
3. `setValueNotifyingHost` from audio thread — gray area, keep. _[watch]_
4. Per-block string-keyed param lookups — not unsafe, just wasteful. _[perf]_

### C. Missing MIDI handling
1. **Sustain pedal CC64** — not handled; the primary keyboard's damper is dead. _[high]_
2. **Pitch bend** — not handled. _[high]_
3. **Mod wheel CC1 → vibrato depth** — not handled. _[high]_
4. All-notes-off / all-sound-off — handled. Note-on vel 0 → JUCE maps to note-off. OK.
5. Running status — JUCE `MidiBuffer` already decodes it; nothing to do.
6. On `prepareToPlay`/transport reset, we should force `allNotesOff` + clear pedal.

### D. Parameter smoothing gaps (zipper noise)
- **Cutoff, resonance, master gain, osc mix** are applied as per-block steps. Need
  one-pole smoothers (per-sample or per-small-chunk). _[high — roadmap + test]_

### E. README v1 roadmap status
- [ ] Verify PolyBLEP by ear/spectrogram → replaced by the automated aliasing test.
- [ ] Pitch bend + mod wheel + sustain pedal — **not implemented.**
- [ ] Mono/legato + glide — **not implemented** (`TODO`s in place).
- [ ] Per-block parameter smoothing — **not implemented.**
- [ ] Persist MIDI-learn mappings in APVTS state — **not implemented** (`TODO`).

---

## 3. Prioritized task list

Order follows the mandated sequence: **build works → tests pass → correctness fixes →
v1 roadmap features.** Test-first for every fix (write the failing test, then fix).

### P1 — Build (Phase 1)
- [ ] Install missing deps (cmake, jack, freetype, fontconfig, xinerama, xcursor, curl).
- [ ] Configure + build Release; fix all compile errors in the JUCE-facing files.
- [ ] Build **both** Standalone and VST3; zero new warnings (no suppression).

### P2 — Test harness (Phase 2, non-negotiable)
- [ ] Catch2 via FetchContent, wired into CTest.
- [ ] JUCE-free DSP test target (includes only `Source/DSP/`).
- [ ] Port `dsp_smoke_test` into the framework.
- [ ] Oscillator tests: boundedness (all 4 waves, 20 Hz–8 kHz), phase continuity,
      **3 kHz saw aliasing < −60 dB** (the soul test), square PWM 0.1/0.5/0.9.
- [ ] Filter tests: 10 s white-noise stability over type×cutoff×resonance grid;
      LP/HP frequency-response sanity; full-range cutoff sweep in one block.
- [ ] Envelope tests: attack `≤ ×1.5`, release `≤ −80 dB in ×2`, retrigger continuity
      (Δ < 0.1), `steal() < 10 ms`. **(Expect failures → drives finding A2.)**
- [ ] LFO tests: boundedness, S&H once/cycle, rate accuracy ≤ 1 %.
- [ ] Voice/Engine tests: determinism (bit-exact), note lifecycle frees voice,
      17-on-16 steal without click, silence-in→silence-out.
- [ ] RT-safety: 1000-block render with **zero heap allocations** in `render`
      (override global `new`/counting hook).
- [ ] Plugin layer: pluginval strictness ≥ 8; state round-trip; MIDI-learn (mapped CC
      moves target, learn binds new CC, survives round-trip).
- [ ] Golden render: 2 s chord + filter-sweep → committed WAV, tolerance compare.
- [ ] `./run-all-checks.sh`: build both configs + ctest + pluginval; nonzero on failure.

### P3 — Correctness fixes (Phase 3, test-first)
- [ ] **RT-safety:** preallocate `monoScratch`; lock-free/alloc-free `MidiLearnManager`.
- [ ] **ADSR timing** re-calibration so nominal seconds match; `quickRelease` < 10 ms.
- [ ] **Click-free steal/retrigger:** stop resetting osc phase on retrigger; make steal
      fade to silence before reuse (or steal-fade gain).
- [ ] **Denormal safety** inside `Source/DSP/` (or documented in tests).
- [ ] **Triangle** amplitude/leak fix (sample-rate-scaled leak or DC-blocker).
- [ ] **Parameter smoothing:** one-pole on cutoff/reso/gain/oscMix; zipper test first.

### P4 — v1 roadmap features (Phase 3)
- [ ] Pitch bend (±2 semis default) — from any device.
- [ ] Mod wheel CC1 → vibrato depth.
- [ ] Sustain pedal CC64 → hold note-offs (defer release until pedal-up).
- [ ] Mono/legato modes + glide (slew note freq in `SynthVoice`).
- [ ] MIDI-learn persistence into APVTS state.

---

## 4. Notes on priorities / ambiguity

Nothing here is ambiguous enough to block on. The one judgment call is **finding A2
(ADSR timing)**: the Phase 2 envelope thresholds as written cannot pass the current
`coefForTime` calibration, so I will treat the spec thresholds as the source of truth
and re-derive the envelope timing to meet them (test-first). If you'd rather keep the
current envelope "feel" and relax the test thresholds instead, say so; otherwise I
proceed with re-calibration. All other items follow the mandated order without
requiring input.
