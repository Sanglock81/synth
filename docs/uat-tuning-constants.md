# 1.0 UAT — tuning constants sheet

The three "voice character" verdicts in the UAT (drive, SAT, unison) are **judged by ear**.
If a verdict fails, the fix is a one-line constant change here, not a redesign. This sheet is
the single place those knobs live, with file · line · current value · what it does · which way
to move it.

> **Any change here is a DSP change** → it shifts the golden renders. After editing: rebuild,
> re-run `./run-all-checks.sh` (goldens will fail), regenerate the goldens deliberately, then
> `--sanitize`, then CI green on both platforms. Never hand-wave a golden update — confirm the
> only thing that changed is the intended constant.

---

## 1. Filter DRIVE — in-loop saturation  (`Source/DSP/SVFilter.h`)

The `filter_drive` knob (0 = bit-exact clean fast path) pushes the filter input into an in-loop
`tanh` saturator, with makeup gain so level stays roughly constant.

| Constant | Line | Value | Controls | Move it |
|---|---|---|---|---|
| `kMaxDriveGain` | ~275 | `4.0` | Input gain into the tanh at drive = 1 (~+12 dB). The whole drive range scales from this. | ↑ = more aggressive/dirtier at full drive; ↓ = tamer, more "warm" than "fuzz". |

Makeup (`driveComp`, `selfOscComp`, clamp `kOutMax`) is auto-derived — leave unless level
tracking is wrong, not for character.

**Verdict question:** does turning `filter_drive` up read as *musical overdrive* (thickens, adds
harmonics) rather than *volume* or *harshness*? Too harsh/loud → lower `kMaxDriveGain` (e.g. 3.0).
Too subtle → raise (e.g. 5.0).

## 2. SAT — width-block saturator  (`Source/DSP/StereoWidth.h`)

A two-stage **variable-threshold** clipper before the widener. Sub-threshold audio passes clean
(so soft playing stays clean = velocity/dynamic contrast preserved); only peaks above the
threshold clip. The `fx_sat` knob lowers the threshold across a two-segment exponential curve.

| Constant | Line | Value | Controls | Move it |
|---|---|---|---|---|
| `kThi` | ~238 | `0.45` | Threshold at SAT = 0 (single notes just start to break up by ~30 %). | ↑ = gentler at low settings (more headroom before it bites); ↓ = grittier sooner. |
| `kTmid` | ~239 | `0.06` | Threshold at SAT = 0.5 (solid soft overdrive). | Sets the "sweet spot" midpoint. ↓ = more overdrive at noon. |
| `kTlo` | ~240 | `0.015` | Threshold by 90 % (hard square → fuzz). | ↓ = more extreme fuzz at max. |
| `kAsymK` | ~241 | `0.42` | Soft-clip **asymmetry** — negative knee pushed out 1/k → even-harmonic (tube) warmth. | →1.0 = more symmetric/cleaner; ↓ = more even-harmonic character/warmth. |
| `kWetRamp` | ~237 | `12.5` | How fast the wet mix engages as SAT leaves 0. | ↑ = snappier onset; ↓ = more gradual. |

**Verdict question ("dynamic contrast"):** at a fixed SAT setting, does *soft* playing stay clean
and *hard* playing bite — i.e. the saturation tracks how hard you play, not a constant fuzz floor?
If soft playing already distorts → raise `kThi`. If it never bites hard → lower `kTmid`/`kTlo`.
If it sounds fizzy/transistor rather than tube-warm → lower `kAsymK` toward ~0.35.

## 3. UNISON — stacked-voice character  (`Source/DSP/SynthVoice.h`)

Per-member detune spread, stereo pan spread, random start phase, and analog drift — the four
things that make a stack sound like an ensemble rather than a chorus.

| Constant / fn | Line | Value | Controls | Move it |
|---|---|---|---|---|
| `kMaxUnison` | ~98 | `7` | Max stacked voices (engine caps the live/Efficient count lower). | Structural — leave for 1.0. |
| `kMaxUnisonCents` | ~522 | `50.0` | ± cents spread at DET = 1. | ↑ = wider/lusher (and more "out of tune"); ↓ = tighter. |
| `unisonPos()` curve | ~405 | `x·(0.6 + 0.4·x²)` | The **non-uniform** spacing shape — edges spread more than centre (mild cubic). Also drives the pan spread. | Raise the `0.4` term = more edge-weighted (hollow centre); lower = more even. |
| `kMaxDriftCents` | ~548 | `2.0` | Per-voice analog pitch-drift ceiling at ANALOG = 1 (also the ANALOG knob). | ↑ = livelier/looser; ↓ = more static. |
| `kMaxPwDrift` | ~549 | `0.01` | Pulse-width drift ceiling. | A hair of movement; ↑ for more shimmer on squares. |
| `uRandPhase()` | ~451 | — | Random start phase per member (decorrelates the stack, no comb on note-on). | Leave — it's the anti-phase-coherence character. |

Pan spread = `unisonPos(m,N) · unison_width` through a 0 dB balance law (`SynthVoice.h` ~338).

**Verdict question:** does a unison patch sound **wide and animated** (a real ensemble, moving) at
moderate DET/WID, rather than thin, static, or comb-filtered? Thin/hollow → lower the `0.4` in
`unisonPos`. Static → raise `kMaxDriftCents`. Not wide enough → the pan spread is width-driven,
so it's a patch setting first; only touch code if WID = 1 still sounds narrow.
