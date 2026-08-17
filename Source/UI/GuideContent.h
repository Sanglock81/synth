// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// ============================================================================
// Section guide CONTENT — the single source of truth for the in-app section help
// (the `?` menu -> pick a section -> spotlight + numbered markers + side card) AND
// the generated docs/guide.md. NO help strings live in the UI classes.
//
// One Section per panel area (menu order = panel order, top-left to bottom-right).
// Each Entry documents one control: `controlId` is the APVTS parameter id of a
// representative control (used to place the numbered marker on the panel and to let
// the coverage test cross-reference entries against the real param-attached controls;
// repeated rows -- 3 oscillators, 3 LFOs, 4 looper lanes -- are documented once via the
// first). `name` is how it reads on the panel; `what` is one sentence on WHAT it does;
// `how` is one sentence of practical/musical HOW-or-WHEN. controlId "" = no marker (a
// button/affordance that isn't a mod-target, or a custom-drawn surface). Write every
// sentence from the code's real behavior.
//
// `covered` gates the coverage test: a covered section must have a guide entry for every
// param-attached control in its panel (a new control without help fails the gate).
// ============================================================================
namespace guide
{
    struct Entry
    {
        const char* controlId;   // representative APVTS param id (marker anchor + coverage key); "" = no marker
        const char* name;        // as it reads on the panel
        const char* what;        // one sentence: WHAT it does
        const char* how;         // one sentence: HOW / WHEN to use it (musical, practical)
    };

    struct Section
    {
        const char* id;
        const char* title;
        const char* intro;
        bool        covered;
        std::vector<Entry> entries;
    };

