# Section guide

Generated from `Source/UI/GuideContent.h` -- the in-app `?` -> section reference (spotlight + numbered markers + card). Do not edit by hand; edit the content header + rerun the guide test.

## Top bar

The performance + patch strip. Preset name and its actions on the left, the voicing + unison + glide controls, the 8 assignable macros, MASTER, and the mode buttons.

1. **SAVE / RANDOM / VARY / CLEAR** - Save a user preset; roll a random patch; nudge the current one; reset to a plain sine. _RANDOM goes anywhere (shaped playable); VARY is a small step from the sound you have._
2. **POLY / MONO / LEG** - Voicing for the focused part: polyphonic, monophonic, or legato. _MONO/LEG for basses + leads; LEG only re-triggers the envelopes on a detached note._
3. **GLIDE** - Portamento time -- how long pitch takes to slide between notes. _A little glide in MONO/LEG gives singing lead + 303-style bass slides._
4. **ANALOG** - Per-voice pitch + pulse-width drift. _Add a touch for a warmer, less-static, vintage-analog feel (0 = bit-exact)._
5. **UNI** - Unison voice count stacked per note (1 = off). _Raise for a thick supersaw; each added voice is detuned + panned._
6. **DET** - Unison detune spread, in cents. _More spread = wider, more chorused stack; too much detunes out of tune._
7. **WID** - Unison stereo width. _Spread the stacked voices across the stereo field for a big pad._
8. **TRIM** - Per-patch output level (baked with the preset). _Match a patch's loudness to the rest of the bank without touching its sound._
9. **MASTER** - Master output level (a performance control, not saved with presets). _Set your overall level; the safety clipper still guarantees the output never exceeds +/-1._
10. **MACROS (1-8)** - Eight assignable knobs; each label shows what it drives. _Assign via the mod matrix (or LINK); mapped to a controller's knobs for hands-on control._
11. **REC** - Arm/record the master output to a WAV file (standalone). _Capture a take of whatever you play; the file lands in your output folder._
12. **LINK / MOD / OUTPUTS / INPUTS** - Open the routing tools: touch-connect a mod source, the mod overlay, and device I/O. _LINK then tap a control to route the armed source; INPUTS maps surfaces to parts._

## Parts rail

The four parts (P1-P4). One is the edit focus -- its sound fills the panels; each part has its own level + pan and can be a synth or a kit (whose pads are edited in the Kit Editor).

1. **PART CELL (P1-P4)** - Tap to focus a part for editing; it shows LIVE (follows your playing) or a pinned role. _Focus a part to edit its sound; a kit part opens the Kit Editor to edit pads._
2. **LVL** - That part's level in the final mix. _Balance the four parts against each other, like a small mixer._
3. **PAN** - That part's stereo position. _Spread parts left/right so a layered arrangement stays clear._

## Oscillators

The three tone sources, mixed (with noise) into the filter. Osc 2 and 3 can also phase-modulate the oscillator below them (FM) instead of just sounding. Markers show osc 1 -- osc 2 and 3 have the same controls.

