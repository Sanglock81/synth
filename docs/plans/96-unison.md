# #96 — Unison (Musicality Tier 3: the "expensive supersaw")

> Committed design record. Character per `docs/musicality-pass.md` Tier 3; ships off-by-default
> (count 1 = bit-identical goldens). Approved to build with the pre-agreed contingency.

## Character (Tier 3 — all four, or it doesn't sound expensive)

1. **Random start phase per stack voice** — MANDATORY. Identical phases collapse the stack to one
   louder voice. Each member's 3 oscillators draw their own start phase from `phaseRng`.
2. **Non-uniform detune spread curve** — members are NOT evenly spaced in cents; a symmetric curve
   (denser toward the centre, wider at the edges) is what reads as analog-rich, not a chorus.
3. **Per-stack-voice stereo pan spread** — members pan across the field by their detune position ×
   width. This is what forces the voice STEREO (there is no per-member pan after a mono sum).
4. **Per-stack-voice analog drift** — the Tier-1b drift, independent state per member, so copies
   wander apart (only when `analog > 0`; the fast path stays bit-exact).

Plus a **1/√N loudness trim** on the unison sum so switching count never jumps the level (the same
equal-RMS discipline as the voice-sum trim and the WT tables).

## Contingency (agreed; reopens with #100 data)

- **Default off (count 1)**, per-patch count, **max 7**. The **Efficient/live** oscillator profile
  **caps** the count to **2–3**; **studio/HQ keeps 7**. Documented. **Never cut, only capped.** The
  cap decision reopens if #100's real-hardware worst case genuinely exceeds the ThinkPad budget.

## Architecture (verified against the engine map)

- **Unison lives inside the voice, not extra polyphony slots.** N members × 3 oscs per note; the
  note pool (`maxVoices = 24`) is unchanged.
- **The voice becomes stereo for unison > 1.** `SynthVoice` keeps `osc1/2/3` as **member 0** (the
  existing bit-exact path) and adds `uosc[kMaxUnison-1][3]` for members 1..N-1, plus a **second
  filter `filterR`**. New `renderStereo(outL,outR,…)` pans each member into L/R, runs the L/R
  filters, VCA → stereo. The existing **mono `render(out,…)` is UNCHANGED** and used when count == 1.
- **Latch the count at note-on** (a new `noteOn` arg), so the mono↔stereo path never switches
  mid-note (mirrors the Tier-2C filter-oversample latch). `isUnison()` lets the engine dispatch.
- **Engine plumbing mirrors the stereo sample bus.** New per-part `partSynthL/R` buffers (sized +
  zeroed exactly like `partSampleL/R`). Unison voices accumulate into them; `mixParts` folds them
  into `sL/sR` with `voiceTrim` next to the sample bus:
  `sL[i] = m[i] + (synL[i] + smpL[i]) * voiceTrim`. **Non-unison voices stay on `partMono`** → the
  whole non-unison path is byte-for-byte unchanged (goldens hold). `partHadVoice` set for unison too.
- **VoiceParams gains** `int unisonCount`, `float unisonDetune` (cents spread 0..1), `float
  unisonWidth` (stereo 0..1). Defaults `1 / 0.15 / 0.5`. Count 1 ignores detune/width entirely.
- **Cap seam:** clamp the effective count against the oscillator quality profile where the engine
  already knows `oscQuality`/`activeVoiceLimit` (PluginProcessor prepare / `setOscQuality`).

## Increments (each: build + `run-all-checks.sh` ± `--sanitize`, goldens bit-identical at default,
click-torture in the same commit, ThinkPad-derated bench proving count==1 free + count==7 measured)

### Inc 1 — DSP: the stereo unison voice  ← DONE (params folded in; UI is Inc 2)

> Voice `renderStereo` (non-uniform detune curve, per-member random phase + independent drift + 0 dB-
> centre balance-law pan, 1/√N trim); mono `render()`/`applyParams()` untouched → goldens bit-identical.
> Engine: `partSynthL/R` stereo unison bus mirroring the sample bus, dispatch on `isUnison()`, mixParts
> fold, quality cap (Efficient→3 / HQ→7). Params `osc_unison`/`_detune`/`_width` wired through
> buildVoiceParams + perPartSoundIds. Two bugs found + fixed: (1) `beginMasterBlock` skipped the focused
> part's params, so the note-on read of the unison count (and, latently, the start-phase policy) was one
> block stale → now set current before this block's note-ons; (2) 18 extra oscillators BY VALUE ×24
> voices overflowed a stacked `SynthEngine` under ASan (SEGV) → the unison stack is now heap-allocated
> (`std::vector`, sized in prepare). Tests: `test_unison.cpp` (stereo, 1/√N level, note-on latch, click-
> free release) + `test_unison_plugin.cpp` (widens vs single voice, state round-trip). Release 508 +
> --sanitize green; goldens bit-identical.
- `SynthVoice`: member stack + `renderStereo` + latch + 1/√N trim + non-uniform detune curve +
  per-member pan/phase/drift. `VoiceParams` fields. `prepare`/`setOscQuality`/`noteOn` fan out to the
  stack. Engine: `partSynthL/R` bus + dispatch on `isUnison()` + `mixParts` fold + `partHadVoice`.
- Tests (`tests/dsp/test_unison.cpp`): count==1 bit-identical to pre-unison (golden render);
  count>1 → real stereo width (L≠R, correlation < 1) + detuned spectrum (beating) + louder-but-not-N×
  (1/√N holds); no click on note-on/off (click-torture); a mid-note count change does NOT switch path
  (latched). Golden gate (`render.f32.wav`) unchanged.

### Inc 2 — Params + wiring + the quality cap
- `Parameters.h`: `osc_unison / osc_unison_detune / osc_unison_width` (per part → `perPartSoundIds`).
- `buildVoiceParams` reads them; the engine clamps the count under Efficient (2–3) vs HQ (7).
- Migration/persistence; golden hold (defaults off).

### Inc 2 — UI + presets + docs  ← DONE
- UNI (count) / DET / WID knobs in the TopBar voice group (beside ANALOG); UI smoke test +
  `unison-controls.png`. **Supersaw** factory preset (count 7, wide). CHANGELOG + README updated.
  Bench scenario is deferred to the #100 ThinkPad prep (informational, not a ctest gate).

## Out of scope (post-1.0)
- Per-osc independent unison; unison for the noise source; a "unison as its own voices" pool model.
