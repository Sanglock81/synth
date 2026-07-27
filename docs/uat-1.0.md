# synth — 1.0 User Acceptance Test (UAT) script

**This is the final human gate before tagging v1.0.0.** It is a hands-on pass on the
**real live hardware** (the 2015 ThinkPad under PipeWire), run over **multiple sessions**
with free playing in between the structured checks — the point is to find what automated
tests can't: feel, latency, clicks, stuck notes, and cross-feature interactions.

### How to run it
1. Work top to bottom. Each check is **action → expected result → tick the box**. If it's
   wrong or feels off, **don't tick it** — log it in the **Defect log** at the bottom.
2. Between sections, **just play** — build patches, jam, loop, switch scenes. Note anything
   that surprises you.
3. Every defect is triaged **BLOCKER** (must fix before tag) vs **KNOWN-ISSUE** (ship with a
   note). Blockers loop back through fix → re-gate (`run-all-checks.sh` ± `--sanitize`, CI
   green) → re-test that item here.
4. **The tag fires only on your explicit call**, after the ThinkPad validation report (#100)
   and after you're satisfied here — never automatically, never alongside the report.

### Environment — fill in before you start (both machines)
| | **Machine 1 — Linux live rig** | **Machine 2 — Windows tablet** |
|---|---|---|
| Model | ThinkPad X1C 3rd gen (2015, dual-core Broadwell) | Surface (____ model) |
| OS / kernel / build | | |
| Audio backend + buffer | PipeWire, ____ samples @ 48 kHz | ASIO (Focusrite), ____ samples @ ____ kHz |
| CPU governor | (bench valid only at **performance**) | (power plan: ____) |
| Display scale / DPI | 1× | ____ % (high-DPI) |
| Build **version + git hash** | _(banner must match HEAD)_ | _(banner must match HEAD)_ |
| Format(s) tested | ☐ Standalone ☐ VST3 (host: ____) | ☐ Standalone ☐ VST3 (host: ____) |

The Linux ThinkPad is the primary live target (sections A–R). The Windows Surface pass is
**Section R** (touch, ASIO, high-DPI, VST3-in-DAW, multi-instance). Run the Linux pass first.

---

## A. Pre-flight

- [ ] **A1. Launch** — the standalone opens to the default scene (P1 lead / P2 spare / P3 bass / P4 808 kit) and is immediately playable.
- [ ] **A2. Version banner** — the shown version + short hash **match the build you intend to tag** (HEAD). A stale banner = wrong binary.
- [ ] **A3. Audio device** — output goes to the expected endpoint (curated PipeWire default); no console spew of xruns at idle.
- [ ] **A4. MIDI in** — Launchkey Mini MK3 keys play the live part; the pad-connected indicator/monitor shows incoming messages.
- [ ] **A5. Latency feel** — playing the keys feels tight (this is the subjective companion to the #100 measured round-trip latency).

## B. Oscillators & tone

- [ ] **B1. Waves** — each osc wave (Saw / Square / Tri / Sine / **WT**) audibly changes timbre; no wave renders as a duplicate/wrong label.
- [ ] **B2. Octave / detune / PW** — osc octave (−2…+2), detune (±100 c), and pulse-width sweep sound correct; PW only affects the square.
- [ ] **B3. Osc mix / levels / noise** — osc1/2/3 levels and noise blend as expected; muting an osc silences only it.
- [ ] **B4. Wavetable morph** — on a WT osc, **WT POS** morphs the frame smoothly (no zipper); tapping the WT selector picks a different table; the **die** button re-rolls a random table and the sound changes.
- [ ] **B5. Unison** — **UNI** stacks voices (1 = off), **DET** spreads pitch, **WID** spreads stereo; at high counts on the live/Efficient profile it caps (doesn't cut) and stays clean.
- [ ] **B6. Analog drift** — ANALOG adds subtle per-voice pitch/PW wander; at 0 the tone is static.

## C. Filter & envelopes

- [ ] **C1. Filter types** — LP / HP / BP / Notch each shape the sound correctly; cutoff + resonance sweep cleanly; **self-oscillation** at high reso is stable (no runaway).
- [ ] **C2. Filter drive** — drive adds dirt without blowing up level.
- [ ] **C3. Keytrack** — with keytrack up, the filter opens as you play higher.
- [ ] **C4. Amp ADSR** — attack/decay/sustain/release behave; a fast attack has no click, a long release tails smoothly.
- [ ] **C5. Filter/mod ADSR + env→pitch** — the mod envelope drives the filter and (via env→pitch) bends pitch; drum-style instant-attack pitch drops work.
- [ ] **C6. Velocity** — vel→amp changes loudness with playing force; vel→cutoff opens the filter on hard hits where a patch uses it (EP "bark"), and stays flat where it shouldn't (organs, drums).

## D. LFOs (×3)

- [ ] **D1. Free LFOs** — rate/depth/shape work; routing an LFO to cutoff/pitch/PW modulates audibly; depth 0 or dest off = inert.
- [ ] **D2. SYNC** — toggling **SYNC** morphs the RATE knob into a division knob; a synced LFO locks to the beat (start the sequencer and hear the wobble on the grid).
- [ ] **D3. Tempo-follow** — change tempo and the synced LFO follows; triplet/dotted divisions feel right.
- [ ] **D4. No clicks on transitions** — toggling SYNC mid-note, and a tempo change, produce **no click** on a cutoff-routed LFO.

## E. FX chain (per part)

- [ ] **E1. Chorus** — rate/depth/mix; the 2-voice mode widens vs 1-voice.
- [ ] **E2. Delay** — time/feedback/mix/spread; feedback stays controlled (no infinite build to clipping).
- [ ] **E3. Reverb + motion** — size/damp/width/mix; **MOTION** adds a slow modulated shimmer to the tail.
- [ ] **E4. Width / SAT** — stereo width widens; **SAT** adds saturation that responds to how hard you play; output stays bounded.
- [ ] **E5. Per-part EQ** — the 5-band EQ shapes the part; on/off is audible; it applies per part (not global).
- [ ] **E6. FX reorder** — the reorder chevrons change processing order and it sounds different (e.g. reverb-before vs after distortion).

## F. Master & macros

- [ ] **F1. Master gain** — sets output level; **loading any patch never changes it** (it's your performance control).
- [ ] **F2. Master EQ** — the master parametric EQ shapes the final bus.
- [ ] **F3. Macros** — the 8 macros move their targets; the **dynamic labels** show what each controls; Launchkey CC 21–28 drive M1–M8 out of the box.
- [ ] **F4. Macro RESTORE** — the restore action puts the Launchkey macro defaults back after they've been reassigned.

## G. Mod matrix (LINK / MOD)

- [ ] **G1. LINK touch-connect** — arm LINK, tap a source then a destination control → a route is created and the destination animates with the source's motion (shows **motion**, not mere existence).
- [ ] **G2. MOD overlay** — the overlay lists routes by category; you can edit depth and remove a route.
- [ ] **G3. Source × dest sweep** — a few representative routes (LFO→cutoff, macro→delay-feedback, mod-wheel→reverb-mix, velocity→something) all actually modulate.
- [ ] **G4. Persistence** — a route survives save→reload of the patch.

## H. Presets, TRIM & loudness

- [ ] **H1. Load menu grouping** — the Load menu shows **Init pinned on top**, factory patches grouped by category in musical order (Bass→…→Drums), **alphabetical within each category**.
- [ ] **H2. Browse the whole bank** — click through **all 61 patches**; each loads and makes sound; names are clean (no file-number prefixes); nothing is silent or broken.
- [ ] **H3. Equal loudness** — switching between **sustained** patches (leads/pads/keys/bass) does **not** jump the volume noticeably (±4 dB matched via TRIM). Percussive/dark patches (plucks, EPs, drones) are matched by feel — a bit louder/quieter is expected there.
- [ ] **H4. TRIM control** — the **TRIM** knob in the top bar shows the loaded patch's program level (e.g. Full Organ reads low); turning it changes that patch's level; it's **saved with the patch** and **not** touched by RANDOM.
- [ ] **H5. Save + category** — Save opens the name dialog **with a Category picker**; save a patch under a category → it appears under **User > <category>** in the Load menu; a patch saved as "User" lands in the plain User section.
- [ ] **H6. Save round-trip** — reload a saved user patch; it recalls the full sound, FX order, and learned MIDI mappings exactly. Master level is **not** changed by the load.
- [ ] **H7. Init / CLEAR** — Init resets to defaults (master untouched); CLEAR blanks the focused part to a single sine with globals intact.

## I. Randomize

- [ ] **I1. RANDOM** — produces a usable, musical patch (not noise); it changes **only the focused part's sound** — mixer, EQ, macros, other parts, tempo, seq/looper all stay put.
- [ ] **I2. VARY** — perturbs the current patch into a neighbour (recognisably related, not a full re-roll).
- [ ] **I3. Repeatability** — RANDOM never leaves master, velocity routing, or performance/rhythm state altered.

## J. Drum kits

- [ ] **J1. 808 Basics** — all 16 pads (triggers 36–51) fire a distinct drum; the two hats **choke** each other; the cymbals (crash/splash/ride) **ring free**.
- [ ] **J2. House Basics** — loads on a part; tighter kick, snappy snare, crisp hats + shared claps/toms/cymbals; 16 pads filled.
- [ ] **J3. Industrial** — driven kick, noise snare, clanging Metal Hit (+ tuned metal toms); 16 pads filled.
- [ ] **J4. Stab Board** — four drums + four tuned minor-triad chord pads play as chords from single hits.
- [ ] **J5. Kit picker** — the per-part "Load drum kit" menu splits **Factory / User** and shows each kit's **pad count**.
- [ ] **J6. Kit editor** — open the editor on a kit part; learn a trigger + sounding note by play; change a pad's source preset, level, choke; **Audition**; edit a pad's voice on the main panel.
- [ ] **J7. Sample pad** — load a WAV onto a pad; it plays pitch-tracked with a clean tail (no click); clear it.
- [ ] **J8. Dense mash** — hammer a kit with fast 16ths including the crash → stays clean, no NaN/blowup, output bounded.

## K. Rhythm — arp, sequencer, looper, scenes

- [ ] **K1. Arpeggiator** — mode/octaves/gate/swing/hold work; per-step/arp velocity affects dynamics; disabling the arp mid-hold **flushes held notes** (no stuck note).
- [ ] **K2. Step sequencer** — the 8 rows fire their drums; per-step velocity/accent works; the default rows map across the kit (kick·snare·clap·hats·tom·crash·cowbell); changing the seq **target part** doesn't hang the old part's note.
- [ ] **K3. Looper record** — arm REC on a lane; it records on the bar boundary and plays back locked to the clock; PLAY/mute toggles the lane.
- [ ] **K4. 4 lanes independent** — record different loops on P1–P4 lanes; they layer and each mutes independently.
- [ ] **K5. Loop lengths** — set different BARS per lane (up to 32); they stay phase-aligned to the master clock.
- [ ] **K6. AUDIO vs MIDI lane** — a MIDI lane re-synths through the current patch; an AUDIO lane records the rendered audio; WAV export writes a valid file.
- [ ] **K7. Scenes** — tap a scene → it arms and **launches on the quantum boundary**; it swaps the drum pattern + looper clips together; the long-press scene menu works; escape hatches cancel a pending launch.
- [ ] **K8. Tempo** — the Tempo knob drives arp/seq/looper/synced-LFOs together; in a DAW it **follows host tempo + transport**.
- [ ] **K9. MIDI clock OUT** — with clock-out on, external gear locks to the synth's tempo (start/stop + 24 ppqn).

## L. Multitimbral & routing

- [ ] **L1. Parts** — P1–P4 each hold their own patch/kit with independent FX/LFO/mixer level+pan.
- [ ] **L2. Edit focus** — tapping a part swaps the panel to it; a kit part dims the synth panels and exposes its EQ.
- [ ] **L3. Isolation** — playing/stealing voices on one part never cuts another part's notes; a mono part's last-note logic doesn't leak across parts.
- [ ] **L4. Input routing** — split the Launchkey (keys → one part, pads → another) and a key-range split zone; each surface plays the routed part.
- [ ] **L5. Routing lifecycle** — after a **relaunch**, routing + zones **reset to default** (only the SOUND persists); loading a MULTI is the only thing that recalls a full routing layout.
- [ ] **L6. INPUTS panel** — Live (follow-focus) vs pinned Part 1 behave distinctly.

## M. Input surfaces & MIDI

- [ ] **M1. Launchkey full** — keys, pads, pitch bend, mod wheel (CC1), sustain (CC64) all work.
- [ ] **M2. Hot-plug** — unplug and replug the Launchkey (or MC8) → it auto-reconnects and MIDI keeps working (no dead controller after reconnect).
- [ ] **M3. Device precedence** — a user-learned mapping overrides the device profile, which overrides factory.
- [ ] **M4. QWERTY** — computer-keyboard notes play; **holding Shift** drops the top (letter) row an octave and lifts the number row an octave; releasing Shift mid-note doesn't bend it.
- [ ] **M5. MIDI-learn** — right-click / long-press a control → amber → send a CC → it binds; the mapping **survives a restart**; clearing it works.
- [ ] **M6. Footswitch looper rig** — MIDI-learn `loop_rec`/`loop_play` to MC8 footswitches sending **Toggle CCs** (CC 102–119); tapping records/mutes lanes hands-free (per the README MIDI-mapping section).

## N. Standalone extras & UI

- [ ] **N1. Fullscreen (F11)** and **health overlay (F12)** toggle correctly.
- [ ] **N2. Scope + FFT** — the scope shows the waveform and the FFT the spectrum, live, without hitching the audio.
- [ ] **N3. Numeric entry** — double-tap a control to type an exact value; it parses back onto the parameter.
- [ ] **N4. Help overlay** — opens and is readable.
- [ ] **N5. Session BOUNCE** — the BOUNCE button renders a folder: per-part WAV stems + master WAV + per-part MIDI + manifest.json. **Drag the folder into a DAW (Ableton/Reaper)** → the stems line up at the manifest BPM and the parts sit in time.
- [ ] **N6. MULTI save/load** — save the whole scene as a MULTI and reload it → all parts, FX, mixer, routing, seq/looper/scenes come back.

## O. Robustness (play hard, listen closely)

- [ ] **O1. No stuck notes** — across every generator (arp / seq / chord / looper / scene switch), turning things on/off mid-note never leaves a note hanging.
- [ ] **O2. No clicks** — starting/stopping voices, muting loop lanes, switching scenes, and one-shot samples are all click-free (this is by ear — the automated click-torture can't judge subtle ones).
- [ ] **O3. Output never clips hard** — even a dense chord + all FX + loud patch stays bounded (the safety clipper holds ±1.0); nothing ever goes to full-scale garbage.
- [ ] **O4. Controls never lie** — every readout equals the live value (e.g. an LFO with SYNC off shows the RATE-knob value, not a stale division).
- [ ] **O5. CPU under load** — a full scene (4 parts, kit, seq, loops, FX) holds within budget on the ThinkPad with no xruns at the chosen buffer.
- [ ] **O6. Soak** — leave a busy scene running for a while; no drift, no leak-driven slowdown, no accumulating clicks.
- [ ] **O7. Mono fold-down** — sum L+R to mono (monitor mono, or a mono PA) on the wide patches — unison/WID, chorus, width>1 (allpass-decorrelated), stereo delay/reverb. Nothing **hollows out or cancels**: the fundamental stays solid, the patch just narrows. (Allpass decorrelation and the balance-law pan are specifically meant to survive mono.)
- [ ] **O8. Blind analog A/B** — have someone toggle **ANALOG** (drift) between 0 and a moderate setting without telling you which, on a sustained pad/unison patch. You should reliably hear the difference: drift on = livelier/looser, off = static/sterile. If you can't tell blind, the drift is too subtle (see the tuning sheet, `kMaxDriftCents`).

## P. Cross-feature workflows (build something real)

Run these end to end — this is where interactions surface.

- [ ] **P1. Full groove** — 808 kit on P4 + step-seq pattern; bass patch on P3; record a bass loop on lane 3; add a lead on P1 and loop it; mute/unmute lanes live. Everything stays in time and isolated.
- [ ] **P2. Verse→chorus** — set up two scenes (different drum pattern + loop clips); switch between them on the quantum boundary while playing over the top; transitions are clean, no stuck notes, loops realign.
- [ ] **P3. Sound design → save → recall** — design a patch (WT + unison + LFO-sync + FX + a LINK route + a learned CC + a TRIM setting), save it under a category, reload it → **everything** recalls, master untouched.
- [ ] **P4. Controller rig** — Launchkey split (keys→lead, pads→kit) + macros on CC21–28 + a footswitch looper CC; perform hands-mostly-free for a few minutes.
- [ ] **P5. Bounce to DAW — Ableton *and* Reaper** — capture the scene with BOUNCE, then import the folder into **both** hosts. In each: the per-part WAV stems drop onto tracks and **line up at the manifest BPM**, the master WAV matches the sum, and the per-part MIDI files import in time. Note any host-specific quirk (sample-rate mismatch, warp/stretch defaults, stem start offset).
- [ ] **P6. Free play** — just make music for a session. Note anything that annoys, surprises, or feels wrong.

## Q. Voice-character tuning verdicts

Focused A/B listening (good monitors/headphones, on the Linux rig). Each is a **verdict** — if it
fails, the fix is a one-line constant in **[the tuning-constants sheet](uat-tuning-constants.md)**,
then re-gate + regenerate goldens. Record the verdict (✓ ships as-is / adjust → new value).

- [ ] **Q1. Drive protocol** — sweep `filter_drive` 0→1 on a saw/bass patch. Verdict: it reads as
      **musical overdrive** (thickens, adds harmonics, roughly level-matched) — not just louder,
      not harsh/fizzy. Tunable: `kMaxDriveGain` (`SVFilter.h` ~275, currently `4.0`) — harsh/loud →
      lower; too subtle → raise. Verdict: ______
- [ ] **Q2. SAT dynamic contrast** — at a fixed moderate `fx_sat`, play **soft then hard**. Verdict:
      soft stays clean and hard bites — the saturation **tracks your dynamics** (no constant fuzz
      floor), with tube-ish (even-harmonic) warmth rather than transistor fizz. Tunables:
      `kThi` / `kTmid` / `kTlo` + `kAsymK` (`StereoWidth.h` ~238–241). Verdict: ______
- [ ] **Q3. Unison character** — a unison lead/pad at moderate DET/WID. Verdict: a **wide, animated
      ensemble** (moving, alive), not thin/static/comb-filtered; detune is lush without sounding
      out of tune. Tunables: `kMaxUnisonCents`, the `unisonPos()` spread curve, `kMaxDriftCents`
      (`SynthVoice.h` ~405/522/548). Verdict: ______

## R. Windows / Surface (second machine)

Run after the Linux pass. Same build, `brave`-free — this is the Windows binary + a touch tablet.

- [ ] **R1. Launch + banner** — the Windows build (standalone) opens; version+hash banner matches HEAD.
- [ ] **R2. Focusrite ASIO** — select the Focusrite ASIO driver; audio plays with no dropouts at the
      chosen buffer; the reported latency is sane; switching buffer sizes works without a crash.
- [ ] **R3. Touch pass** — drive the whole UI by **touch only** (no mouse): knobs (drag), buttons,
      the part rail, seq grid, scene buttons, and **long-press MIDI-learn / scene menu** all respond;
      no control is too small to hit; double-tap numeric entry works by touch.
- [ ] **R4. High-DPI** — at the Surface's display scaling (150–200 %), the UI is crisp (no blur),
      correctly sized (no clipped panels/text), and hit-targets line up with what's drawn. Try a
      scale change while open if practical.
- [ ] **R5. VST3 in a DAW** — load the VST3 in your Windows DAW; it instantiates, makes sound, saves
      + reloads its state with the project, and automation of a param works.
- [ ] **R6. Multi-instance in Ableton** — load **several instances** (e.g. 3–4) on separate tracks.
      Each is independent (own patch/scene/loops), they don't cross-talk or fight for MIDI, and CPU
      + RAM stay sane (note: each instance pre-allocates ~98 MB of loop rings — expected, it's RAM
      not CPU). Play two at once; no xruns beyond the machine's honest limit.
- [ ] **R7. Cross-platform state** — a MULTI (or patch) saved on Linux **loads on Windows** and sounds
      identical (the determinism goldens promise this — confirm by ear on a WT/unison patch).

---

## Defect log

Triage each: **BLOCKER** (fix + re-gate + re-test before tag) or **KNOWN-ISSUE** (ship with a note).

| # | Area / check | Severity | What happened (repro steps) | Build hash | Status (open / fixed@hash / accepted) |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |
| 3 | | | | | |
| 4 | | | | | |
| 5 | | | | | |

---

## Session log & sign-off

| Session | Date | Areas covered | Blockers found | Notes |
|---|---|---|---|---|
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |

**Final verdict (you fill this in — the tag waits on it):**

- ThinkPad validation (#100) complete and acceptable: ☐
- All BLOCKER defects fixed and re-gated (CI green both platforms): ☐
- Known-issues documented in CHANGELOG: ☐
- **I explicitly approve tagging v1.0.0:** ☐  — signed: ____________  date: ______

_The tag (#101) is cut only after this line is signed. Never automatically._
