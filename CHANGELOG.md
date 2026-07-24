# Changelog

All notable changes to **synth** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Post-1.0 work on `master` (not yet tagged; the ThinkPad validation is the final pre-tag gate).

### Added
- **FX SAT — a real overdrive/fuzz (two-stage clipper), and WIDTH now runs first.** The WIDTH FX
  block gains a **SAT** knob: a per-channel clipper applied *before* the widening — modelled on how
  overdrive/fuzz pedals actually work, not a volume boost. The shaper has **unity gain near zero**, so
  a signal below the threshold passes ~unchanged and only what pokes above clips; turning SAT up
  **lowers the threshold** across the whole range (progressively more clipping), while the knee and a
  smoothing filter split the sweep into **two musically distinct halves**:
  - **0 → ~30 %: onset of a warm SOFT overdrive.** An asymmetric soft-clip knee, then **rounded by a
    smoothing lowpass** so the drive is smooth, not fizzy. The asymmetry (both polarities unity-slope
    at zero, the positive knee reached sooner) generates **even harmonics** only as a note is *driven*
    — so a soft note stays clean and it's genuinely **velocity-sensitive** (measured ~5× more
    distortion on a hard note than a soft one). Tuned so a **single note starts to break up by ~30 %**,
    not only stacked chords.
  - **~50 % → 90 %: hardening toward a FUZZ.** From noon the knee hardens (soft → hard clamp) and the
    smoothing lowpass **opens up**, reaching a full hard square — a **fuzz — by 90 %** (holding there
    to max), with the threshold diving in step so the clip gets rawer and edgier the higher you go.

  Loudness stays flat across the sweep and across velocity via an **envelope-following auto-makeup**
  that restores each note to its own input level *including* the lowpass loss (never a hidden boost —
  the output peak only ever drops), so the per-part **LEVEL** stays the volume control. Runs **2×
  oversampled** while engaged, so even a hard-clipped high note stays clean (measured ~1 % aliasing);
  the wet crossfade folds the oversampling in/out click-free as SAT crosses zero. `sat = 0` is a
  **bit-exact bypass** (goldens hold); it's a full mod destination (an LFO can lean on it). Distinct
  from the filter panel's DRIVE. The block is relabelled **SAT + WIDTH**, and the **default FX chain
  now runs WIDTH first** — you clip/spatialize the dry signal, then delay/reverb bloom over it.
- **Oscilloscope reads bigger.** The master scope's vertical gain is doubled and the horizontal window
  is zoomed in ~20 %, so the waveform shape is easy to read at typical playing levels (hot peaks still
  fold smoothly to the panel edge, never overdrawn).
- **FX blocks reorder by header chevrons, not drag; presets no longer rearrange them.** Each FX
  block's name bar carries small **up/down chevrons** — tap them to move that effect one slot earlier
  or later in the chain (EQ stays last). There is deliberately **no drag gesture**, so grabbing a
  knob can never nudge a block; the chevrons are dimmed at the ends of the chain. The order is a
  **global, user-controlled setting that persists in the session** and is **no longer clobbered by
  loading a SOUND preset** — fixing a bug where every order-less factory preset silently reset the
  chain to the old order (snapping WIDTH out of first place). Tapping the rest of the bar still
  toggles the effect on/off.
- **Chorus VOICES 1|2 (Musicality Pass, Tier 4c — dual-tap thickening).** The CHORUS block gains a
  small **1|2** selector. At **2** a second modulated tap is read per channel at a longer centre
  delay (19 ms) with its LFO at 120°/240° — independent of the first tap's 0°/90° — so the two taps
  give L and R genuinely different motion: a classic dimension-style **thicker, wider** chorus.
  The two taps sum at half weight so the level stays controlled. **Default 1 ⇒ the single-tap chorus,
  bit-identical** (goldens hold); toggling 1↔2 crossfades via a smoothed blend, so it is click-free
  mid-note. Measured: voices=2 lowers L/R correlation and substantially changes the wet voice.
  This completes the **Musicality Pass** (#99): Tier 1 (voicing) → Tier 2 (nonlinear filter: drive,
  self-oscillation, oversampling) → Tier 4a (modulated reverb) → Tier 4c (dual-tap chorus). Tier 3
  (unison character) lives in the unison work item.