1. **ON** - Turns this oscillator on or off. _Off = skipped entirely (real CPU saved), so kill unused ones._
2. **WAVE** - Waveform: saw, square, triangle, sine, or wavetable. _SIN/TRI are the pure carriers FM needs; tap WT twice for tables._
3. **OCTAVE** - Coarse pitch, in octaves (-2..+2). _Drop osc 3 an octave for a sub; spread octaves for a fuller sound._
4. **SEMI** - Coarse tune, in semitones (-24..+24). _Stack an interval vs the others: +7 a fifth, +12 an octave._
5. **DETUNE** - Fine tune, in cents (+/-100). _A few cents against another osc thickens the sound (beating)._
6. **PW / WT POS** - Square pulse width; in WT mode, the wavetable position. _Sweep it (or an LFO) for PWM motion / to morph the table._
7. **LEVEL** - This oscillator's level in the mix. _Balance the sources; an FM modulator can sit at 0 and still work._
8. **PHASE (RS/RN/FR)** - Note start phase: Reset, Random, or Free-run. _Reset is tight; Random/Free add analog + unison spread._
9. **FM 2>1** - How hard osc 2 phase-modulates osc 1 (FM). _Raise on a SIN/TRI/WT carrier for e-piano/bells; osc 2 SEMI = ratio._
10. **FM 3>2** - How hard osc 3 phase-modulates osc 2. _Chains a 2nd FM stage (3->2->1) for richer, metallic tones._
11. **NOISE** - White-noise source level (the 4th source). _A touch adds breath/attack; more gives wind and percussion._
12. **NOISE FIELD** - A drag pad for the noise's character: across the bottom edge are the classic noise colours (brown, pink, white, bright); dragging upward focuses the noise into a band around that point, and at the top the band is tight enough to ring into pitched noise. _Bottom-left for rumble and wind, bottom-right for air and hiss; lift it for a whistle or a resonant sweep. Double-click returns it to white._
13. **FOC** - The field's focus axis on its own, so it can be modulated by itself. _Route an LFO or envelope here to open and close the noise band; at the top it rings._

## Filter

One resonant filter shapes the mixed oscillators' brightness -- the main tone-shaping stage, swept by the mod envelope, keytracking, and velocity.

1. **LP / HP / BP / NOTCH** - Filter shape: low-pass, high-pass, band-pass, or notch. _LP is the classic 'darker' filter; HP thins the low end; BP/Notch for vowel/phaser colors._
2. **CUTOFF** - The corner frequency the filter works around. _The big brightness control; sweep it (envelope/LFO) for the signature filter motion._
3. **RESO** - Resonance -- emphasis right at the cutoff. _Adds a vocal peak; the very top of the range self-oscillates into a sine tone._
4. **DRIVE** - Saturation inside the filter loop. _Push for analog grit + a fatter, compressed tone as you drive it harder._
5. **ENV AMT** - How much the mod envelope moves the cutoff (bipolar). _Positive for a bright attack that decays down; negative to open up over time._
6. **KEYTRK** - How much the note's pitch raises the cutoff. _Full keytrack keeps timbre consistent up the keyboard (like an acoustic instrument)._
7. **VEL>CUT** - How much velocity opens the filter. _Set it so harder hits sound brighter -- expressive, dynamic playing._

## Envelopes

Two ADSR envelopes. The AMP tab shapes loudness over the note; the MOD tab is a second envelope that drives the filter and pitch. The four faders show whichever tab is selected.

1. **AMP / MOD** - Switches the four faders between the amp envelope and the mod envelope. _Shape loudness on AMP; shape the filter sweep (and pitch) on MOD._
2. **A (Attack)** - Time to rise from silence to full at note-on. _Short for plucks/keys; longer for pads that swell in._
3. **D (Decay)** - Time to fall from the peak down to the sustain level. _Sets the initial 'thump' or 'ping' before the note settles._
4. **S (Sustain)** - The held level while a note stays down. _High for organs/pads; low (with decay) for percussive keys + plucks._
5. **R (Release)** - Time to fade out after the note is released. _Short for tight staccato; long for tails that ring on._
6. **E>PCH** - How much the mod envelope bends pitch (semitones). _A short punch of pitch = drum thump / laser zap on the note attack._
7. **VEL** - How much velocity controls loudness. _Raise for dynamic keys; lower for a steady, even level regardless of touch._

## LFOs

Three low-frequency oscillators per part for cyclic motion. Each picks a destination + shape and runs free (RATE in Hz) or locked to the tempo (SYNC -> DIV). Each LFO has an identity colour (LFO 1 amber, 2 teal, 3 violet) shown on every control it modulates.

