# Road to v1.0.0 — living status

This is the handoff doc: phases, gates, decisions. Updated at every phase boundary.
Sessions rotate; **this doc + the repo is the handoff.** Execute phases strictly in order,
full gate (`run-all-checks.sh` + `--sanitize` + bench with clock context) at each, STOP for
the user's review between phases. Standing rules: test-first; audio thread sacred; `Source/DSP/`
JUCE-free; parameter IDs frozen (append, never reorder/rename); voices dumb; stop-and-ask on
failed gates; nothing focusable on the main panel; do not claim "per your call" unless quoting
the user.

**NOISE-CLEANLINESS (permanent regression criterion, R3):** every change that generates
notes or touches the audio path ships **in the same commit** with click-torture coverage of
its specific behavior (`tests/plugin/test_click_torture.cpp` — scans the full processor output
for sample-to-sample discontinuities, out-of-range peaks, non-finites). The `[click]` suite is
part of **every** gate from now on. Noise cleanliness is not a bug-of-the-week — it is a
standing acceptance test.

## Cross-cutting

- **CPU gates are provisional until the ThinkPad `validate.sh` report lands.** The dev-box
  ×3.5 derate is unverified and currently flags code that has shipped fine for months, so the
  measured ThinkPad number is the arbiter for the Sub-phase 2 CPU gate and every future one.
  Package: `tools/thinkpad-validate/`.
- **GitHub push:** authorized as of R1. Remote `origin` = github.com/Sanglock81/synth.git.
  Push at every phase gate; keep CI green on Linux + Windows.

## Status

| Phase | State |
|---|---|
| Phases 0–6, 7 (Bug B, 7A/B/C), 8A infra | done (pre-roadmap) |
| Routing discoverability + key-range zones (Part A/B) | done, gated |
| Sub-phase 1 — Kit parts | done, gated |
| Sub-phase 2 — full multitimbral (per-part FX + 3 LFOs + mixer) | **complete**; CPU gate provisional (ThinkPad pending) |
| **R1 — clear the debts** | DONE — mixer + push; **CI green on Linux + Windows** (build-test). sanitize.yml is nightly; `--sanitize` verified locally (190/190) |
| Static/pops regression hunt | two engine defects fixed (skip-resume FX clear, mixer zipper); exact static NOT reproduced offline — **user ear-confirm pending** |
| R2 — GUI overhaul (+ help overlay) | touch reliability done (hardware-confirmed); layout mockup signed off; **functional layout WIRED + gated** (see below); awaiting user review of the built editor |
| R3 — 1.0 feature set | **COMPLETE + gated.** Shipped: master parametric EQ; macro routing (+ Launchkey pots -> macros); **arp + 8-row step sequencer** (Group 2); **looper** — clock-linked/armed, dual MIDI+AUDIO lanes, WAV export (Group 3); **chord engine + QWERTY/CC/note modifiers**; Group 4 (double-tap numeric entry, LFO->knob mod animation, CLEAR); noise-cleanliness click-torture suite (ear-confirmed clean); edit-focus panel swap + MULTI capture + revert (1.3); **kit per-pad synth editing**; Random excludes the rhythm section; poly/mono/legato + glide. **This session (2026-07-12/13):** full per-part isolation (generator-yields-to-live voice steal; kit-edit live-pad); **voices 16 -> 24** (trim decoupled, goldens bit-identical); **load isolation** (patch loads are sound-only, never disturb seq/looper/globals/other parts); **default startup scene** (P1 lead / P2 spare / P3 bass / P4 808 kit = seq target). v2 backlog (NOT v1, per README): mod matrix, osc cross/ring-mod, wavetable, MIDI-clock sync, unison, filter drive, QWERTY v2, audio-stem export. |
| R4 — release engineering (v1.0.0) | **not started** — the remaining v1 work: (1) ThinkPad deployment validation (the one open v1 gate, now urgent after the 24-voice bump: derated worst case ~109%, confirm no xruns / adjust cap); (2) versioning + CHANGELOG + packaging/install docs; (3) refresh stale docs + editor.png. |

## R1 — clear the debts