- **Reverb MOTION (Musicality Pass, Tier 4a — modulated tail).** Static reverb comb tunings ring
  at fixed frequencies, which reads *metallic* on a long tail. A new per-part **MOTION** knob (in
  the REVERB block) adds very slow, very small modulation to a subset of the reverb's **allpass
  diffusers** — three of the four series allpass, each on its own slow LFO (0.13–0.29 Hz, so there
  is no single coherent wobble), a few samples deep via interpolated reads. Wobbling the diffusion
  continuously reshuffles the echo pattern and **smears the fixed coloration** — pads *swim* instead
  of ringing. Chosen by measurement: modulating the parallel combs instead only *reinforced* the
  resonant peaks, so the allpass (the classic Dattorro-plate approach) is modulated. Measured:
  the time-averaged HF spectral crest drops ~19 % with motion at full depth (the peaks smear).
  **Default 0 ⇒ the classic static reverb, bit-identical** (goldens hold). Zipper-safe on the knob,
  denormal-safe on the modulated decay, and a full mod destination (LINK / automation / MIDI-learn).
  Bench: +~0.004 ms/block (negligible — interpolated reads on three short lines).
- **NOISE is now a visible control.** White noise was always a fourth sound source in the engine
  but had **no knob** — you couldn't reach it. It now has a **LEVEL** knob in a fourth source row
  beneath the three oscillators (aligned under their LEVEL column), a full mod destination (LINK,
  automation, MIDI-learn, numeric entry). (Its COLOR — white/pink — is a documented post-1.0 slot.)

### Fixed
- **Velocity now shapes volume properly, and does so PERCEPTUALLY.** The default **VEL>AMP** was 0.7
  with a *linear* amp map, which left even the softest note at 70 % of full amplitude — velocity
  barely moved the level. Now the default is **0.9** and the curve is **dB-linear (logarithmic)**:
  equal velocity steps give equal loudness (dB) steps below unity, matching how hearing works, so
  soft notes are genuinely quiet and dynamics feel even across the range (>1.0 accents get a gentle
  linear boost). velocity's other routings (cutoff, etc.) are unchanged. The render golden was
  regenerated for this intended change.
- **Turning a sequencer/arp step off is now a single tap.** A step used to require a *double-tap* to
  silence (a stray single tap was ignored), which was fiddly — the double-tap window was tight and a
  slightly-long press slid into velocity mode instead. Now **a single quick tap toggles** a step
  (dark→on, lit→off); only a deliberate hold or a clear vertical drag enters velocity mode, so
  turning steps off is easy and velocity edits still can't happen by accident. (Applies to both the
  step sequencer and the arp gate row.)