1. **DEST** - Where this LFO's motion goes (off / pitch / cutoff / PW / ...). _LONG-PRESS to LINK this LFO: it holds still + its existing links glow its colour; tap any knob to add a route (tap again to remove), or slide a knob to set the sweep bounds (its low..high). Long-press again to commit; a short-press or Esc cancels. DEPTH scales the links._
2. **RATE** - Free-run speed in Hz (when SYNC is off). _Slow for evolving pads; fast (audio-rate) toward a buzzy, ring-mod-ish tone._
3. **DIV** - The tempo division when SYNC is on (replaces RATE). _Lock the wobble to the beat: 1/8, 1/16, triplet or dotted._
4. **DEPTH** - How far the LFO moves its destination. _Subtle for movement, extreme for special effects; 0 = off._
5. **SHAPE** - Waveform: triangle, sine, square, or sample-and-hold. _S&H gives random stepped motion; square is an on/off gate; tri/sine are smooth._
6. **SYNC** - Locks this LFO to the tempo (RATE morphs into DIV). _On engages at the next bar (click-safe) + phase-locks to the beat; off returns to RATE._

## FX chain

The per-part effects, applied in order (drag a name bar to reorder, tap it to toggle): saturation + width, chorus, ping-pong delay, then reverb.

1. **SAT** - Tube-style saturation before the widener. _Adds warmth + harmonics; it lowers a soft threshold, so harder input distorts more._
2. **WIDTH** - Stereo width (mid/side); past unity widens beyond the speakers. _Narrow to mono-check; widen for size, but keep bass centered._
3. **CHORUS RATE** - Speed of the chorus modulation. _Slow for a gentle drift; faster for a vibrato-ish shimmer._
4. **CHORUS DEPTH** - How deep the chorus detuning swings. _More depth = more obvious, seasick chorus._
5. **CHORUS MIX** - Dry/wet blend of the chorus. _A little thickens; full is a lush, washed sound._
6. **VOICES 1|2** - One or two chorus taps. _2 adds a second decorrelated voice -- thicker + wider._
7. **DELAY TIME** - Echo time (follows tempo where synced). _Short slap for space; longer for rhythmic repeats._
8. **DELAY FBK** - How many times the echo repeats. _Low for one or two taps; high for long, self-feeding trails._
9. **DELAY PNG** - Ping-pong spread across the stereo field. _Bounces the echoes left/right for width._
10. **DELAY MIX** - Dry/wet blend of the delay. _Set the echo's presence behind the dry signal._
11. **REVERB SIZE** - The space's size / tail length. _Small = room; large = hall/cathedral._
12. **REVERB DAMP** - How fast the high frequencies fade in the tail. _More damping = darker, warmer tail; less = brighter + glassy._
13. **REVERB WIDTH** - Stereo width of the reverb. _Wide for an enveloping space; narrow to keep it focused._
14. **REVERB MIX** - Dry/wet blend of the reverb. _A touch for depth; more to float the sound in the space._
15. **MOTION** - Slow modulation of the reverb tail. _Smears the metallic ring so pads swim (0 = static)._

## EQ

A 5-band parametric EQ at the end of the focused part's chain -- the mixing-desk tone stage. It follows edit focus. Each band is one vertical slider you drag to shape it.

1. **EQ ON** - Enables the EQ for the focused part (tap the header). _Off is a true bypass; turn on to sculpt the part's tone._
2. **BAND GAIN** - Drag a band slider up/down to boost or cut it. _Cut to clean up mud/harshness; boost gently to add air or body._
3. **BAND FREQ** - Drag a band sideways to move its center frequency. _Sweep to find the frequency you want to shape, then set the gain._
4. **BAND Q** - Each band's width (how wide a range it affects). _Narrow to notch a single problem frequency; wide for broad tone shaping._
5. **BAND ON/OFF** - Toggles a single band (the dot on the band). _Disable a band to A/B its effect without losing the setting._

## Chord row

One-finger chords: with CHORD on, a single played note sounds a whole diatonic chord in the chosen key. Modifier keys temporarily change the chord quality as you play.

