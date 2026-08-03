# Hands-on test report — triage & diagnosis (2026-08)

## DECIDED (locked)

**Pre-tag (before v1.0.0) — the five confirmed defects, each gated + sample-accurate/regression tested:**
- **#16 Looper** (P0): arm → recording begins **exactly at the next downbeat** → at loop end,
  playback starts **immediately from that same boundary** while record disarms (no dead cycle). PLUS
  the **content-offset**: captured events/audio must align **sample-accurately** to the record-start
  boundary — nothing played on the downbeat may be missing from the take. Sample-accurate tests.
- **#14 Width lean**: fix + an **L/R energy-balance assertion** at max width + **re-run the mono-fold
  test at max width**.
- **#12 Animation coverage**: the standing spec is *every modulated control animates* — partial
  coverage is a **defect**, not polish. Every mod-target knob lights when any route drives it.
- **#13a LINK break**: fix the break now (redesign is 1.1). **Extend the real-event smoke to the
  LFO-as-source arm path specifically** — if the sweep tests passed while hands failed, that coverage
  gap is the bug's twin.
- **#2 PW drag sensitivity**.
- **Discoverability (docs)**: guide/README note for **top-bar unison** (UNI/DET/WID) and for
  **RS/RN/FR** — state where phase policy is audible (note attacks, detuned stacks, FM patches; on a
  sustained saw it is rightly invisible).