### Fixed (earlier)
- **Windows CI green again (#110).** Two test-suite `TEST_CASE` names contained an **em-dash (—)**.
  ctest re-invokes each test by name as a Catch2 filter; the UTF-8 em-dash round-trips on Linux but
  mangles on the Windows console codepage, so the exe matched no test and ctest called it a failure —
  Windows `build-test` had been red since the Musicality Pass Tier 1 for this reason (it was never a
  DSP determinism bug; the audio was always bit-identical). Renamed the two tests to ASCII and added
  an **ASCII-only-test-name guard to the gate** (`run-all-checks.sh`) so it can't recur.

### Changed
- **UI audit + layout polish.** A full parameter-vs-control audit (every registered parameter now
  has a control, or is intentionally hidden). Highlights: the **macro row** shows full assignment
  names (no truncation) with the CC badge relocated so it never collides, and a **value readout that
  appears only while you adjust** a knob (nothing at rest); **sequencer rows** show the pad name *and*
  trigger note ("Snare D1", "Snare E1") so no two read alike; **VARY** sits next to RANDOM; the
  **chord modifier** keys are now keycap badges; the **oscilloscope trace runs hotter** (fills the
  panel, soft-clipped) and the EQ gets more room; **envelope values** are transient (ms/s, shown only
  while dragging); and hover **tooltips + the help overlay** now explain the cryptic controls (osc
  phase RS/RN/FR, ANALOG, DRIVE, LFO SYNC/DIV, NOISE).
- **Removed 12 dead parameters** (the retired master EQ `eq_*` and `arp_latch`) — pre-1.0 cleanup of
  never-functional params ahead of the ID freeze. Old sessions carrying them load cleanly (values
  discarded). `osc_mix` is retained (it drives legacy osc-level migration, so it is not dead).

### Added (cont.)
- **Cleaner filter drive (Musicality Pass, Tier 2C — 2× oversampling).** The in-loop tanh
  saturation aliases at base rate — audible as harshness on high, hard-driven notes (measured
  −22 dB at note C7, drive/self-osc). The driven/self-oscillating filter now runs **2× oversampled**
  (a contained half-band around just the filter), cutting that aliasing by **~10–12 dB** at the
  worst case. It engages **only when a voice is actually driven or self-oscillating** (a clean voice
  stays on the bit-exact base-rate path and pays nothing), and it's **latched for the note's whole
  life** so the rate never switches mid-note (no click). Self-oscillation stays in tune at 2× (±1.5
  cents). Evaluated against folding into the oscillator oversampling domain and benched both:
  the contained 2× wins on cost (half the added work) and diff size, and keeps the clean path
  bit-exact. Active in both Efficient and HQ so the fix reaches the live machine.
- **Filter SELF-OSCILLATION (Musicality Pass, Tier 2B).** Push **RESO** past the top and the filter
  blooms into a **pure, keytracked sine at the cutoff** — a playable voice (sine bass, whistles,
  drones), the way a cranked analog filter sings. It **starts reliably even with no input**: the loop
  carries an inaudible (~-120 dB) noise floor, the digital stand-in for analog thermal noise, so
  silence always blooms (in ~0.2 s). It **plays in tune** — the self-oscillation lands within ~1.5
  cents of the note across the whole keyboard at full keytrack. Old presets are safe: RESO's range is
  unchanged and **only the very top sliver opens into self-osc** (everything below is bit-identical),
  and randomize never reaches it. Decaying tails are denormal-safe.
- **Filter DRIVE (Musicality Pass, Tier 2 — the big one).** The state-variable filter gains an
  in-loop **tanh saturation**: a new **DRIVE** knob (per part) soft-clips the signal entering the
  filter loop and bounds the resonance-feedback path — the way the classic analog filters get their
  growl (saturation *inside* the loop, not a waveshaper in front). Clean playing is untouched:
  **drive 0 takes a bit-exact fast path that is literally the old linear code** (goldens hold, and
  you pay for the tanh only when driven). The nonlinearity uses a fast rational tanh (pinned within
  ~0.024 of `std::tanh` by the DSP suite); driven output is makeup-matched to stay within ~2 dB of
  clean across the range, and the drive amount is **smoothed** so knob/automation/macro moves don't
  click. DRIVE is macro-routable.
- **Analog life for the oscillators (Musicality Pass, Tier 1).** Each oscillator gains a
  **start-phase policy** — **RESET** (today's bit-identical alignment), **RANDOM** (a fresh phase
  per note, so detuned stacks and chords bloom differently every strike), or **FREE** (the
  oscillator runs continuously and a note picks up wherever the phase is). A global **ANALOG** knob
  adds subtle per-voice pitch/PW **drift** (a slow bounded random walk) — the "alive" quality of
  vintage polys. Both default to off (RESET / analog 0), so existing patches are bit-identical; the
  new controls live in the oscillator rows (phase) and beside GLIDE (ANALOG).
- **MIDI clock OUT (#85) — the synth as clock master.** Transmits **24-ppq MIDI clock + start/stop**
  derived from the same transport as everything else: **standalone** sends the internal Tempo, and
  in a **DAW** it relays the host tempo + play state. Enable it (and, in the standalone, pick the
  MIDI output device) in the new **OUTPUTS** dialog, so external gear (Aeros looper, Chase Bliss
  pedals) locks to the synth. Clock ticks are placed at sample-accurate offsets — jitter is **≤ 1
  sample (~21 µs)**, far tighter than pedal loopers need. The instrument's MIDI output carries only
  the clock (it never echoes played notes).
- **Scenes (J3).** Eight arrangement snapshots — **loop clips (MIDI + audio) + drum pattern +
  per-lane transport** — as a row of numbered buttons in the looper section. The **active scene is the live
  state**: recordings and pattern edits write into it automatically (no "store" step). **Tap** a
  scene to launch it; the switch is **quantized** and, by default (**Loop end**), waits for the
  **longest loop in the current scene to finish** so a single tap never cuts a phrase short — and it
  won't switch until any in-progress **recording** has completed too. A newly-activated scene
  **starts from its beginning** (the loops rewind to the downbeat rather than resuming mid-phrase),
  and the flip is click-free (held loop notes are flushed). A shorter quantum (**1 / 2 / 4 / 8 bar**)
  is selectable if you want faster switches. **Long-press** a scene for a menu: *Copy active scene
  here* (clone) or *Clear scene*. Launching an empty scene is a valid blank canvas. Buttons show
  empty (outline), filled (has content), pending (blinking), and active (solid). **Audio loop
  recordings are per-scene too** — captured lazily (only the recorded region, so memory scales with
  what you actually record); scene content is session-runtime (exported via MIDI/WAV).
- **Hover help tooltips.** Resting the mouse on any control for ~1 s now shows its full name
  (e.g. hovering the looper **R** button shows "Loop Rec", a knob shows "Filter Cutoff"). The
  label comes from the control's parameter, so every knob, selector, and toggle is covered.
- **Per-part looper loop lengths, up to 32 bars (J2).** Each of the four looper lanes now sets
  its **own** length — a compact **BARS** knob on every row (turn to **1 / 2 / 4 / 8 / 16 / 32**)
  instead of one shared grid. So a 2-bar drum groove on P4 can loop under an 8-bar chord progression on P1, and the
  lanes stay locked to a single downbeat (a shorter loop simply wraps a whole number of times inside
  a longer one — driven by one master clock, `masterPos % laneLength`, so there is no phase drift).
  **MIDI** loops offer all lengths at any tempo; **AUDIO** loops are honestly capped by the ring size
  (shown as "aud Nb" on the row when a slower tempo can't fit the selection). Old sessions restore
  unchanged (the length list was extended append-only).
- **Master tempo linking + tempo-synced LFOs (J1).** In a DAW the synth now **follows the host's
  tempo and transport** — the arpeggiator, step sequencer, looper **and** LFOs lock to the project
  BPM and play position (via `AudioPlayHead`); standalone keeps using the internal **Tempo** knob.
  Each of the three per-part LFOs gains a **SYNC** toggle: when on, its **RATE** knob morphs into a
  stepped **DIV** (note-division) knob — **4 bar, 2 bar, 1/1…1/32** straight, **1/4T–1/16T** triplet,
  **1/4.–1/16.** dotted (14 divisions). A synced LFO's phase is derived from the transport position,
  so it stays **bar-locked with no phase jump** — even for triplet/dotted divisions that don't
  divide the bar evenly, and across tempo changes and host loop braces. Toggling SYNC on engages at
  the next bar boundary (no mid-note jump); toggling off freezes the current rate as free Hz. The
  effective rate is computed live in the engine, so **locked parts** track tempo changes too.

- **Sample-playback kit pads (I2).** Any drum-kit pad can now play a loaded **WAV / AIFF / FLAC**
  one-shot instead of a synthesized voice — load it in the **Kit Editor** ("Load sample…"; the pad
  shows **SMPL**). Playback is **stereo**, **pitch-tracked** (the sample transposes with the note;
  4-point cubic interpolation, which also does the file-rate→engine-rate conversion), and
  **play-as-recorded** by default (root = the pad's note, ratio 1.0). Samples get the pad's level,
  choke group, trigger/sounding notes, and the part's FX + EQ + pan like any pad, with a short
  anti-click fade at each end and on choke. Files live in a **managed, content-deduplicated**
  library (`~/.config/synth/samples/`), so a pad references a sample by content hash — five kits
  loading the same file share one copy, and the reference travels with `.kit` files, sessions, and
  MULTIs. A missing sample is silent, never a crash.
- **Launchkey drum pads are their own routable input surface (I1).** A controller whose pads
  send on a separate MIDI channel + note range (declared in its device profile — the Launchkey
  Mini's 16 pads are notes 36–51 on channel 10) now split off into an independent
  **"&lt;device&gt; Pads"** surface. It appears as its own row in **INPUTS** right under the device, so
  the pads can route to a different part than the keys — e.g. pads → the P4 drum kit while the
  keys play a lead. The split is data-driven (`padChannel` / `padNotes` in the profile JSON), so
  the pads get their own key-range zones, activity indicator, and MULTI persistence like any
  surface; keys and other CCs stay on the device's own surface.

### Changed
- **The per-part EQ reaches drum-kit parts, and kit parts are now focusable (K2).** Tapping any
  part cell — synth **or** kit — moves the shared edit-focus, so the EQ section (and the part's
  channel) follows it. A kit's summed output now runs through its per-part EQ (the creative FX —
  chorus/delay/reverb/width — stay dry on kits, only the EQ passes through). Because a drum kit has
  no single synth sound, while a kit is focused the OSC / FILTER / ENVELOPE / LFO panels dim with a
  "KIT — edit pads in Kit Editor" hint and a one-tap button; the kit keeps playing from its per-pad
  voices. Each part remembers its own EQ across focus changes and in the session. *(Previously the
  whole FX chain was bypassed for kits, and tapping a kit refused edit-focus — so its EQ was
  unreachable and the FFT showed no EQ shaping on drums. The master scope/FFT was already post-EQ.)*
- **One EQ, per part, at the end of the chain (K1).** The plugin now has a single EQ concept:
  a fixed **5-band parametric EQ** applied as the **last stage of the focused part's chain**
  (post-FX), living in its own right-column section that **follows edit focus** (the header
  names the part). It replaces two older, overlapping EQs — the **master finisher EQ is
  retired** (its `eq_*` params stay registered but are inert/hidden for state back-compat), and
  the per-part EQ is **removed from the FX drag-reorder chain** (that chain is now the four
  reorderable FX: chorus/delay/reverb/width). The new section is a mixing-desk surface: a
  vertical **gain slider per band** (drag up/down = gain, **drag sideways = frequency** with a
  live readout, **double-tap = numeric freq/gain/Q**), a per-band on/off dot, and a section
  on/off bar; editing any band auto-enables the section so a boosted band is never silently
  bypassed. The four band gains (`EQ Low/L-Mid/H-Mid/High Gain`) are mod-matrix / macro targets.
  Old presets migrate transparently: an EQ anywhere in a saved `fx_order` now always runs last
  (its slot is inert), and band 4 + the per-band switches default to a neutral, on state.
  *Fixes a latent bug:* the per-part EQ previously did nothing on the LIVE/focused part
  (`snapshotFXParams` never carried it) — it now works on every part.
- **Macros ship pre-assigned.** The 8 macros now default to musical targets — M1 filter
  cutoff, M2 resonance, M3 filter-env amount, M4 amp release, M5 LFO1 rate, M6 LFO1 depth,
  M7 reverb mix, and M8 the **focused part's level** (follows the edit focus). Older sessions
  with no saved macro map inherit these defaults; an explicitly-cleared map is respected. The
  Launchkey Mini pots (CC 21–28) already drive M1–M8, so the controller is expressive out of
  the box.
- **Per-step velocity on both rhythm surfaces.** Every sequencer step AND every arpeggiator
  step carries its own velocity percentage (10–200 %), edited with one grammar on both grids:
  **single-tap a dark box turns it on; double-tap a lit box turns it off** (a stray tap never
  silences a step); **touch-and-hold a box then drag up/down sets its velocity** — shown as a
  number in the box while adjusting and a bottom-up fill (accented, > 100 %, brightens) at
  rest. On the arp the velocity belongs to the STEP, not the note — the same box scales
  whatever note the pattern lands on it. Replaces the old binary sequencer accent (legacy
  accented steps migrate to a high velocity). Persists with patterns/presets/MULTIs; states
  without per-step velocities load at 100 %. (The brief single arp-velocity knob added earlier
  in this cycle is retired in favour of per-step control.)
- **Clock alignment:** the sequencer, arpeggiator and looper now share one transport origin
  (the loop clock), re-anchoring to the bar downbeat every bar — the seq no longer leads the
  arp/looper. Swing self-accumulates within the bar. Looper MIDI recording is quantized to a
  1/32 grid (per-lane toggle, default on) with note-pairing protection.
- **Looper is now 4 fixed per-part lanes** (lane N ↔ part N), each with its own
  REC/PLAY/CLEAR/MIDI-AUDIO transport and its own audio ring. Capture is by lane (part N),
  fully decoupled from the edit/play focus (switching focus no longer disturbs the looper).
  Shared loop-grid; honest audio-bar cap at low tempo; per-lane UI rows. REC is one-shot:
  arm, engage at the loop downbeat, record exactly the set bars (1/2/4), then auto-stop and
  play.
- **Poly/Mono/Legato is now per-part** (edited via focus like the rest of the sound). Each
  part has its own mode, mono voice, note stack and glide; kit parts are always poly. Fixes a
  real isolation break — a mono lead on part 1 was cut whenever the sequencer ran on part 4
  (global single-voice mono). The master EQ / per-part EQ, filter, FX and LFOs are all per-part.
- **Per-part 3-band parametric EQ** as a 5th reorderable FX block; the master EQ stays a global
  finisher. Every FX/EQ on/off header is a loud lit toggle.
- **Stereo width now widens a dry mono source.** width > 1 synthesizes side content from
  the mid via a Schroeder allpass cascade (phase-only, not a Haas delay), added purely
  antisymmetrically so the mono fold-down is unchanged. width ≤ 1 and width == 1 unchanged.
- **Default scene:** the drum kit moved to **P4** (the sequencer's default target); P3 stays
  the bass, P2 is now the free spare.
- **808 / Punchy kick voicing:** amp attack softened 1 ms → 2 ms for a defined transient.

### Fixed / investigated
- **FX SAT is now obvious, and WIDTH-first reaches existing sessions.** Follow-up to the SAT
  feature: the knob felt inert because most of its travel crossfaded dry/wet at low drive. It now
  reaches **full wet by ~8 %** of the sweep, so the rest of the knob controls **drive** — a small
  turn already bites and the top is heavy (drive range 8× → **20×**; measured THD ~0.38 at full
  vs ~0 clean). The tube even-harmonic colour lives at moderate settings; cranked, it goes fuzzy
  (odd), as a real tube does. Separately, the **WIDTH-first default** didn't reach anyone with a
  saved session (the persisted `fx_order` restored the old order); a one-time, version-stamped
  migration now moves a legacy session that carried the *old default* `[0,1,2,3]` to width-first,
  while leaving any custom order — or a deliberate new save — alone.
- **MIDI auto-detect at startup restored.** A controller present at launch played only after an
  unplug/replug: it was enabled before our routing callback was attached (JUCE wires the callback
  at device-open time). The app now reopens already-enabled inputs after attaching the callback.
- **INPUTS: "Live" and "Part 1" are now separate routing choices.** A surface's routing offered
  *P1 (Live)*, P2, P3, P4 — but "P1" secretly meant **follow the focused part**, so you could
  never *pin* a surface to Part 1 (it always chased whatever part was in focus). Each surface now
  chooses **Live** (follows the edit/play focus — the default) **or** a fixed **Part 1–4** that
  plays that part regardless of focus, exactly like Parts 2–4 already did. Implemented as a
  `kLivePart` (-1) sentinel distinct from part index 0; the routed-event FIFO carries a *signed*
  part so the sentinel survives (it was previously stored unsigned and any negative part was
  silently dropped). MULTI files are version-stamped: a legacy layout that saved "P1 (Live)"
  (part 0) still loads as **Live**, while newly-saved *Part 1* pins correctly. Behavioural test:
  a Live surface follows the focus; a Part-1-pinned surface sounds on part 0 even with focus
  elsewhere.
- **Launchkey (any MIDI input) going dead after a hot-plug reconnect.** A controller worked at
  startup but delivered nothing after a disconnect→reconnect (no notes, nothing in the F12
  monitor) until an app restart. On reconnect JUCE reopened the device with its "enabled" flag
  still set, so the all-device MIDI callback was never reattached — enabled yet silent. The
  hot-plug watcher now forces a clean disable→enable on reconnect and re-asserts the all-device
  callback, so a reconnected device delivers again. (Hardware-confirmed.)
- **808 kick "double-hit pop" (two kicks in a row).** Re-striking a percussive sound while its
  tail still sounded re-attacked the voice **in place**, which clicked: the amp re-attack corner
  **and** the mod-envelope pitch restart (Kick 808 sweeps pitch **+22 st**) both landed as slope
  discontinuities against the live tail — the worst inter-sample step of the whole render, ~2.6×
  the kick's own onset. A percussive voice (amp env with no sustain) now **re-strikes from silence**
  instead: the old tail gets a quick fade and the new hit starts on a fresh voice (phase 0,
  envelopes from 0), so both corners vanish and the brief overlap is smooth. Sustained sounds are
  unchanged (they still retrigger in place, which is click-free for them). Regression-tested in the
  DSP suite (the re-struck step must stay within the onset's own ceiling).
- **A matched controller profile is now authoritative.** Plugging in a Launchkey re-asserts
  its 8 pots (CC 21–28) onto the 8 macros on connect, overriding any stale/learned binding an
  old session left on those CCs — so the pots always drive the macros with no manual step. The
  match is broadened to any "Launchkey" (Mini/MK3/25/…); learn still wins live until the next
  hot-plug. (Reset MIDI + macros remains as a manual factory-restore.)
- **Standalone launches full-screen** by default (the recommended live mode — no OS title bar
  for a touch drag to catch); the maximise button still toggles back to a window.
- **Reset MIDI + macros, and touch-friendly macro knobs.** Added a **Reset MIDI + macros**
  button (INPUTS dialog) that restores BOTH the controller map (CC 21–28 → the 8 macros,
  clearing any learned/stale binding) AND the macro→target assignments (M1 cutoff … M8
  focused-part level) — recovers the Launchkey pots and the macro destinations when a past
  session had them pointing elsewhere. The top-bar macro knobs now also accept **horizontal**
  drag (not just vertical), so on a windowed touch screen you can adjust them sideways instead
  of dragging up into the OS title bar (which would grab the drag and move the window).
- **Per-step velocity now audibly shapes the note.** The seq/arp emit was clamping velocity
  to `min(1.0, …)`, so a step at 100 % already emitted the maximum and the whole 100–200 %
  "accent" range was inert. Velocity is now a real `0.1–2.0` scalar (100 % unchanged; > 100 %
  boosts, < 100 % ghosts). It reaches the voice and drives BOTH the VCA (`vel→amp`) and the
  filter (`vel→cutoff`) — louder *and* brighter on a harder step — verified end-to-end. The
  output safety clipper still guarantees the bus never exceeds ±1.0 on an over-unity accent.
  (Brightness response is per-preset via `vel_to_cutoff`; amplitude response is on by default.)
- Investigated a reported width/EQ "does nothing": both work in the real processor topology
  (added real-topology tests); width was a mono no-op (now fixed above), the master EQ works.
- Investigated a reported 808 kick "HF click/pop": the kick is **engine-clean** (measured far
  below the click standard, single hit and rapid retrigger; block-size-independent) — locked
  in by a regression. Remaining transient character is preset voicing.

## [1.0.0] — 2026-07-13

First public release: a JUCE 8 / C++17 virtual-analog polysynth (VST3 + Standalone,
Linux-first) built for a 2015 dual-core ThinkPad live target. Every change shipped
test-first behind a full gate (`run-all-checks.sh` + `--sanitize` + bench).

### Synth engine
- Three PolyBLEP oscillators (saw / square+PWM / triangle / sine), 4× oversampled with a
  configurable quality/CPU tradeoff (`Efficient` audible-band-clean default, `HQ` full-band
  for studio); per-source levels, octave, detune, noise, kill switches.
- Cytomic TPT state-variable filter (LP/HP/BP/Notch) with control-rate cutoff, key-track,
  and filter-envelope amount; dedicated amp + mod (filter) ADSRs; mod-env → pitch.
- Click-free voice stealing and retrigger (phase only resets on an idle voice); 24-voice
  pool with a fixed voice-sum headroom trim (goldens bit-identical across pool sizes).
- Mono / legato / poly modes with glide (portamento); pitch bend, mod-wheel vibrato, and
  sustain-pedal (CC64) handling from any device.
- Per-block parameter smoothing (cutoff/resonance/osc-mix + master gain) — no zipper noise.

### Multitimbral, kits, mixer
- Four parts: part 0 live + parts 1–3 locked, each with its own baked voice params, FX
  chain, and three LFOs (race-free lock-free publish). Silent parts with idle FX are skipped.
- Drum **kit parts**: per-pad synth voices, choke groups, note-off ledger, learn-by-play,
  per-pad editing (open the full synth panel on any pad), factory kits + user `.kit` files.
- Per-part **mixer** (level + 0 dB-centre balance pan), MIDI-learnable, captured in MULTI.
- **Full per-part isolation**: a generator (sequencer/arp/looper) always yields its voices to
  live playing, so a running pattern can never cut a note you play; editing one part (or one
  kit pad) never affects another.

### Performance surfaces
- Diatonic **chord engine** with momentary QWERTY / CC / consumed-note modifiers that
  re-voice even held chords (major/minor/sus/dim/7th/dom7).
- **Arpeggiator** + 8-row **step sequencer** (dedicated drum grid), clock-linked, targeting a
  single part.
- **Looper**: clock-linked, armed + measure-quantized, dual MIDI + AUDIO lanes, WAV export.
- Multi-surface **input routing** (QWERTY + per-MIDI-device → parts) with key-range split
  zones; plug-and-play MIDI hot-plug, device profiles, and precedence.

### Mix / master / modulation
- Master 4-band parametric EQ (low shelf, two bells, high shelf) at the end of the chain,
  default-flat bit-identical bypass.
- Eight **macros**, routable to any parameter and assigned by Random; Launchkey pots drive
  them out of the box.
- Spliced safety clipper and voice-sum gain staging — output never exceeds ±1.0.

### UI / UX
- Hardware-style custom editor: left part rail (P1–P4 + kit-pad seam), signal-flow centre,
  right oscilloscope + FFT (RT-safe SPSC tap), slim top bar, chord/rhythm/looper bottom
  zones, `?` help overlay, fullscreen (kiosk) mode. Touch-reliable (Surface-confirmed);
  nothing on the main panel steals keyboard focus.
- **Default startup scene**: P1 lead · P2 spare · P3 bass · P4 808 kit (the sequencer's
  target) — playable and audibly distinct out of the box.
- Edit-focus: tapping a part swaps the whole panel to its sound with per-part persistence,
  MULTI edit-capture, and revert. Double-tap numeric entry; LFO→knob modulation animation.
- Factory preset library (16 patches across categories) + Init; **loading a patch is
  sound-only** — it never disturbs the sequencer, looper, tempo, macros, mixer, or other parts.

### Persistence & recall
- APVTS state round-trip (incl. persisted MIDI-learn maps); a preset persists SOUND only.
- **MULTI** save/load is the one explicit recall of the full multitimbral layout (parts,
  locked presets, kits, routing, zones); routing/zones otherwise reset to the default scene
  on relaunch.

### Standalone / platform
- Standalone app with just-works PipeWire output, QWERTY keyboard input, and robust input
  reliability across focus/lifecycle events.
- Observability: RT-safe SPSC ring logger, audio-health telemetry, F12 debug overlay, crash
  handler, provenance banner.

### Engineering
- JUCE-free `Source/DSP/` (std-only), frozen parameter IDs, dumb voices (all APVTS access in
  a snapshot), audio thread free of allocation/locks/IO.
- Catch2 test suite (DSP + plugin-layer + RT-alloc guards + golden render + click-torture
  noise-cleanliness), pluginval strictness 8, ASan/LSan/UBSan + MIDI-storm soak, and a
  `dsp_bench` reporting block time against the ThinkPad budget. CI green on Linux + Windows.
- Licensed under **AGPLv3**.

### Known limitations / deferred to v2
- On-ThinkPad round-trip latency + voice-cap validation under PipeWire is the final
  pre-deploy check (`tools/thinkpad-validate/`); the dev-box ×3.5 derate flags the 24-voice
  pathological worst case, so the measured ThinkPad number is the arbiter for the voice cap.
- Not in v1 (planned for v2): mod matrix, MIDI-clock sync, unison/detune stacking, oscillator
  hard-sync / cross-mod, sub-oscillator, filter drive, and a preset browser.

[1.0.0]: https://github.com/Sanglock81/synth/releases/tag/v1.0.0