1. **CHORD** - Turns the one-finger chord engine on or off. _On: every note becomes a chord; off: normal single notes._
2. **ROOT** - The key's root note. _Set the key you're playing in so the chords stay diatonic._
3. **QUALITY** - The scale/quality the chords are built from. _Major/minor etc. sets the overall color of the auto-chords._
4. **MODIFIERS (C V B N M , .)** - Hold a QWERTY key to force Maj/Min/Sus4/Sus2/Dim/Dom7/7th on the played chord. _Grab a modifier for a passing chord without changing the key; held chords re-voice live._

## Arpeggiator

Turns held notes into a rhythmic pattern, clocked to the tempo. It reorders whatever you hold; a rest step is skipped. Runs independently of the step sequencer.

1. **ARP** - Turns the arpeggiator on or off. _On: held notes play as a pattern; off: they sound as a chord._
2. **MODE** - Note order: up, down, up-down, random, or as-played. _UP is classic; UP-DN bounces; RAND for unpredictable motion._
3. **OCT** - How many octaves the pattern spans. _Raise for a wider, cascading arp._
4. **GATE** - How long each step sounds (staccato vs legato). _Short + punchy, or long so steps overlap into a run._
5. **SWING** - Delays the off-beats for a shuffled feel. _A little swing loosens a stiff, straight pattern._
6. **TEMPO** - The internal tempo (BPM); in a DAW it follows the host. _Drives the arp, sequencer, looper, and any synced LFOs._
7. **HOLD** - Latches the held notes so the arp keeps running hands-free. _Hold a chord, let go, and tweak the sound while it plays._
8. **STEP VELOCITY** - The per-step boxes set each step's accent. _Draw an accent pattern so the arp grooves instead of playing flat._

## Sequencer

An 8-row step grid that drives the targeted part (typically a drum kit): tap cells to place hits. Locked to the transport, with per-step velocity, gate, and row mutes.

1. **SEQ** - Turns the step sequencer on or off. _On: the grid plays the target part on the beat._
2. **P1-P4** - Which part the grid plays. _Point it at your drum-kit part; other parts stay free for keys/bass._
3. **GATE** - How long each step's note sounds. _Short for tight drums; longer for sustained/tonal steps._
4. **STEP CELLS + row M** - Tap a cell to place/clear a hit; drag a step's velocity; M mutes a row. _Build a beat one row (drum) at a time; mute rows to try variations._

## Looper & Scenes

Four per-part loop lanes (MIDI or audio) locked to the transport, plus scene slots that recall a whole arrangement on a bar boundary. Markers show lane 1.

1. **R (Record)** - Arms + records this lane; capture starts at the next bar. _Play a part in; it loops in time and layers with the others._
2. **P (Play)** - Plays or stops this lane's loop. _Mute/unmute layers live to build or strip back the arrangement._
3. **MIDI / AU** - Whether the lane records MIDI (notes) or AUDIO. _MIDI stays editable + re-voiceable; AUDIO captures the exact sound (or an input)._
4. **BARS** - This lane's loop length in bars. _Short for a groove; long for an evolving progression._
5. **Q (Quantize)** - Snaps recorded notes to a grid. _On tightens a loose take to the beat; off keeps your exact timing._
6. **SCENE QUANT** - The boundary scene launches wait for. _Set to a bar/2-bar so scene changes land musically, not mid-phrase._
7. **SCENE SLOTS (1-8)** - Tap to launch a saved arrangement (quantized); long-press to copy/clear. _Snapshot loop + seq states into scenes and switch sections live._
8. **MIDI / WAV export** - Bounce the recorded loops out to MIDI or WAV files. _Take your jam into a DAW, or render stems._

## Scope / Spectrum + F12

The output displays (not controls). The scope shows the waveform; the spectrum shows its frequency content. F12 opens an audio-health overlay for performance + safety.

1. **SCOPE** - The live output waveform. _Watch the shape + level; a flat line means silence, a clipped-looking top means it's hot._
2. **SPECTRUM (FFT)** - The output's frequency content, low to high. _See where a patch's energy sits -- bass weight, presence, harshness._
3. **F12 overlay** - Render time, active voices, xruns, and clipping. _Open it if you hear glitches: overruns = raise the buffer; clip = lower the level._
