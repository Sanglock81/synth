# Synth 1.0 — User Acceptance Test Plan (uat-1.0.md)

UAT is the final, scripted, hands-on pass performed by John before the v1.0.0 tag.
It complements — never replaces — the automated gates (CI, click-torture, headless UI
smoke, pluginval, target validation). Everything here is done by ear and by hand on
real hardware.

## Triage rules

- **BLOCKER** — wrong sound, wrong state, lost data, crash, stuck note, unusable UI.
  Blockers loop back through fix-and-regate; the tag waits.
- **KNOWN-ISSUE** — cosmetic, rare, or has a clean workaround. Recorded in the
  CHANGELOG under Known Issues and shipped.
- Every defect gets one line (here or in the tracker) with machine, steps, and class.

## Machine matrix

| Machine | Role | What gates here |
|---|---|---|
| ThinkPad (i7-8650U, Linux) | Reference target | Everything. Performance numbers gate on this machine only. Governor = `performance`. |
| Legion (7840HS, Win 11) | Windows platform + DAW integration | Functional pass on Windows; session-export-into-Ableton; VST3 in a real project. |
| Surface Pro 4 (Win, touch) | Windows touch + high-DPI pass | Touch usability; UI legibility at ~200% scaling; informative (non-gating) perf/latency notes on weaker hardware and built-in audio. |

## A. Functional pass (full on the ThinkPad; platform-relevant subset per Windows machine)

1. **Cold start** — fresh config (move aside `*.settings`); launch Standalone; play
   immediately with no dialog visits. Verify the startup-log git-hash banner matches the
   installed build.
2. **Routing & zones** — each surface defaults to the live patch on connect and at
   startup; routing never auto-restores. Exercise part zones/splits.
3. **Rhythm stack together** — arp + step sequencer + looper + scenes in one session.
   Verify the new factory default rows (kick / snare / rim / CH / OH / crash / ride /
   tom) sound correctly against at least two kits; reassign one row to a specialty pad
   and confirm the pattern follows.
4. **Mod system by hand** — LINK touch-connect routes; full LFO Link gesture: long-press
   DEST to arm, LFO holds still, existing links shown as an editable picture, tap
   add/remove, press-and-slide sweep bounds with live readout, long-press commit, Esc
   cancel. Verify per-LFO colors and dash cues.
5. **Kits & samples** — several factory kits incl. the three new (Foundry, Circuit,
   Hearth): choke groups, chord pads, velocity response; confirm the cross-kit trigger
   consistency by playing one pattern across all three new kits plus a classic kit.
   Import a user sample to a pad; confirm dedup on re-import.
6. **NOISE XY** — on a noise-heavy patch: drag the NOISE surface through all four
   bottom-edge colors (brown / pink / white / bright) and up into focus territory to the
   near-ring extreme; verify readout, GUIDE entry, and that returning to bottom-center
   restores the original character exactly. Route an LFO to a NOISE destination via the
   LINK gesture and verify smooth (click-free) modulation at high focus. Load three
   pre-feature factory presets and confirm no audible change from memory of rc1.
7. **QWERTY-only session** — a complete short piece using QWERTY v2, no MIDI hardware.
8. **MIDI clock out to pedals** — clock the Warped Vinyl (and any other clocked pedal);
   verify lock at multiple tempos and across tempo changes.
9. **Session export into a DAW** — export; import into Ableton Live on the Legion;
   verify audio and structure.
10. **Windows/touch pass (SP4)** — main workflows by touch only: patch browsing, macro
    sweeps, LINK gestures, NOISE XY drag, sequencer editing. Note anything demanding
    pointer precision. Check legibility at native scaling.
11. **Unknown-controller check** — attach a controller with no factory profile; confirm
    sensible zero-config playability.
12. **Sample-rate premise check** — one short session at 44.1 kHz (optionally 96 kHz):
    tuning, tempo sync, LFO SYNC divisions, looper seams, NOISE XY center calibration.
13. **State torture** — rapid preset switching with notes held; part copy; save/reload
    mid-performance; kill and relaunch mid-session; no stuck notes, no corrupt state;
    PANIC (Ctrl+.) works.

## B. Sound & tuning sessions (ThinkPad, good monitoring)

1. **Drive character** — filter DRIVE across types; self-oscillation; in-loop tanh
   musical at extremes.
2. **SAT contrast** — soft-then-dig-in.
3. **Unison** — 1→7 on a lead and a pad; width/detune; live-cap behavior.
4. **Wavetables** — factory tables incl. fifth-wave; mip cleanliness on fast high sweeps.
5. **NOISE XY musicality** — tilt end: does pink read as pink on a pad's breath layer?
   Focus end: is the near-ring extreme usable, is makeup gain right across the surface?
6. **Blind analog-layer A/B** — phase policy + drift + reverb MOTION, blind, several
   patches.
7. **Bank audit** — every factory preset a few seconds: no silent patches, no level
   bombs, category sanity. The three new kits audit against the existing twelve for
   level balance and character separation.

## C. Integration performance

One uninterrupted live set (20+ min) on the ThinkPad at the live buffer size, using the
rhythm stack, scene changes, and at least one of the new kits — ending in a
bounce/recording. No overrun, no stuck note, no manual rescue = pass.

## Sign-off

UAT is complete when A–C are done on their designated machines and zero BLOCKERs remain.
John's explicit signature in the working conversation fires the v1.0.0 tag; the signature
is quoted verbatim in the tag annotation.