1. **Part mixer** (closes Sub-phase 2): `partN_level` / `partN_pan` params (defaults 1.0 /
   center keep goldens green), **0 dB-centre balance pan law**, captured in MULTI saves,
   mixer-math + pan-law + MULTI round-trip tests, MIX panel section (reachable while
   playing, MIDI-learnable). Kit balance two layers: part level + per-pad Kit Editor level
   (both verified). — **done** (commit b4a872b; dsp 82 / plugin 108 green).
2. **GitHub push** DONE — `origin/master` live (workflow scope granted). First-ever Windows
   CI surfaced latent Windows breaks, all fixed to green: POSIX `unistd.h`/`sysconf` in the
   soak (guarded `__linux__`), MSVC C4996 (`_CRT_SECURE_NO_WARNINGS`), 1 MB stack overflow
   from the ~344 KB SynthEngine (`/STACK:8388608`), and an em-dash test name that broke the
   ctest name filter (ASCII-only). build-test green on Linux + Windows. Push at every gate
   hereafter; treat CI failures as gate failures.
3. Clean slate (stale shells) — done.
4. ThinkPad report reminder (above) — standing.

## R2 — GUI overhaul

**Touch reliability (first, before any layout) — diagnosis + fixes done; HARD USER GATE
pending (hardware confirmation).**
- Instrumented: `VASYNTH_TOUCH_TRACE=1` env var enables a global mouse-event trace to the
  log (finger index, position, target component, live-drag count, modal state) + a line
  whenever the focus watchdog grabs — so a failed first-touch on the Surface is captured
  as data, not guessed.
- Root-cause fix (strongest suspect): the 30 Hz focus-reclaim watchdog no longer grabs
  keyboard focus while a touch/drag gesture is live (`getNumDraggingMouseSources() > 0`) —
  grabbing focus mid-gesture is the classic cause of a dropped first touch. Predicate gains
  a `gestureActive` arg; regression case added.
- Touch hardening: linear faders `setSliderSnapsToMousePosition(true)` (first tap jumps to
  the finger); velocity mode already off.
- **HARD GATE — PASSED (2026-07-09):** user confirmed first-touch reliable + grab-mode good
  on hardware. Follow-up tuning: drag sensitivity gentled ~20% via one constant
  `kDragPixelsForFullRange` (313). Static also confirmed clean by ear.
- **Layout MOCKUP GATE (current):** non-functional mockup rendered at default/fullscreen/
  narrow (docs/mockup-*.png) — left part rail (P1-P4 + kit-pad sub-selector seam), centre
  in signal-flow order, right SCOPE+FFT, slim top bar, bottom CHORD row + collapsed
  RHYTHM/LOOPER zones, knob/fader/selector grammar. **STOP for user sign-off before wiring
  attachments.**
- Then: layout wire-up, control grammar at real sizes, master oscilloscope+FFT (RT-safe
  SPSC tap), help overlay (R2 addition), invariants regression.

**Layout wire-up — DONE (2026-07-10), gated.** The signed-off mockup is now the live
editor (`docs/editor.png`). New UI: `PanelChrome.h` (shared filled-tint frame/sub-box),
`Sections.h` (OSC/FILTER/ENVELOPE/LFO), reworked `FXPanel.h` (backlit name-bar = on/off;
tap toggles, drag reorders), `TopBar.h` (preset + Save/Random + live CPU + 8 macros +
MASTER + REC placeholder + help), `PartRail.h` (P1-P4 + kit-pad grid + level + INPUTS),
`ScopeView.h` + a processor RT-safe SPSC scope tap (`pushScope`/`readScope`), `BottomZones.h`
(horizontal CHORD bar + collapsible RHYTHM/LOOPER placeholders), `HelpOverlay.h` ('?').
Widgets gained `HSelector`, `ShapeSelector`, `RotaryKnob` side-label. 8 macro params appended
(`macro1..8`; real/automatable/learnable — routing is R3).
  - **Gates:** dsp 84 / plugin 113; Release run-all-checks **198/198 incl. pluginval s8**;
    sanitizers (ASan/LSan/UBSan + soak) ALL PASSED; editor invariants (no focusable
    descendant, state round-trip, open/close storm) green. Bench unchanged (GUI phase; DSP
    engine untouched) — 4-part worst case ~83% p99 ThinkPad budget, still **provisional**.
  - **Follow-ups flagged for the user:** (3) osc FINE->LEVEL + small ON kill; filter DRIVE
    (R3) dropped for VEL>CUT; WT wave + RHYTHM/LOOPER engines R3.