    inline const std::vector<Section>& sections()
    {
        static const std::vector<Section> s {
            // ---- Top bar ---------------------------------------------------------------------
            { "topbar", "Top bar",
              "The performance + patch strip. Preset name and its actions on the left, the voicing + "
              "unison + glide controls, the 8 assignable macros, MASTER, and the mode buttons.",
              true,
              {
                { "",          "SAVE / RANDOM / VARY / CLEAR", "Save a user preset; roll a random patch; nudge the current one; reset to a plain sine.",
                  "RANDOM goes anywhere (shaped playable); VARY is a small step from the sound you have." },
                { "poly_mode", "POLY / MONO / LEG", "Voicing for the focused part: polyphonic, monophonic, or legato.",
                  "MONO/LEG for basses + leads; LEG only re-triggers the envelopes on a detached note." },
                { "glide_time","GLIDE",            "Portamento time -- how long pitch takes to slide between notes.",
                  "A little glide in MONO/LEG gives singing lead + 303-style bass slides." },
                { "analog",    "ANALOG",           "Per-voice pitch + pulse-width drift.",
                  "Add a touch for a warmer, less-static, vintage-analog feel (0 = bit-exact)." },
                { "osc_unison","UNI",              "Unison voice count stacked per note (1 = off).",
                  "Raise for a thick supersaw; each added voice is detuned + panned." },
                { "osc_unison_detune","DET",       "Unison detune spread, in cents.",
                  "More spread = wider, more chorused stack; too much detunes out of tune." },
                { "osc_unison_width", "WID",       "Unison stereo width.",
                  "Spread the stacked voices across the stereo field for a big pad." },
                { "patch_trim","TRIM",             "Per-patch output level (baked with the preset).",
                  "Match a patch's loudness to the rest of the bank without touching its sound." },
                { "master_gain","MASTER",          "Master output level (a performance control, not saved with presets).",
                  "Set your overall level; the safety clipper still guarantees the output never exceeds +/-1." },
                { "macro1",    "MACROS (1-8)",     "Eight assignable knobs; each label shows what it drives.",
                  "Assign via the mod matrix (or LINK); mapped to a controller's knobs for hands-on control." },
                { "",          "REC",              "Arm/record the master output to a WAV file (standalone).",
                  "Capture a take of whatever you play; the file lands in your output folder." },
                { "",          "LINK / MOD / OUTPUTS / INPUTS", "Open the routing tools: touch-connect a mod source, the mod overlay, and device I/O.",
                  "LINK then tap a control to route the armed source; INPUTS maps surfaces to parts." },
              } },

            // ---- Parts rail ------------------------------------------------------------------
            { "parts", "Parts rail",
              "The four parts (P1-P4). One is the edit focus -- its sound fills the panels; each part has "
              "its own level + pan and can be a synth or a kit (whose pads are edited in the Kit Editor).",
              true,
              {
                { "",           "PART CELL (P1-P4)", "Tap to focus a part for editing; it shows LIVE (follows your playing) or a pinned role.",
                  "Focus a part to edit its sound; a kit part opens the Kit Editor to edit pads." },
                { "part0_level","LVL",              "That part's level in the final mix.",
                  "Balance the four parts against each other, like a small mixer." },
                { "part0_pan",  "PAN",              "That part's stereo position.",
                  "Spread parts left/right so a layered arrangement stays clear." },
              } },

            // ---- Oscillators -----------------------------------------------------------------
            { "osc", "Oscillators",
              "The three tone sources, mixed (with noise) into the filter. Osc 2 and 3 can also "
              "phase-modulate the oscillator below them (FM) instead of just sounding. Markers show "
              "osc 1 -- osc 2 and 3 have the same controls.",
              true,
              {
                { "osc1_on",     "ON",               "Turns this oscillator on or off.",
                  "Off = skipped entirely (real CPU saved), so kill unused ones." },
                { "osc1_wave",   "WAVE",             "Waveform: saw, square, triangle, sine, or wavetable.",
                  "SIN/TRI are the pure carriers FM needs; tap WT twice for tables." },
                { "osc1_octave", "OCTAVE",           "Coarse pitch, in octaves (-2..+2).",
                  "Drop osc 3 an octave for a sub; spread octaves for a fuller sound." },
                { "osc1_semi",   "SEMI",             "Coarse tune, in semitones (-24..+24).",
                  "Stack an interval vs the others: +7 a fifth, +12 an octave." },
                { "osc1_detune", "DETUNE",           "Fine tune, in cents (+/-100).",
                  "A few cents against another osc thickens the sound (beating)." },
                { "osc1_pw",     "PW / WT POS",      "Square pulse width; in WT mode, the wavetable position.",
                  "Sweep it (or an LFO) for PWM motion / to morph the table." },
                { "osc1_level",  "LEVEL",            "This oscillator's level in the mix.",
                  "Balance the sources; an FM modulator can sit at 0 and still work." },
                { "osc1_phase",  "PHASE (RS/RN/FR)", "Note start phase: Reset, Random, or Free-run.",
                  "Reset is tight; Random/Free add analog + unison spread." },
                { "osc1_fm",     "FM 2>1",           "How hard osc 2 phase-modulates osc 1 (FM).",
                  "Raise on a SIN/TRI/WT carrier for e-piano/bells; osc 2 SEMI = ratio." },
                { "osc2_fm",     "FM 3>2",           "How hard osc 3 phase-modulates osc 2.",
                  "Chains a 2nd FM stage (3->2->1) for richer, metallic tones." },
                { "noise_level", "NOISE",            "White-noise source level (the 4th source).",
                  "A touch adds breath/attack; more gives wind and percussion." },
                { "noise_x",     "NOISE FIELD",      "A drag pad for the noise's character: across the bottom edge are the classic "
                                                     "noise colours (brown, pink, white, bright); dragging upward focuses the noise "
                                                     "into a band around that point, and at the top the band is tight enough to ring "
                                                     "into pitched noise.",
                  "Bottom-left for rumble and wind, bottom-right for air and hiss; lift it for a "
                  "whistle or a resonant sweep. Double-click returns it to white." },
                { "noise_y",     "FOC",              "The field's focus axis on its own, so it can be modulated by itself.",
                  "Route an LFO or envelope here to open and close the noise band; at the top it rings." },
              } },

            // ---- Filter ----------------------------------------------------------------------
            { "filter", "Filter",
              "One resonant filter shapes the mixed oscillators' brightness -- the main tone-shaping "
              "stage, swept by the mod envelope, keytracking, and velocity.",
              true,
              {
                { "filter_type",    "LP / HP / BP / NOTCH", "Filter shape: low-pass, high-pass, band-pass, or notch.",
                  "LP is the classic 'darker' filter; HP thins the low end; BP/Notch for vowel/phaser colors." },
                { "filter_cutoff",  "CUTOFF",          "The corner frequency the filter works around.",
                  "The big brightness control; sweep it (envelope/LFO) for the signature filter motion." },
                { "filter_reso",    "RESO",            "Resonance -- emphasis right at the cutoff.",
                  "Adds a vocal peak; the very top of the range self-oscillates into a sine tone." },
                { "filter_drive",   "DRIVE",           "Saturation inside the filter loop.",
                  "Push for analog grit + a fatter, compressed tone as you drive it harder." },
                { "filter_env_amt", "ENV AMT",         "How much the mod envelope moves the cutoff (bipolar).",
                  "Positive for a bright attack that decays down; negative to open up over time." },
                { "filter_keytrack","KEYTRK",          "How much the note's pitch raises the cutoff.",
                  "Full keytrack keeps timbre consistent up the keyboard (like an acoustic instrument)." },
                { "vel_to_cutoff",  "VEL>CUT",         "How much velocity opens the filter.",
                  "Set it so harder hits sound brighter -- expressive, dynamic playing." },
              } },

            // ---- Envelopes -------------------------------------------------------------------
            { "env", "Envelopes",
              "Two ADSR envelopes. The AMP tab shapes loudness over the note; the MOD tab is a second "
              "envelope that drives the filter and pitch. The four faders show whichever tab is selected.",
              true,
              {
                { "",            "AMP / MOD",  "Switches the four faders between the amp envelope and the mod envelope.",
                  "Shape loudness on AMP; shape the filter sweep (and pitch) on MOD." },
                { "amp_attack",  "A (Attack)", "Time to rise from silence to full at note-on.",
                  "Short for plucks/keys; longer for pads that swell in." },
                { "amp_decay",   "D (Decay)",  "Time to fall from the peak down to the sustain level.",
                  "Sets the initial 'thump' or 'ping' before the note settles." },
                { "amp_sustain", "S (Sustain)","The held level while a note stays down.",
                  "High for organs/pads; low (with decay) for percussive keys + plucks." },
                { "amp_release", "R (Release)","Time to fade out after the note is released.",
                  "Short for tight staccato; long for tails that ring on." },
                { "fltenv_to_pitch","E>PCH",   "How much the mod envelope bends pitch (semitones).",
                  "A short punch of pitch = drum thump / laser zap on the note attack." },
                { "vel_to_amp",  "VEL",        "How much velocity controls loudness.",
                  "Raise for dynamic keys; lower for a steady, even level regardless of touch." },
              } },

            // ---- LFOs ------------------------------------------------------------------------
            { "lfo", "LFOs",
              "Three low-frequency oscillators per part for cyclic motion. Each picks a destination + "
              "shape and runs free (RATE in Hz) or locked to the tempo (SYNC -> DIV). Each LFO has an "
              "identity colour (LFO 1 amber, 2 teal, 3 violet) shown on every control it modulates.",
              true,
              {
                { "lfo_dest",  "DEST",  "Where this LFO's motion goes (off / pitch / cutoff / PW / ...).",
                  "LONG-PRESS to LINK this LFO: it holds still + its existing links glow its colour; tap any "
                  "knob to add a route (tap again to remove), or slide a knob to set the sweep bounds (its "
                  "low..high). Long-press again to commit; a short-press or Esc cancels. DEPTH scales the links." },
                { "lfo_rate",  "RATE",  "Free-run speed in Hz (when SYNC is off).",
                  "Slow for evolving pads; fast (audio-rate) toward a buzzy, ring-mod-ish tone." },
                { "lfo_div",   "DIV",   "The tempo division when SYNC is on (replaces RATE).",
                  "Lock the wobble to the beat: 1/8, 1/16, triplet or dotted." },
                { "lfo_depth", "DEPTH", "How far the LFO moves its destination.",
                  "Subtle for movement, extreme for special effects; 0 = off." },
                { "lfo_shape", "SHAPE", "Waveform: triangle, sine, square, or sample-and-hold.",
                  "S&H gives random stepped motion; square is an on/off gate; tri/sine are smooth." },
                { "lfo_sync",  "SYNC",  "Locks this LFO to the tempo (RATE morphs into DIV).",
                  "On engages at the next bar (click-safe) + phase-locks to the beat; off returns to RATE." },
              } },

            // ---- FX chain --------------------------------------------------------------------
            { "fx", "FX chain",
              "The per-part effects, applied in order (drag a name bar to reorder, tap it to toggle): "
              "saturation + width, chorus, ping-pong delay, then reverb.",
              true,
              {
                { "fx_sat",         "SAT",     "Tube-style saturation before the widener.",
                  "Adds warmth + harmonics; it lowers a soft threshold, so harder input distorts more." },
                { "stereo_width",   "WIDTH",   "Stereo width (mid/side); past unity widens beyond the speakers.",
                  "Narrow to mono-check; widen for size, but keep bass centered." },
                { "chorus_rate",    "CHORUS RATE",  "Speed of the chorus modulation.",
                  "Slow for a gentle drift; faster for a vibrato-ish shimmer." },
                { "chorus_depth",   "CHORUS DEPTH", "How deep the chorus detuning swings.",
                  "More depth = more obvious, seasick chorus." },
                { "chorus_mix",     "CHORUS MIX",   "Dry/wet blend of the chorus.",
                  "A little thickens; full is a lush, washed sound." },
                { "chorus_voices",  "VOICES 1|2",   "One or two chorus taps.",
                  "2 adds a second decorrelated voice -- thicker + wider." },
                { "delay_time",     "DELAY TIME", "Echo time (follows tempo where synced).",
                  "Short slap for space; longer for rhythmic repeats." },
                { "delay_feedback", "DELAY FBK",  "How many times the echo repeats.",
                  "Low for one or two taps; high for long, self-feeding trails." },
                { "delay_spread",   "DELAY PNG",  "Ping-pong spread across the stereo field.",
                  "Bounces the echoes left/right for width." },
                { "delay_mix",      "DELAY MIX",  "Dry/wet blend of the delay.",
                  "Set the echo's presence behind the dry signal." },
                { "reverb_size",    "REVERB SIZE", "The space's size / tail length.",
                  "Small = room; large = hall/cathedral." },
                { "reverb_damp",    "REVERB DAMP", "How fast the high frequencies fade in the tail.",
                  "More damping = darker, warmer tail; less = brighter + glassy." },
                { "reverb_width",   "REVERB WIDTH","Stereo width of the reverb.",
                  "Wide for an enveloping space; narrow to keep it focused." },
                { "reverb_mix",     "REVERB MIX",  "Dry/wet blend of the reverb.",
                  "A touch for depth; more to float the sound in the space." },
                { "reverb_motion",  "MOTION",     "Slow modulation of the reverb tail.",
                  "Smears the metallic ring so pads swim (0 = static)." },
              } },

            // ---- EQ (custom drag surface -> no param-attached components; documented by concept) --
            { "eq", "EQ",
              "A 5-band parametric EQ at the end of the focused part's chain -- the mixing-desk tone "
              "stage. It follows edit focus. Each band is one vertical slider you drag to shape it.",
              true,
              {
                { "peq_on",     "EQ ON",     "Enables the EQ for the focused part (tap the header).",
                  "Off is a true bypass; turn on to sculpt the part's tone." },
                { "peq_b1_gain","BAND GAIN", "Drag a band slider up/down to boost or cut it.",
                  "Cut to clean up mud/harshness; boost gently to add air or body." },
                { "peq_b1_freq","BAND FREQ", "Drag a band sideways to move its center frequency.",
                  "Sweep to find the frequency you want to shape, then set the gain." },
                { "peq_b1_q",   "BAND Q",    "Each band's width (how wide a range it affects).",
                  "Narrow to notch a single problem frequency; wide for broad tone shaping." },
                { "peq_b1_on",  "BAND ON/OFF", "Toggles a single band (the dot on the band).",
                  "Disable a band to A/B its effect without losing the setting." },
              } },

            // ---- Chord row -------------------------------------------------------------------
            { "chord", "Chord row",
              "One-finger chords: with CHORD on, a single played note sounds a whole diatonic chord in "
              "the chosen key. Modifier keys temporarily change the chord quality as you play.",
              true,
              {
                { "chord_enabled", "CHORD",  "Turns the one-finger chord engine on or off.",
                  "On: every note becomes a chord; off: normal single notes." },
                { "chord_root",  "ROOT",     "The key's root note.",
                  "Set the key you're playing in so the chords stay diatonic." },
                { "chord_scale", "QUALITY",  "The scale/quality the chords are built from.",
                  "Major/minor etc. sets the overall color of the auto-chords." },
                { "",            "MODIFIERS (C V B N M , .)", "Hold a QWERTY key to force Maj/Min/Sus4/Sus2/Dim/Dom7/7th on the played chord.",
                  "Grab a modifier for a passing chord without changing the key; held chords re-voice live." },
              } },

            // ---- Arpeggiator -----------------------------------------------------------------
            { "arp", "Arpeggiator",
              "Turns held notes into a rhythmic pattern, clocked to the tempo. It reorders whatever you "
              "hold; a rest step is skipped. Runs independently of the step sequencer.",
              true,
              {
                { "arp_on",     "ARP",   "Turns the arpeggiator on or off.",
                  "On: held notes play as a pattern; off: they sound as a chord." },
                { "arp_mode",   "MODE",  "Note order: up, down, up-down, random, or as-played.",
                  "UP is classic; UP-DN bounces; RAND for unpredictable motion." },
                { "arp_octaves","OCT",   "How many octaves the pattern spans.",
                  "Raise for a wider, cascading arp." },
                { "arp_gate",   "GATE",  "How long each step sounds (staccato vs legato).",
                  "Short + punchy, or long so steps overlap into a run." },
                { "arp_swing",  "SWING", "Delays the off-beats for a shuffled feel.",
                  "A little swing loosens a stiff, straight pattern." },
                { "tempo",      "TEMPO", "The internal tempo (BPM); in a DAW it follows the host.",
                  "Drives the arp, sequencer, looper, and any synced LFOs." },
                { "arp_hold",   "HOLD",  "Latches the held notes so the arp keeps running hands-free.",
                  "Hold a chord, let go, and tweak the sound while it plays." },
                { "",           "STEP VELOCITY", "The per-step boxes set each step's accent.",
                  "Draw an accent pattern so the arp grooves instead of playing flat." },
              } },

            // ---- Sequencer -------------------------------------------------------------------
            { "seq", "Sequencer",
              "An 8-row step grid that drives the targeted part (typically a drum kit): tap cells to "
              "place hits. Locked to the transport, with per-step velocity, gate, and row mutes.",
              true,
              {
                { "seq_on",     "SEQ",    "Turns the step sequencer on or off.",
                  "On: the grid plays the target part on the beat." },
                { "seq_target", "P1-P4",  "Which part the grid plays.",
                  "Point it at your drum-kit part; other parts stay free for keys/bass." },
                { "seq_gate",   "GATE",   "How long each step's note sounds.",
                  "Short for tight drums; longer for sustained/tonal steps." },
                { "",           "STEP CELLS + row M", "Tap a cell to place/clear a hit; drag a step's velocity; M mutes a row.",
                  "Build a beat one row (drum) at a time; mute rows to try variations." },
              } },

            // ---- Looper & Scenes -------------------------------------------------------------
            { "looper", "Looper & Scenes",
              "Four per-part loop lanes (MIDI or audio) locked to the transport, plus scene slots that "
              "recall a whole arrangement on a bar boundary. Markers show lane 1.",
              true,
              {
                { "loop_rec",   "R (Record)", "Arms + records this lane; capture starts at the next bar.",
                  "Play a part in; it loops in time and layers with the others." },
                { "loop_play",  "P (Play)",   "Plays or stops this lane's loop.",
                  "Mute/unmute layers live to build or strip back the arrangement." },
                { "loop_mode",  "MIDI / AU",  "Whether the lane records MIDI (notes) or AUDIO.",
                  "MIDI stays editable + re-voiceable; AUDIO captures the exact sound (or an input)." },
                { "loop_bars",  "BARS",       "This lane's loop length in bars.",
                  "Short for a groove; long for an evolving progression." },
                { "loop_quant", "Q (Quantize)", "Snaps recorded notes to a grid.",
                  "On tightens a loose take to the beat; off keeps your exact timing." },
                { "scene_quant","SCENE QUANT", "The boundary scene launches wait for.",
                  "Set to a bar/2-bar so scene changes land musically, not mid-phrase." },
                { "",           "SCENE SLOTS (1-8)", "Tap to launch a saved arrangement (quantized); long-press to copy/clear.",
                  "Snapshot loop + seq states into scenes and switch sections live." },
                { "",           "MIDI / WAV export", "Bounce the recorded loops out to MIDI or WAV files.",
                  "Take your jam into a DAW, or render stems." },
              } },

            // ---- Scope / Spectrum + F12 (display-only; no param-attached controls) ------------
            { "scope", "Scope / Spectrum + F12",
              "The output displays (not controls). The scope shows the waveform; the spectrum shows its "
              "frequency content. F12 opens an audio-health overlay for performance + safety.",
              true,
              {
                { "", "SCOPE",   "The live output waveform.",
                  "Watch the shape + level; a flat line means silence, a clipped-looking top means it's hot." },
                { "", "SPECTRUM (FFT)", "The output's frequency content, low to high.",
                  "See where a patch's energy sits -- bass weight, presence, harshness." },
                { "", "F12 overlay", "Render time, active voices, xruns, and clipping.",
                  "Open it if you hear glitches: overruns = raise the buffer; clip = lower the level." },
              } },
        };
        return s;
    }
}
