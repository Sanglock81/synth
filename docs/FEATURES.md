# synth — complete feature & function reference

A hands-on inventory of everything the instrument does, organized by area. Use it as a
walk-through checklist: each bullet is a distinct feature or control you can exercise. Cross-refs:
scenario checklist `docs/uat-1.0.md`; in-app help is the **`?`** menu (Keyboard Map + the 13-section
guide, mirrored to `docs/guide.md`); preset/kit detail `docs/presets.md`.

_A hands-on reference — not auto-maintained. If a detail here disagrees with the running
build, trust the build._

---

## 1. Sound engine — oscillators

- **Three oscillators** per voice (OSC1/2/3), each independently:
  - **ON / OFF** kill switch — an off oscillator is *skipped* (real CPU saved, not just muted).
  - **Wave**: Saw · Square (PWM) · Triangle · Sine · **Wavetable (WT)**.
  - **OCTAVE** (−2..+2), **SEMI** coarse tune (−24..+24 st), **DETUNE** fine (±100 cents).
  - **PW** pulse width (square); in WT mode the same slot becomes **WT POS** (frame morph).
  - **LEVEL** in the mix.
  - **Start-phase policy** per osc: **RS** Reset (bit-exact each note) / **RN** Random / **FR** Free-run.
- **Wavetable engine**: factory tables + a **seeded random-table die** (deterministic, persisted by
  seed); mip-mapped (pitch-safe), position smoothed (zipper-free), 4× oversampled.
- **FM (phase-modulation) chain** osc3 → osc2 → osc1:
  - **FM 2>1** (osc1 row): osc2 phase-modulates osc1. **FM 3>2** (osc2 row): osc3 modulates osc2.
  - Depth ≈ modulation index β = 2π·depth (0.3–0.5 e-piano/bell, 1.0 aggressive).
  - Modulator uses its **raw** output regardless of mix level (a level-0 modulator still shapes its
    carrier); the modulator's SEMI/OCTAVE set the **ratio** (keytracked).
  - **Carrier restriction**: FM only on **sine/tri/WT** carriers — saw/square disable the knob (dimmed).
  - A **MOD** badge marks whichever oscillator is currently acting as a modulator.
- **Unison** (top bar UNI/DET/WID): up to a **7-voice** detuned + panned + phase-decorrelated stack
  (a proper supersaw), off by default; Efficient/HQ oscillator quality trade-off.
- **Analog drift** (ANALOG): subtle per-voice pitch + pulse-width wander (0 = bit-exact).
- **Noise** — a 4th sound source with its own LEVEL, under the oscillators, plus a **NOISE XY
  field**: a 2D drag pad that sweeps the noise's character. Along the bottom edge it moves through
  the classic noise colours (brown / pink / white / bright); dragging upward focuses the noise into a
  band centred on that point (40 Hz – 12 kHz), and at the top the band is tight enough to ring into
  pitched noise. The two regions crossfade into one continuous surface. Both axes are mod
  destinations (**Noise Tilt**, **Noise Focus**). Centre-bottom is white — and an exact bypass, so a
  patch that leaves the field alone sounds exactly as it did before the field existed.
- **Anti-aliasing**: PolyBLEP + 4× oversample + FIR decimation; Efficient (live) vs HQ (studio) modes.

## 2. Sound engine — filter

- **One resonant state-variable filter** (TPT/zero-delay), types **LP / HP / BP / Notch**.
- **CUTOFF**, **RESO** (top of the range **self-oscillates** into a sine).
- **DRIVE** — analog-style saturation inside the filter loop (per-voice; 0 = bit-exact clean).
- **ENV AMT** — mod-envelope → cutoff (bipolar).
- **KEYTRK** — note pitch → cutoff (timbre consistency up the keyboard).
- **VEL>CUT** — velocity → cutoff (harder = brighter).

## 3. Sound engine — envelopes