**1.1 — designed track (no 1.0 code):** per-osc unison (single/3/5), PW-on-sine clip morph, scope
auto-trigger, envelope visualizer + curve shapes, the LFO link-UX redesign, the parts/LFO layout
narrowing, and **filter upgrades (#9)** as a **1.1 design doc** (per-osc filters + comb + evolving).

**No change (explained):** keep the saw (#1), RS/RN/FR is WAD (#5b), chorus VOICES ≠ unison (#15).

---


Every item from the hands-on pass, traced to the code, classified, and dispositioned. Legend:
**DEFECT** (broken, fix) · **UX** (works but rough, small change) · **FEATURE** (new work) ·
**WAD** (working-as-designed; explanation, maybe reconsider). Priority P0 (blocker) → P3 (nice).

## Summary

| # | Item | Class | Pri | Proposed |
|---|------|-------|-----|----------|
| 16 | Looper record/playback timing | **DEFECT** | **P0** | Fix now (pre-tag) |
| 14 | Stereo width weighted left at max | **DEFECT** | P1 | Fix now (pre-tag) |
| 13 | LFO LINK unusable + redesign | DEFECT + FEATURE | P1 | Fix the break now; redesign 1.1 |
| 12 | Mod animation only on some knobs | DEFECT | P1 | Extend now (pre-tag) |
| 2 | PW knob drag too insensitive | UX | P2 | Quick (pre-tag) |
| 6 | Parts −40% / LFO −20% width | UX | P2 | Layout pass (pre-tag or 1.1) |
| 7 | Filter DRIVE vs SAT confusion | UX/WAD | P2 | Relabel, keep both (see below) |
| 4 | PW on sine → sine→square morph | FEATURE | P2 | 1.1 (nice, small) |
| 3 | Oscilloscope auto-trigger (static wave) | FEATURE | P2 | 1.1 |
| 5 | Per-osc unison (single/3/5) | FEATURE | P2 | 1.1 (global unison exists) |
| 5b | RS/RN/FR "do nothing" | WAD | — | Explanation (subtle by nature) |
| 1 | Remove saw, use triangle+PW | WAD | — | **Advise keep saw** (see below) |
| 8 | What does KEYTRK do | WAD | — | Explanation (keep) |
| 15 | Chorus VOICES = unison? | WAD | — | Explanation (no) |
| 11 | Mod env beyond pitch | WAD/FEATURE | P3 | Already routes to cutoff + matrix |
| 9 | Filter upgrades (per-osc/comb/evolving) | FEATURE | P3 | 1.1+ design |
| 10 | Envelope visualizer + non-linear shapes | FEATURE | P3 | 1.1 |

---

## P0 — Looper record/playback timing (#16)  **DEFECT, top priority**

**Observed:** primed record doesn't start exactly on the measure; at record-end the loop doesn't play
back immediately — it starts the *following* cycle; the recording sounds offset late.

**Diagnosis (code-confirmed):** two separate issues.
1. **Playback delayed one full cycle.** `Source/DSP/Looper.h` documents an *arm-on-wrap* rule: "a
   freshly recorded event is not played until the loop next wraps... overdubs join on the following
   cycle." The processor (`PluginProcessor.cpp` ~3039–3047) correctly quantizes record **start** to
   the lane downbeat (`loopArmPending` → engages when `position(lane)==0 || wrapped`) and records
   exactly one pass, but on completion the just-recorded events stay *armed-for-next-wrap*, so they
   first sound one cycle later. **This is the core bug.** The arm-on-wrap rule exists to avoid
   double-triggering during *overdub of a playing loop* — but for a fresh **record → play** it should
   arm the events to sound from the **same** downbeat that record ended on.
2. **"Recording started late" feel.** Record engages at the block where the downbeat is *detected*
   (block-granular, and only after `position==0`). If the player lands notes a hair before the
   detected downbeat, they're dropped → recording reads as starting late. Companion fix: latch the
   exact downbeat sample and honor pre-downbeat notes within the block (or a small look-back).

**Fix plan (pre-tag):** on one-shot record completion, transition the lane to PLAYING with its events
armed to fire from position 0 immediately (not next wrap); tighten the record-start to the
sample-accurate downbeat. Add a looper timing test (record a known pattern → assert playback begins on
the *next* downbeat, sample-aligned, on the *same* cycle). Applies to both MIDI (`Looper.h`) and audio
(`AudioLoop.h`) lanes.

## P1 — Stereo width weighted left at max (#14)  **DEFECT**

**Observed:** width all the way up leans left.

**Diagnosis:** `Source/DSP/StereoWidth.h` is symmetric on paper (`L = mid + side`, `R = mid − side`).
At width ≤ 1 it's exact mid/side (balanced). The imbalance appears only at **width > 1**, where extra
side is *synthesized* from the mid through an allpass cascade (`outSide`). If that synthesized side has
any nonzero mean / DC or an asymmetric transient, `+outSide`/`−outSide` biases L hotter than R.
**Suspect:** the allpass-cascade side term isn't zero-mean. **Fix plan (pre-tag):** measure L vs R RMS
on a mono input at max width (should be equal); if unequal, DC-block / zero-mean the synthesized side,
or balance the pan law. Add an L/R-balance test.

**RESOLVED (commit pending).** Root cause was subtler than "not zero-mean": an allpass cannot
decorrelate DC, and this cascade left the whole **low-mid** band nearly in-phase with the mid too —
the mid↔side correlation only crossed zero ~1.8 kHz — so `E[mid·side] > 0` made L hotter (measured up
to **~18 dB** of per-frequency imbalance at max width; +5.4 dB broadband on a 60/250/1200 Hz mono
mix). A simple high-pass on the side was insufficient (the correlated band *is* the midrange). Fix:
**orthogonalise the synthesized side against the mid** (Gram-Schmidt — subtract the leaky running
`⟨mid·decorr⟩/⟨mid·mid⟩` projection). Balance is now **~0.06 dB at every frequency** and the mono
fold-down stays bit-exact. The estimator is seeded at β = 1 so a note onset opens *narrow → wide*
(never over-wide), and the FX-skip→retrigger click test was tightened to the correct invariant
(boundary jump ≤ steady-state jump, not an absolute slope bound that mis-flags a bright widened saw).
Regression tests: L/R energy-balance + clean mono fold on a bass-heavy mono source at max width
(`tests/dsp/test_fx.cpp` `[width][balance]`); `fx_nondefault` golden regenerated (width = 1.7).

## P1 — Mod animation only shows on some knobs (#12)  **DEFECT (coverage)**

**Observed:** the "something is moving this" animation appears only in a few places (e.g. cutoff), not
on every modulated knob; "LFO pitch animation only affects cutoff."

**Diagnosis:** animation (`paintModRing`) is wired to controls whose param is a **registry
destination** (`wireModTargets` → `moddest::destForParam`) and driven by the published voice-offset for
a subset (cutoff/PW/reso/levels/FM). LFO→pitch has **no single knob** to animate; a matrix route to,
say, an envelope stage or an FX knob may not publish an offset the knob reads. **Goal (your ask):
every knob animates when *anything* is modulating it.** **Fix plan (pre-tag-ish):** publish a
per-destination live-offset for *all* registry dests and have every mod-target knob read it, so any
active route (LFO/env/velocity/macro) lights its target. Bigger dests (env stages, FX) need their
block-offset surfaced to the UI. Pitch stays un-animated (no knob) unless we add a pitch indicator.

## P1/1.1 — LFO LINK unusable + redesign (#13)  **DEFECT now, FEATURE later**

**Observed:** can't set an LFO link properly; wants: touch-and-hold the LFO **ON** to start a drag,
drag to a knob/slider to link; turning the LFO **OFF** clears all its links; multiple targets per LFO;
a menu listing established links; scrap the Pitch/Cutoff dest buttons for a **LINK** button.

**Diagnosis:** today an LFO has a fixed **DEST** selector (off/pitch/cutoff/PW) *plus* the global
**LINK** button (mod-matrix touch-connect). Two overlapping mechanisms = the confusion. Your redesign
turns each LFO into a pure mod **source** with its own drag-to-link affordance and multi-target list —
cleaner and standard. **Split:** (a) **now** — verify/repair the existing LINK touch-connect for LFO
sources (a real "can't set it" break is a defect); (b) **1.1** — the per-LFO hold-to-drag + multi-link
+ links menu + dropping the DEST selector (a genuine UI rework; the LFO still needs its legacy fixed
dest for preset back-compat, or a migration).

## P2 — quick UX

- **PW knob drag too insensitive (#2).** All rotaries use `kDragPixelsForFullRange = 313` px
  (Widgets.h). PW's *audible* range is narrow, so the full arc over 313 px feels sluggish. There's a
  per-knob `setDragSensitivity` — lower it for PW (and reconsider the global default). Pre-tag.
- **Parts −40% / LFO −20% width (#6).** Layout re-proportioning to give Filter + Envelope room. Pure
  layout; do it with the filter/envelope upgrades so the new space has a purpose.
- **Filter DRIVE vs SAT (#7).** They are **not** redundant — see the answer below. Recommend
  **relabel** (filter "DRIVE" is in-loop; the FX "SAT" stays SAT) or add a one-line tooltip; keep both.

## Answered questions / working-as-designed

- **Why do saw and PW-skewed-triangle sound different? Should we drop saw? (#1)**
  In `PolyBlepOscillator.h`: **saw** is `2·phase−1` **with a PolyBLEP correction** at the wrap
  discontinuity → *band-limited* (no aliasing), full harmonic series (1/n rolloff). **Triangle** is a
  piecewise-linear ramp where **PW is the up/down asymmetry**; at PW→0/1 it approaches a ramp shape but
  the triangle path has **no BLEP** → its wrap discontinuity **aliases**, and an asymmetric triangle's
  harmonic amplitudes differ from a true saw. So the PW-triangle "saw" is an *aliased approximation*
  with different harmonic weighting, not a real saw. **Recommendation: keep the saw.** Dropping it
  would replace a clean band-limited saw with an aliased one — a downgrade, not a simplification.
- **RS/RN/FR appear to do nothing (#5b).** They *are* wired (`SynthVoice::noteOn` → `osc.reset(startPhaseFor(mode))`;
  RESET=identical each note, RANDOM=random start, FREE=keep running phase). But **start phase is
  inaudible on a sustained single tone** — it only matters for attack-transient consistency, phase
  relationships between multiple oscillators, unison spread, and very short percussive hits. Working as
  designed; the effect is inherently subtle. Worth reconsidering whether it earns osc-row space vs
  moving to an advanced menu.
- **What does KEYTRK do? (#8)** Filter keytrack: the played **note's pitch raises the cutoff**. At full
  keytrack the timbre stays constant as you go up the keyboard (like an acoustic instrument — high
  notes stay proportionally bright); at zero, high notes sound duller because the fixed cutoff filters
  more of their harmonics. Standard synth control — **keep it** (maybe a tooltip).
- **Chorus VOICES 1|2 = the unison I asked for? (#15)** No. Chorus **VOICES** is the number of **chorus
  taps** (an FX-stage thing — 2 adds a second decorrelated delayed voice for a thicker/wider chorus).
  **Oscillator unison** is separate: the top-bar **UNI / DET / WID** (1–7 voice stacked-oscillator
  supersaw). Different features. (Your #5 per-osc single/3/5 unison is a new ask — see below.)
- **Mod env can do more than pitch (#11).** It already drives **filter cutoff** (ENV AMT) *and* pitch
  (E>PCH) — and the **Mod Env is a mod-matrix source**, so it can already route to *any* destination
  (PW, wave pos, FX, LFO rate, osc level…) via LINK/MOD. So the capability exists; it's a
  *discoverability* gap more than a missing feature. A "route mod-env" affordance could surface it.

## Features (mostly 1.1 — most reopen the freeze)

- **Per-osc unison, single/3/5 (#5).** Today unison is global (voice-wide, 1–7). Per-oscillator unison
  (or a clear single/3/5 selector in the osc row) is a real feature + DSP (each osc gets its own
  detuned stack). 1.1.
- **PW on sine → sine↔square soft-clip morph (#4).** Feed the sine through a PW-driven waveshaper
  (0% sine → 100% square, soft clipping between). Small, tasteful, removes a dead knob. 1.1.
- **Oscilloscope auto-trigger (#3).** Zero-crossing / period-locked trigger so the waveform sits
  horizontally static and readable. Scope enhancement. 1.1.
- **Filter upgrades (#9): per-osc filters, comb filters, evolving/time-varying filters.** The big one —
  a major sound-design expansion (multiple filter instances, new topologies, filter-FM/morph). Needs a
  design pass; pairs with the layout re-proportioning (#6). 1.1+.
- **Envelope visualizer + non-linear ADSR shapes (#10).** An ADSR display + per-stage curve shaping
  (exp/log/S-curve). Visualizer is moderate; curve shaping touches the envelope DSP. 1.1.
- **LFO redesign (#13b)** — see P1/1.1 above.

---

## Proposed scoping (needs your call)

- **Fix as pre-tag defects (before v1.0.0):** #16 looper timing (P0), #14 width imbalance, #12 mod-
  animation coverage, #13a the LFO-link *break*, #2 PW drag, and the #7/#8 relabels/tooltips.
- **Defer to 1.1 (reopens scope):** #4 sine-PW morph, #3 scope trigger, #5 per-osc unison, #6 layout,
  #9 filter upgrades, #10 envelope viz/curves, #13b LFO redesign, #11 mod-env affordance.
- **No change (explained):** #1 keep saw, #5b RS/RN/FR, #15 chorus voices.