**R2 refinement pass (2026-07-10, gated) — user review of the built layout.**
  - **Master parametric EQ** at the end of the chain (post-FX sum / pre master gain):
    `DSP/ParametricEQ.h` (JUCE-free 4-band biquad — low shelf, 2 bells, high shelf) +
    `UI/EQPanel.h` (response curve + band knobs) under a shortened scope/FFT. Default
    off/flat = bit-identical bypass. `test_eq` + a plugin bypass-default test.
  - **Macros now routable + Random-assigned** (closes follow-up 1): Random assigns 1-4
    macros (100/50/25/10%) each to a DISTINCT routable param + randomizes values; map
    persists in state (`macro_map`); a macro knob drives its target on the message thread
    and shows the target's name. `test_macros`. (Full mod-matrix still R3.)
  - **Per-part LEVEL + PAN knobs** (MIDI-learnable) in the widened Parts cells — closes
    follow-up 2 (the mixer's on-panel home; the old MIX section stays retired).
  - Scope amplitude +40%; bigger SAVE/RANDOM; **fullscreen (FS) button**; taller CHORD bar;
    RHYTHM+LOOPER rebuilt as filled `preview - R3` panels (visual only until their engines).
  - **Gates:** dsp 88 / plugin 116; Release 205/205 incl. pluginval s8; sanitizers ALL
    PASSED; clean bench worst case ~49% p99 ThinkPad budget (EQ-off bypass adds nothing).

## Static/pops regression (engine-first, telemetry = discontinuity not CPU)
- **Fixed — silent-part FX skip resume (required):** FX chain state reset once on skip-entry
  (delay/reverb/chorus + chorus mod LFO). Dropped chorus resume jump 0.117→0.086.
- **Fixed — mixer level/pan zipper (confirmed, maxJump 0.204→smoothed):** per-part L/R gains
  ramp across the block; smoothers snap to first setMix.
- **Verified:** voiceTrim applied once per part (== old mono sum), soft-clip last, bounds test
  covers L+R. Permanent tortures: engine skip/resume over all 4 FX; processor multi-part
  pedaled silence/return (live+locked+kit) scanning L+R for clicks/bounds/finite.
- **OPEN:** the realistic processor torture is clean (maxJump 0.038) — the exact reported
  static did NOT reproduce offline. May be real-time/device/action-specific or already fixed.
  NOT closed until the user confirms clean by ear; if it persists, need a WAV recording + the
  exact triggering action to nail the confirmed root cause.

### Decisions log
- 2026-07-08 — Per-part mixer is REQUIRED for Sub-phase 2 (user corrected an earlier misread
  of "future development feature"); it is R1 item 1. Kit was audibly quiet vs other parts —
  the mixer (part level) + per-pad kit levels are the fix.
- 2026-07-09 — Sub-phase 2 CPU gate marked provisional pending the ThinkPad report; the
  measured derate will replace the assumed ×3.5 everywhere.

## Post-1.0 backlog (documented reservations, not accidental gaps)
- **Noise COLOR selector (white / pink).** The NOISE 4th-source row (added in the UI audit pass)
  deliberately leaves its middle column open. Post-1.0, a white/pink COLOR selector lands there,
  beside the NOISE label and LEVEL knob — the vacancy is planned, not an orphaned layout. (DSP:
  a one-pole pinking filter on the existing white-noise source; param `noise_color`, default white
  = bit-identical.)
- **WILD randomize into self-oscillation.** Randomize caps `filter_reso` at 0.6 (predates Tier 2B
  self-osc). Post-2C, WILD (the deep-exploration tier) may occasionally enter the self-osc sliver.
  (Tracked as an H5 follow-up.)