- **Two ADSR envelopes**, exponential, click-free retrigger + voice-steal:
  - **AMP** envelope → loudness.
  - **MOD** envelope → filter cutoff (ENV AMT) **and** pitch (**E>PCH**, ±48 st) — the basis of the
    drum voices' pitch-drop.
- **VEL** — velocity → amplitude depth, with a raised soft-hit **floor** (soft notes stay audible,
  dynamics kept), tuned for fast multi-note passages.

## 4. Modulation — LFOs

- **Three LFOs per part**, each: **DEST** (off / pitch / cutoff / PW / … any mod dest), **RATE** (Hz),
  **DEPTH**, **SHAPE** (tri / sine / square / S&H).
- **SYNC** toggle → RATE morphs into a stepped **DIV** (note division): 4 bar … 1/32, plus **triplet**
  and **dotted**. Sync **engages at the next bar** (click-safe) and **phase-locks to the beat**;
  un-syncing returns to the RATE knob (the knob always shows the live rate).
- LFO → knob **animation** shows motion (echo + decay), not mere existence.

## 5. Modulation — matrix, macros, performance controllers

- **Mod matrix** (8 slots): any **source** → any **registry destination** with bipolar depth.
  - Sources: LFO1/2/3, Mod Env, Amp Env, Velocity, Note, Mod Wheel, Pitch Bend, Random (per-note
    S&H), Macro 1–8.
  - Destinations: pitch, cutoff, resonance, PW, wave pos, osc 1/2/3 level, noise level, **noise
    tilt + noise focus (the NOISE XY field)**, **osc FM
    depth (Osc 1/2 FM)**, amp, all FX params (chorus/delay/reverb/width/SAT/EQ bands), LFO rate/depth,
    every envelope stage, filter env-amt/keytrack/vel routes, osc octave/detune, glide, trim,
    **part level (tremolo)** and **part pan (equal-power auto-pan)**.
  - **Per-part, regardless of focus:** every part's own routes modulate **that** part whenever it's
    sounding — a background pad's LFO→auto-pan/tremolo (or any FX/EQ/LFO-rate route) keeps moving while
    you edit another part. (Voice-tier dests were always per-part; the mixer- and block-tier dests are
    too now — the focus-scoped interim limitation is gone.)
- **LINK** — touch-connect: arm LINK, tap any control → routes the armed source to it (drag ~2 s to
  set depth). **MOD** overlay lists/edits routes.
- **8 assignable macros** (M1–M8), each labelled with what it drives; default map (M1 cutoff … M8
  focused-part level); Reset MIDI + macros restores factory.
- **Pitch bend**, **mod-wheel vibrato**, **sustain pedal** — per part.

## 6. Effects (per part, reorderable)

- **Reorderable FX chain** (drag a name bar to reorder, tap to toggle): **SAT + WIDTH → CHORUS →
  DELAY → REVERB**, then a fixed EQ at the end.
- **SAT** — tube-style saturation (variable-threshold, velocity-sensitive, 2× oversampled).
- **WIDTH** — mid/side stereo width; past unity widens beyond the speakers (allpass decorrelation).
- **CHORUS** — rate/depth/mix + **VOICES 1|2** (a second decorrelated tap).
- **DELAY** — time (tempo-followable) / feedback / **ping-pong spread** / mix.
- **REVERB** — Freeverb-style size/damp/width/mix + **MOTION** (slow tail modulation, anti-metallic).
- **5-band parametric EQ** at the end of each part's chain — follows edit focus; vertical GAIN
  slider per band, drag sideways = frequency, double-tap = value, per-band on/off, EQ on/off.
- **Output safety**: `1/√maxVoices` headroom trim + a transparent safety **soft-clipper** — output
  **never exceeds ±1.0** for any patch/polyphony (bit-exact below 0.8 threshold).

## 7. Voicing & performance

- **Poly / Mono / Legato** per part (LEG re-triggers envelopes only on a detached note).
- **GLIDE** (portamento) time.
- **24-voice** polyphony (configurable cap); own-part-first voice stealing; per-part isolation.

