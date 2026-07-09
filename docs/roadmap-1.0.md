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
| Sub-phase 2 — full multitimbral (per-part FX + 3 LFOs) | code complete; **CPU gate provisional** (ThinkPad pending); **mixer = R1** |
| **R1 — clear the debts** | **in progress** |
| R2 — GUI overhaul (+ help overlay) | not started |
| R3 — 1.0 feature set (+ R3.11 QWERTY v2) | not started |
| R4 — release engineering (v1.0.0) | not started |

## R1 — clear the debts

1. **Part mixer** (closes Sub-phase 2): `partN_level` / `partN_pan` params (defaults 1.0 /
   center keep goldens green), stated pan law, captured in MULTI saves, mixer-math tests,
   reachable while playing. Kit balance is two layers — verify per-pad levels in the kit
   editor work too. — *in progress*
2. **GitHub push** authorized; push master, confirm CI green on both OSes, fix CI findings.
   Push at every gate hereafter.
3. Clean slate (stale shells) — done.
4. ThinkPad report reminder (above) — standing.

### Decisions log
- 2026-07-08 — Per-part mixer is REQUIRED for Sub-phase 2 (user corrected an earlier misread
  of "future development feature"); it is R1 item 1. Kit was audibly quiet vs other parts —
  the mixer (part level) + per-pad kit levels are the fix.
- 2026-07-09 — Sub-phase 2 CPU gate marked provisional pending the ThinkPad report; the
  measured derate will replace the assumed ×3.5 everywhere.
