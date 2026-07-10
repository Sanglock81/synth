# Road to v1.0.0 — living status

This is the handoff doc: phases, gates, decisions. Updated at every phase boundary.
Sessions rotate; **this doc + the repo is the handoff.** Execute phases strictly in order,
full gate (`run-all-checks.sh` + `--sanitize` + bench with clock context) at each, STOP for
the user's review between phases. Standing rules: test-first; audio thread sacred; `Source/DSP/`
JUCE-free; parameter IDs frozen (append, never reorder/rename); voices dumb; stop-and-ask on
failed gates; nothing focusable on the main panel; do not claim "per your call" unless quoting
the user.

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
| **R1 — clear the debts** | mixer done; **pushed to origin/master** (workflow scope granted); CI watched |
| Static/pops regression hunt | two engine defects fixed (skip-resume FX clear, mixer zipper); exact static NOT reproduced offline — **user ear-confirm pending** |
| R2 — GUI overhaul (+ help overlay) | touch reliability (focus-vs-gesture fix + GRAB mode) done; **hardware touch gate pending**; then layout-mockup gate |
| R3 — 1.0 feature set (+ R3.11 QWERTY v2) | not started |
| R4 — release engineering (v1.0.0) | not started |

## R1 — clear the debts

1. **Part mixer** (closes Sub-phase 2): `partN_level` / `partN_pan` params (defaults 1.0 /
   center keep goldens green), **0 dB-centre balance pan law**, captured in MULTI saves,
   mixer-math + pan-law + MULTI round-trip tests, MIX panel section (reachable while
   playing, MIDI-learnable). Kit balance two layers: part level + per-pad Kit Editor level
   (both verified). — **done** (commit b4a872b; dsp 82 / plugin 108 green).
2. **GitHub push** authorized but BLOCKED: git authenticates via the `gh` token
   (`Sanglock81`), which has `repo` but not `workflow` scope — GitHub refuses a push that
   includes `.github/workflows/` files. `gh auth refresh -s workflow` hadn't taken effect
   as of last check. SSH has no key on this box. User chose to defer and continue dev;
   finish the push (+ confirm CI Linux/Windows) once the scope is granted. 65 commits wait
   on local `master`.
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
- **HARD GATE:** user confirms first-touch reliability on the Surface. If it still flakes,
  capture the trace log and the confirmed root cause gets the fix. Layout rebuild does NOT
  start until this gate passes.
- Then: layout rebuild (mockup sign-off gate), control grammar, master oscilloscope+FFT,
  help overlay (R2 addition), invariants regression.

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