## 8. Parts & multitimbral

- **Four parts** (P1–P4), each an independent patch **or** drum kit with its **own** FX + 3 LFOs +
  mixer **level/pan** — a small mixer.
- **Edit focus**: tap a part → the panels edit it; a **kit** part dims the synth panels ("edit pads
  in Kit Editor") and exposes its EQ.
- **Live vs pinned** parts: a surface can follow the play-focus (Live) or pin to one part.
- Full **isolation**: live edits / RANDOM / voice-steal never cross parts.

## 9. Drum kits

- **Kit part** = per-note map of up to **16 pads** (4×4, Launchkey grid). Each pad: trigger note,
  source preset (baked), 1 sounding note (or **2–4 = a chord pad**), level, **choke group**.
- **Choke** — a hit in a nonzero group click-freely cuts still-ringing pads in the same group
  (closed-hat silences open-hat); re-hitting a pad retriggers (monophonic pad).
- **Sample pads** — load a WAV onto any pad (pitch-tracked, cubic interp, clean tail); md5-dedup
  managed library; clear per pad.
- **Chord pads** — 2–4 tuned sounding notes from a single hit (build in the Kit Editor).
- **Kit Editor** — learn trigger + sounding notes by play; set source preset, level, choke;
  **Audition**; edit a pad's voice on the main panel (the rest of the kit keeps its baked sound).
- **Factory kits** — **Classic Machines** (10 synthesized machine kits: 808 · 909 · 606 · 78 · 707 ·
  LM1 · DMX · RX5 · R50 · MP60) + **Originals** (Industrial, Studio); picker grouped Classic /
  Originals / User with pad counts. Hats choke, cymbals ring free; LM1 honors "no cymbals".
- **Migration** — an old MULTI/.kit that stored a retired kit loads its successor (808 Basics→808,
  House Basics→909, Stab Board→808).

## 10. Rhythm — arp, sequencer, chord

- **Arpeggiator** — modes (up/down/up-down/random/as-played), **OCT** range, **GATE**, **SWING**,
  **HOLD** (latch), per-step velocity/accent; clocked to tempo; resyncs to the running clock (no
  restart-and-reset at the bar).
- **Step sequencer** — 8-row grid drives a target part (typ. a kit); per-step velocity, its own
  **GATE**, row mutes; changing the target part doesn't hang notes. The default rows are the
  **foundational eight** — kick, snare, rim, closed hat, open hat, crash, ride, low tom — on this
  project's own kit trigger notes (36/38/39/42/43/48/47/44), so the same pattern stays meaningful
  when you switch kits. Row labels name the pad each row actually triggers.
- **Chord engine** — one-finger diatonic chords: root + quality, plus held **modifier keys**
  (Maj/Min/Sus4/Sus2/Dim/Dom7/7th); held chords re-voice live.

## 11. Looper & scenes

- **Four per-part loop lanes**, MIDI **or** audio, locked to the transport; per lane **R**ecord /
  **P**lay / **MIDI-AU** mode / **BARS** (loop length up to 16) / **Q**uantize.
- **Armed recording** — capture starts at the next bar; layers stay in time.
- **Scenes** — 8 slots snapshot the drum pattern + looper clips; **launch on a quantum boundary**
  (bar/2-bar); pending-launch blink + escape hatches; long-press copy/clear; outgoing-edit rule.
- **Export** — bounce recorded loops to **MIDI** or **WAV**.

## 12. Tempo, clock & host sync

- **Internal Tempo** (20–300 BPM) drives arp / seq / looper / synced LFOs.
- **Host-tempo follow** — in a DAW the arp/seq/looper **and** synced LFOs lock to the project BPM +
  play position; the Tempo knob shows host BPM.
- **MIDI clock OUT** — the synth as clock master: 24-ppq clock + start/stop to a selectable output
  (standalone) or the host, ≤ 1-sample jitter (external gear locks to it). Enable in **OUTPUTS**.

## 13. Presets

- **Factory bank** (160+), grouped by category (Bass · Lead · Keys · Pad · Pluck · Brass · Strings ·
  Winds · Organ · FX · Experimental · Drums), alphabetical within category; **Init** pinned on top.
- **RANDOM** — one press = a fully random patch on the focused part, shaped toward playable
  (perceptual distributions, musical SEMI intervals, coherent envelopes, 0–3 shaped mod routes,
  occasional self-osc). **VARY** — a small step from the current sound. RANDOM/VARY never touch
  master, routing, or other parts.
- **Patch program level** — a **TRIM** knob baked with the sound (loudness-matched bank); MASTER is
  yours, never stored in a preset.
- **Mod routes travel with a patch** — a preset can carry up to 8 matrix routes.
- **Save / Rename / Delete** user presets with a category picker; factory patches are read-only.
- **CLEAR** — reset the focused part to a plain sine.

## 14. Input, MIDI & surfaces

- **Multi-surface routing** — assign each connected controller/keyboard to a part (or Live/pinned);
  Launchkey pads as a channel-split routable pad surface; **INPUTS** dialog.
- **MIDI-learn** — right-click / long-press any control to arm; next CC binds it (CC chip shown);
  same gesture clears. Launchkey pots CC21–28 → macros by default; persists in state.
- **Pitch bend + mod-wheel (CC1) + sustain** routed to the played part; hot-plug reconnect re-attaches.
- **QWERTY musical keyboard** (standalone) — play notes, Z/X octave, chord-modifier row; never
  starved by text fields (numeric entry is modal).
- **Double-tap** any control → type an exact value.

## 15. UI, display & help

- **Single-surface hardware-style panel** — top bar · part rail · centre (Osc · Filter · Env · LFO ·
  FX) · right (Scope + Spectrum + per-part EQ) · bottom (Chord · Arp/Seq · Looper). Scales with the
  window; dark LookAndFeel.
- **Section guide** (`?` → menu) — Keyboard Map + the 13 panel sections; pick one → spotlight + numbered
  markers + a side card explaining each control (what + how). Reference, not a tour.
- **Scope** (waveform) + **Spectrum** (FFT) displays.
- **F12 audio-health overlay** — render time, active voices, xruns, clipping.
- **Version/build banner**; **F11 fullscreen** (standalone); grab-mode touch controls.

## 16. Session, persistence & format

- **MULTI** — save/load the whole 4-part layout (patches, kits, routing, mixer) as one file.
- **Session Export** — one-folder DAW handoff: per-part MIDI + WAV stems, master WAV, a
  manifest, offline render of the scene realign cycle.
- **State round-trip** — SOUND persists; routing/parts reset to the default scene on reopen (only
  MULTI recalls a layout). MIDI-learn mappings persist in APVTS state.

## 17. Reliability & engineering (not user-facing, but testable)

- **RT-safe** audio thread (no allocation/locks); observability logging via an RT-safe ring.
- **Click-free** everywhere (a standing rule: any voice stop at non-zero amp gets a tail fade +
  click-torture tests).
- **Cross-platform determinism** — seeded generative content regenerates bit-identically; goldens.
- **Gated** — every change ships behind Release + ASan/UBSan sanitizers + pluginval + CI on Linux
  **and** Windows.

---

### Known honest limitations (by design, this version)
- Kit pads render **dry** (no per-pad FX block) — drum grit is per-voice `filter_drive`.
- Section-guide markers are **functional, not pixel-polished**; the EQ + Scope are custom-drawn
  surfaces so their guide cards have no anchored markers (1.1 UI pass).
- Fully-acoustic drum realism is the **1.1 sampled-kit** pass; you can load your own samples
  onto any pad today.
- Host time-signature other than 4/4 is treated as 4/4 for the bar math.
