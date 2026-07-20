#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <utility>

// ============================================================================
// Single source of truth for every synth parameter.
//
// Design notes:
//  * Every parameter lives in the APVTS. That gives us, for free:
//      - thread-safe atomic reads from the audio thread
//      - DAW automation (Ableton) of every knob when running as VST3
//      - state save/load (presets) via the ValueTree
//      - auto-generated GUI via GenericAudioProcessorEditor
//  * Frequency-ish parameters use a log/skewed NormalisableRange so that
//    knobs feel musical (half the knob travel = one perceptual "half").
//  * IDs are namespaced strings; keep them stable forever once presets exist.
// ============================================================================

namespace ParamID
{
    // Oscillator 1
    inline constexpr auto osc1Wave    = "osc1_wave";     // saw / square / tri / sine
    inline constexpr auto osc1Octave  = "osc1_octave";   // -2..+2
    inline constexpr auto osc1Detune  = "osc1_detune";   // cents
    inline constexpr auto osc1PW      = "osc1_pw";       // pulse width (square only)

    // Oscillator 2
    inline constexpr auto osc2Wave    = "osc2_wave";
    inline constexpr auto osc2Octave  = "osc2_octave";
    inline constexpr auto osc2Detune  = "osc2_detune";
    inline constexpr auto osc2PW      = "osc2_pw";

    // Oscillator 3 (6A)
    inline constexpr auto osc3Wave    = "osc3_wave";
    inline constexpr auto osc3Octave  = "osc3_octave";
    inline constexpr auto osc3Detune  = "osc3_detune";
    inline constexpr auto osc3PW      = "osc3_pw";

    // Mixer. osc_mix is FROZEN (legacy crossfade) — kept registered for state
    // compatibility; the engine now uses independent per-source levels + kill
    // switches. Old patches migrate osc_mix -> osc1/2 levels on load.
    inline constexpr auto oscMix      = "osc_mix";       // legacy: 0 = osc1 only, 1 = osc2 only
    inline constexpr auto noiseLevel  = "noise_level";
    inline constexpr auto osc1Level   = "osc1_level";    // 0..1
    inline constexpr auto osc2Level   = "osc2_level";
    inline constexpr auto osc3Level   = "osc3_level";
    inline constexpr auto osc1On      = "osc1_on";       // kill switch (bool)
    inline constexpr auto osc2On      = "osc2_on";
    inline constexpr auto osc3On      = "osc3_on";

    // Velocity routing (6A)
    inline constexpr auto velToAmp    = "vel_to_amp";    // 0..1
    inline constexpr auto velToCutoff = "vel_to_cutoff"; // 0..1 (max ~+3 oct)

    // Filter (state-variable, multimode)
    inline constexpr auto filterType    = "filter_type";   // LP / HP / BP / Notch
    inline constexpr auto filterCutoff  = "filter_cutoff"; // Hz, log skew
    inline constexpr auto filterReso    = "filter_reso";
    inline constexpr auto filterEnvAmt  = "filter_env_amt";// bipolar, semitone-ish sweep
    inline constexpr auto filterKeytrack= "filter_keytrack";

    // Envelopes
    inline constexpr auto ampAttack   = "amp_attack";
    inline constexpr auto ampDecay    = "amp_decay";
    inline constexpr auto ampSustain  = "amp_sustain";
    inline constexpr auto ampRelease  = "amp_release";

    inline constexpr auto fltAttack   = "flt_attack";
    inline constexpr auto fltDecay    = "flt_decay";
    inline constexpr auto fltSustain  = "flt_sustain";
    inline constexpr auto fltRelease  = "flt_release";
    inline constexpr auto fltEnvToPitch = "fltenv_to_pitch";  // mod env -> pitch, semitones

    // LFO 1 (lfo_* IDs kept for compatibility). Sub-phase 2: three per-part LFOs
    // (lfo2_*, lfo3_* added); each still routes to a single selectable destination.
    inline constexpr auto lfoRate     = "lfo_rate";
    inline constexpr auto lfoDepth    = "lfo_depth";
    inline constexpr auto lfoShape    = "lfo_shape";     // tri / sine / square / s&h
    inline constexpr auto lfoDest     = "lfo_dest";      // off / pitch / cutoff / pw
    inline constexpr auto lfo2Rate    = "lfo2_rate";
    inline constexpr auto lfo2Depth   = "lfo2_depth";
    inline constexpr auto lfo2Shape   = "lfo2_shape";
    inline constexpr auto lfo2Dest    = "lfo2_dest";
    inline constexpr auto lfo3Rate    = "lfo3_rate";
    inline constexpr auto lfo3Depth   = "lfo3_depth";
    inline constexpr auto lfo3Shape   = "lfo3_shape";
    inline constexpr auto lfo3Dest    = "lfo3_dest";
    // J1: per-LFO tempo sync. sync off = free-running Hz (rate); sync on = the div note-division.
    inline constexpr auto lfoSync     = "lfo_sync";   inline constexpr auto lfoDiv  = "lfo_div";
    inline constexpr auto lfo2Sync    = "lfo2_sync";  inline constexpr auto lfo2Div = "lfo2_div";
    inline constexpr auto lfo3Sync    = "lfo3_sync";  inline constexpr auto lfo3Div = "lfo3_div";

    // Global
    inline constexpr auto glideTime   = "glide_time";
    inline constexpr auto masterGain  = "master_gain";
    inline constexpr auto polyMode    = "poly_mode";     // poly / mono / legato

    // Part mixer (Sub-phase 2). Per-part level (0..2, unity 1.0) + pan (-1..+1, centre 0).
    // Defaults keep the master sum bit-identical. IDs part0_..part3_.
    inline constexpr auto part0Level  = "part0_level";
    inline constexpr auto part0Pan    = "part0_pan";
    inline constexpr auto part1Level  = "part1_level";
    inline constexpr auto part1Pan    = "part1_pan";
    inline constexpr auto part2Level  = "part2_level";
    inline constexpr auto part2Pan    = "part2_pan";
    inline constexpr auto part3Level  = "part3_level";
    inline constexpr auto part3Pan    = "part3_pan";

    // Chord engine (7B). Diatonic one-finger chords; momentary modifiers are NOT
    // params (they're momentary QWERTY/CC/note sources — see ModifierLearnManager).
    inline constexpr auto chordEnabled = "chord_enabled";
    inline constexpr auto chordRoot    = "chord_root";   // C..B (0..11)
    inline constexpr auto chordScale   = "chord_scale";  // Major / Natural Minor

    // FX (6B). Global reorderable stereo chain: chorus / delay / reverb / width.
    // Each block has a bool enable + rotary params. The chain ORDER is not an
    // automatable parameter (a permutation, not a value) — it lives as the
    // `fx_order` STATE-TREE property (see PluginProcessor::setFxOrder), so it
    // saves/loads with presets but never appears as a host-automatable knob.
    inline constexpr auto fxChorusOn    = "fx_chorus_on";
    inline constexpr auto chorusRate    = "chorus_rate";
    inline constexpr auto chorusDepth   = "chorus_depth";
    inline constexpr auto chorusMix     = "chorus_mix";
    inline constexpr auto fxDelayOn     = "fx_delay_on";
    inline constexpr auto delayTime     = "delay_time";      // ms
    inline constexpr auto delayFeedback = "delay_feedback";
    inline constexpr auto delayMix      = "delay_mix";
    inline constexpr auto delaySpread   = "delay_spread";    // ping-pong amount
    inline constexpr auto fxReverbOn    = "fx_reverb_on";
    inline constexpr auto reverbSize    = "reverb_size";
    inline constexpr auto reverbDamp    = "reverb_damp";
    inline constexpr auto reverbWidth   = "reverb_width";
    inline constexpr auto reverbMix     = "reverb_mix";
    inline constexpr auto fxWidthOn     = "fx_width_on";
    inline constexpr auto stereoWidth   = "stereo_width";    // 0=mono, 1=normal, 2=wide

    inline constexpr auto fxOrder       = "fx_order";        // state-tree property: "a,b,c,d"

    // Macros (R2 layout). Eight performance knobs (0..1) surfaced in the top bar.
    // They are real, automatable, MIDI-learnable, preset-saved parameters NOW; the
    // mod-matrix that routes a macro to destinations arrives in R3. Appended last so
    // existing IDs/ordering are untouched.
    inline constexpr auto macro1 = "macro1";
    inline constexpr auto macro2 = "macro2";
    inline constexpr auto macro3 = "macro3";
    inline constexpr auto macro4 = "macro4";
    inline constexpr auto macro5 = "macro5";
    inline constexpr auto macro6 = "macro6";
    inline constexpr auto macro7 = "macro7";
    inline constexpr auto macro8 = "macro8";

    // Master parametric EQ (R2): a 4-band shaper at the END of the signal chain
    // (post per-part FX sum, pre master gain). Low shelf, two sweepable bells, high
    // shelf. All gains default 0 dB and eq_on defaults false -> a true bypass, so the
    // master output is bit-identical until the player dials it in. IDs appended last.
    inline constexpr auto eqOn      = "eq_on";
    inline constexpr auto eqLsFreq  = "eq_ls_freq";   inline constexpr auto eqLsGain = "eq_ls_gain";
    inline constexpr auto eqLmFreq  = "eq_lm_freq";   inline constexpr auto eqLmGain = "eq_lm_gain";   inline constexpr auto eqLmQ = "eq_lm_q";
    inline constexpr auto eqHmFreq  = "eq_hm_freq";   inline constexpr auto eqHmGain = "eq_hm_gain";   inline constexpr auto eqHmQ = "eq_hm_q";
    inline constexpr auto eqHsFreq  = "eq_hs_freq";   inline constexpr auto eqHsGain = "eq_hs_gain";

    // Per-part EQ (task #51): a 3-band fully-parametric (bell) EQ block in each part's
    // reorderable FX chain. Per-part (in perPartSoundIds); default flat + off => bypass.
    inline constexpr auto peqOn     = "peq_on";
    inline constexpr auto peqB1Freq = "peq_b1_freq";  inline constexpr auto peqB1Gain = "peq_b1_gain";  inline constexpr auto peqB1Q = "peq_b1_q";
    inline constexpr auto peqB2Freq = "peq_b2_freq";  inline constexpr auto peqB2Gain = "peq_b2_gain";  inline constexpr auto peqB2Q = "peq_b2_q";
    inline constexpr auto peqB3Freq = "peq_b3_freq";  inline constexpr auto peqB3Gain = "peq_b3_gain";  inline constexpr auto peqB3Q = "peq_b3_q";
    // K1: consolidated per-part EQ becomes a fixed end-of-part 4-band shaper. B4 + per-band on/off
    // are appended (ID freeze: add, never rename). The master-EQ eq_* IDs stay registered but inert.
    inline constexpr auto peqB4Freq = "peq_b4_freq";  inline constexpr auto peqB4Gain = "peq_b4_gain";  inline constexpr auto peqB4Q = "peq_b4_q";
    inline constexpr auto peqB5Freq = "peq_b5_freq";  inline constexpr auto peqB5Gain = "peq_b5_gain";  inline constexpr auto peqB5Q = "peq_b5_q";
    inline constexpr auto peqB1On = "peq_b1_on";  inline constexpr auto peqB2On = "peq_b2_on";
    inline constexpr auto peqB3On = "peq_b3_on";  inline constexpr auto peqB4On = "peq_b4_on";
    inline constexpr auto peqB5On = "peq_b5_on";

    // Tempo (R3): internal clock BPM, drives the arpeggiator + looper. (Host-tempo
    // sync in a DAW is future; standalone uses this.)
    inline constexpr auto tempo     = "tempo";

    // Arpeggiator / step sequencer (R3). Default off => note dispatch is unchanged
    // (goldens bit-identical). The 16-step velocity pattern lives in the state tree
    // ("arp_steps"), not as 16 automatable params.
    inline constexpr auto arpOn      = "arp_on";
    inline constexpr auto arpMode     = "arp_mode";      // Up/Down/Up-Down/Random/As-played
    inline constexpr auto arpOctaves  = "arp_octaves";   // 1..4
    inline constexpr auto arpGate     = "arp_gate";      // 0..1 of a step
    inline constexpr auto arpSwing    = "arp_swing";     // 0..0.7
    inline constexpr auto arpLatch    = "arp_latch";
    inline constexpr auto arpHold     = "arp_hold";

    // Per-part MIDI looper (R3). Transport bools + loop length in bars. Default off /
    // 1 bar; the recorded loop content is runtime-only (exported to MIDI, not preset).
    // Looper (task #47): FOUR fixed lanes, lane N == part N, each with its OWN transport.
    // The original loop_rec/play/mode are lane 1 (P1/part 0); _2/_3/_4 are P2/P3/P4.
    // J2: loop_bars is now PER-LANE (loop_bars is lane 1; _2/_3/_4 for P2-P4). The enum was
    // EXTENDED APPEND-ONLY to {1,2,4,8,16,32} so indices 0-2 still mean 1/2/4 bars — old
    // sessions that stored index 0/1/2 restore unchanged. IDs frozen.
    inline constexpr auto loopRec     = "loop_rec";       // lane 1 (P1) REC
    inline constexpr auto loopPlay    = "loop_play";      // lane 1 (P1) PLAY
    inline constexpr auto loopBars    = "loop_bars";      // lane 1 (P1) loop length: 1/2/4/8/16/32 bars
    inline constexpr auto loopMode    = "loop_mode";      // lane 1 (P1) playback lane: MIDI re-synth / AUDIO
    inline constexpr auto loopRec2 = "loop_rec2", loopRec3 = "loop_rec3", loopRec4 = "loop_rec4";
    inline constexpr auto loopPlay2 = "loop_play2", loopPlay3 = "loop_play3", loopPlay4 = "loop_play4";
    inline constexpr auto loopMode2 = "loop_mode2", loopMode3 = "loop_mode3", loopMode4 = "loop_mode4";
    inline constexpr auto loopBars2 = "loop_bars2", loopBars3 = "loop_bars3", loopBars4 = "loop_bars4";   // per-lane length (P2-P4)
    inline constexpr auto sceneQuant = "scene_quant";   // J3: scene launch quantum (1/2/4/8 bar or loop-end)
    inline constexpr auto clockOut   = "clock_out";     // #85: transmit 24-ppq MIDI clock + start/stop
    inline constexpr auto loopQuant = "loop_quant", loopQuant2 = "loop_quant2", loopQuant3 = "loop_quant3", loopQuant4 = "loop_quant4";   // per-lane 1/32 quantize (default on)

    // Step sequencer (R3 Group 2). 8-row drum grid; shares tempo/swing with the arp.
    inline constexpr auto seqOn       = "seq_on";
    inline constexpr auto seqGate     = "seq_gate";       // 0..1 of a step
    inline constexpr auto seqTarget   = "seq_target";     // P1..P4 (the drum target part)
}

// Builds the full parameter layout. Called once in the processor constructor.
inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using P  = juce::AudioParameterFloat;
    using Pc = juce::AudioParameterChoice;
    using Pb = juce::AudioParameterBool;
    namespace ID = ParamID;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    const juce::StringArray waveNames { "Saw", "Square", "Triangle", "Sine" };

    // A perceptually-sane time range: 1 ms .. 10 s with heavy skew toward
    // the short end, because that's where envelope precision matters.
    auto timeRange = juce::NormalisableRange<float>(0.001f, 10.0f, 0.0f, 0.3f);

    // Log-ish cutoff range covering the audible band.
    auto cutoffRange = juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.25f);

    // --- Oscillators -------------------------------------------------------
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::osc1Wave, 1},  "Osc1 Wave", waveNames, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc1Octave, 1},"Osc1 Octave", juce::NormalisableRange<float>(-2.0f, 2.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc1Detune, 1},"Osc1 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 0.0f, juce::AudioParameterFloatAttributes().withLabel ("ct")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc1PW, 1},    "Osc1 PW", juce::NormalisableRange<float>(0.05f, 0.95f), 0.5f));

    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::osc2Wave, 1},  "Osc2 Wave", waveNames, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2Octave, 1},"Osc2 Octave", juce::NormalisableRange<float>(-2.0f, 2.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2Detune, 1},"Osc2 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 7.0f, juce::AudioParameterFloatAttributes().withLabel ("ct")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2PW, 1},    "Osc2 PW", juce::NormalisableRange<float>(0.05f, 0.95f), 0.5f));

    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::osc3Wave, 1},  "Osc3 Wave", waveNames, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc3Octave, 1},"Osc3 Octave", juce::NormalisableRange<float>(-2.0f, 2.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc3Detune, 1},"Osc3 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 0.0f, juce::AudioParameterFloatAttributes().withLabel ("ct")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc3PW, 1},    "Osc3 PW", juce::NormalisableRange<float>(0.05f, 0.95f), 0.5f));

    // --- Mixer -------------------------------------------------------------
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::oscMix, 1},     "Osc Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));   // FROZEN legacy
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::osc1Level, 1},  "Osc1 Level", juce::NormalisableRange<float>(0.0f, 1.0f), 0.8f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::osc2Level, 1},  "Osc2 Level", juce::NormalisableRange<float>(0.0f, 1.0f), 0.8f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::osc3Level, 1},  "Osc3 Level", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::noiseLevel, 1}, "Noise", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::osc1On, 1},    "Osc1 On", true));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::osc2On, 1},    "Osc2 On", true));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::osc3On, 1},    "Osc3 On", false));

    // --- Velocity ----------------------------------------------------------
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::velToAmp, 1},    "Vel->Amp", juce::NormalisableRange<float>(0.0f, 1.0f), 0.7f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::velToCutoff, 1}, "Vel->Cutoff", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));

    // --- Filter ------------------------------------------------------------
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::filterType, 1},   "Filter Type", juce::StringArray{ "LP", "HP", "BP", "Notch" }, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterCutoff, 1}, "Cutoff", cutoffRange, 2000.0f, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterReso, 1},   "Resonance", juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterEnvAmt, 1}, "Filter Env Amt", juce::NormalisableRange<float>(-1.0f, 1.0f), 0.3f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterKeytrack, 1},"Keytrack", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));

    // --- Envelopes ----------------------------------------------------------
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampAttack, 1},  "Amp Attack",  timeRange, 0.005f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampDecay, 1},   "Amp Decay",   timeRange, 0.1f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampSustain, 1}, "Amp Sustain", juce::NormalisableRange<float>(0.0f, 1.0f), 0.8f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampRelease, 1}, "Amp Release", timeRange, 0.15f, juce::AudioParameterFloatAttributes().withLabel ("s")));

    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltAttack, 1},  "Flt Attack",  timeRange, 0.005f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltDecay, 1},   "Flt Decay",   timeRange, 0.2f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltSustain, 1}, "Flt Sustain", juce::NormalisableRange<float>(0.0f, 1.0f), 0.3f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltRelease, 1}, "Flt Release", timeRange, 0.2f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltEnvToPitch, 1}, "Mod Env->Pitch", juce::NormalisableRange<float>(-48.0f, 48.0f), 0.0f, juce::AudioParameterFloatAttributes().withLabel ("st")));

    // --- LFO -----------------------------------------------------------------
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfoRate, 1},  "LFO Rate", juce::NormalisableRange<float>(0.01f, 30.0f, 0.0f, 0.4f), 2.0f, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfoDepth, 1}, "LFO Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfoShape, 1}, "LFO Shape", juce::StringArray{ "Triangle", "Sine", "Square", "S&H" }, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfoDest, 1},  "LFO Dest",  juce::StringArray{ "Off", "Pitch", "Cutoff", "PW" }, 0));
    // LFO 2 + 3 (Sub-phase 2). Default depth 0 / dest Off -> inert, so goldens hold.
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfo2Rate, 1},  "LFO2 Rate", juce::NormalisableRange<float>(0.01f, 30.0f, 0.0f, 0.4f), 2.0f, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfo2Depth, 1}, "LFO2 Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo2Shape, 1}, "LFO2 Shape", juce::StringArray{ "Triangle", "Sine", "Square", "S&H" }, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo2Dest, 1},  "LFO2 Dest",  juce::StringArray{ "Off", "Pitch", "Cutoff", "PW" }, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfo3Rate, 1},  "LFO3 Rate", juce::NormalisableRange<float>(0.01f, 30.0f, 0.0f, 0.4f), 2.0f, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfo3Depth, 1}, "LFO3 Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo3Shape, 1}, "LFO3 Shape", juce::StringArray{ "Triangle", "Sine", "Square", "S&H" }, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo3Dest, 1},  "LFO3 Dest",  juce::StringArray{ "Off", "Pitch", "Cutoff", "PW" }, 0));
    // J1: per-LFO tempo sync (default OFF -> goldens hold) + note-division (default 1/8, index 5).
    {
        const juce::StringArray divs { "4 bar","2 bar","1/1","1/2","1/4","1/8","1/16","1/32","1/4T","1/8T","1/16T","1/4.","1/8.","1/16." };
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::lfoSync,  1}, "LFO Sync",  false));
        params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfoDiv,   1}, "LFO Div",  divs, 5));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::lfo2Sync, 1}, "LFO2 Sync", false));
        params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo2Div,  1}, "LFO2 Div", divs, 5));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::lfo3Sync, 1}, "LFO3 Sync", false));
        params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfo3Div,  1}, "LFO3 Div", divs, 5));
    }

    // --- Global --------------------------------------------------------------
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::glideTime, 1}, "Glide", juce::NormalisableRange<float>(0.0f, 2.0f, 0.0f, 0.4f), 0.0f, juce::AudioParameterFloatAttributes().withLabel ("s")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::masterGain, 1},"Master", juce::NormalisableRange<float>(0.0f, 1.0f), 0.7f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::polyMode, 1},  "Mode", juce::StringArray{ "Poly", "Mono", "Legato" }, 0));

    // --- Part mixer (Sub-phase 2) --------------------------------------------
    // level 0..2 (unity 1.0), pan -1..+1 (centre 0). Defaults keep goldens.
    {
        const juce::NormalisableRange<float> lvlR (0.0f, 2.0f), panR (-1.0f, 1.0f);
        const char* lvlIDs[] { ID::part0Level, ID::part1Level, ID::part2Level, ID::part3Level };
        const char* panIDs[] { ID::part0Pan,   ID::part1Pan,   ID::part2Pan,   ID::part3Pan   };
        for (int p = 0; p < 4; ++p)
        {
            params.push_back(std::make_unique<P>(juce::ParameterID{lvlIDs[p], 1}, "P" + juce::String(p) + " Level", lvlR, 1.0f));
            params.push_back(std::make_unique<P>(juce::ParameterID{panIDs[p], 1}, "P" + juce::String(p) + " Pan",   panR, 0.0f));
        }
    }

    // --- Chord engine (7B) ---------------------------------------------------
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::chordEnabled, 1}, "Chord", false));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::chordRoot, 1}, "Chord Root",
        juce::StringArray{ "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" }, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::chordScale, 1}, "Chord Scale",
        juce::StringArray{ "Major", "Minor" }, 0));

    // --- FX (6B): global reorderable stereo chain --------------------------
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::fxChorusOn, 1},   "Chorus On", false));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::chorusRate, 1},    "Chorus Rate", juce::NormalisableRange<float>(0.05f, 8.0f, 0.0f, 0.4f), 0.8f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::chorusDepth, 1},   "Chorus Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::chorusMix, 1},     "Chorus Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));

    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::fxDelayOn, 1},     "Delay On", false));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::delayTime, 1},     "Delay Time", juce::NormalisableRange<float>(1.0f, 1500.0f, 0.0f, 0.4f), 300.0f, juce::AudioParameterFloatAttributes().withLabel ("ms")));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::delayFeedback, 1}, "Delay Feedback", juce::NormalisableRange<float>(0.0f, 0.95f), 0.35f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::delayMix, 1},      "Delay Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.35f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::delaySpread, 1},   "Delay Spread", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::fxReverbOn, 1},    "Reverb On", false));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::reverbSize, 1},    "Reverb Size", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::reverbDamp, 1},    "Reverb Damp", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::reverbWidth, 1},   "Reverb Width", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::reverbMix, 1},     "Reverb Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.3f));

    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::fxWidthOn, 1},     "Width On", false));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::stereoWidth, 1},   "Stereo Width", juce::NormalisableRange<float>(0.0f, 2.0f), 1.4f));

    // --- Macros (R2): 8 performance knobs, default 0 (inert; routed in R3) -----
    {
        const char* macroIDs[] { ID::macro1, ID::macro2, ID::macro3, ID::macro4,
                                 ID::macro5, ID::macro6, ID::macro7, ID::macro8 };
        for (int m = 0; m < 8; ++m)
            params.push_back(std::make_unique<P>(juce::ParameterID{macroIDs[m], 1},
                "Macro " + juce::String (m + 1), juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    }

    // --- Master parametric EQ (R2): defaults flat + off => bit-identical bypass ---
    {
        const juce::NormalisableRange<float> gainR (-18.0f, 18.0f), qR (0.3f, 8.0f, 0.0f, 0.5f);
        const juce::NormalisableRange<float> fR (20.0f, 20000.0f, 0.0f, 0.25f);
        const auto hz = juce::AudioParameterFloatAttributes().withLabel ("Hz");
        const auto db = juce::AudioParameterFloatAttributes().withLabel ("dB");

        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::eqOn, 1}, "EQ On", false));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqLsFreq, 1}, "EQ Low Freq",  fR, 120.0f,  hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqLsGain, 1}, "EQ Low Gain",  gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqLmFreq, 1}, "EQ LoMid Freq", fR, 500.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqLmGain, 1}, "EQ LoMid Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqLmQ, 1},    "EQ LoMid Q",   qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqHmFreq, 1}, "EQ HiMid Freq", fR, 3000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqHmGain, 1}, "EQ HiMid Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqHmQ, 1},    "EQ HiMid Q",   qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqHsFreq, 1}, "EQ High Freq", fR, 8000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::eqHsGain, 1}, "EQ High Gain", gainR, 0.0f, db));

        // Per-part EQ (task #51): 3 fully-parametric bells. Default flat + off => bypass.
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqOn, 1}, "Part EQ On", false));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB1Freq, 1}, "Part EQ B1 Freq", fR, 180.0f,  hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB1Gain, 1}, "Part EQ B1 Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB1Q, 1},    "Part EQ B1 Q",    qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB2Freq, 1}, "Part EQ B2 Freq", fR, 1000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB2Gain, 1}, "Part EQ B2 Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB2Q, 1},    "Part EQ B2 Q",    qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB3Freq, 1}, "Part EQ B3 Freq", fR, 5000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB3Gain, 1}, "Part EQ B3 Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB3Q, 1},    "Part EQ B3 Q",    qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB4Freq, 1}, "Part EQ B4 Freq", fR, 10000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB4Gain, 1}, "Part EQ B4 Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB4Q, 1},    "Part EQ B4 Q",    qR, 0.9f));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB5Freq, 1}, "Part EQ B5 Freq", fR, 14000.0f, hz));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB5Gain, 1}, "Part EQ B5 Gain", gainR, 0.0f, db));
        params.push_back(std::make_unique<P>(juce::ParameterID{ID::peqB5Q, 1},    "Part EQ B5 Q",    qR, 0.9f));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqB1On, 1}, "Part EQ B1 On", true));   // per-band on/off (K1)
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqB2On, 1}, "Part EQ B2 On", true));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqB3On, 1}, "Part EQ B3 On", true));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqB4On, 1}, "Part EQ B4 On", true));
        params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::peqB5On, 1}, "Part EQ B5 On", true));
    }

    // --- Tempo + arpeggiator (R3) --------------------------------------------
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::tempo, 1}, "Tempo", juce::NormalisableRange<float>(20.0f, 300.0f), 120.0f, juce::AudioParameterFloatAttributes().withLabel ("bpm")));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::arpOn, 1}, "Arp", false));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::arpMode, 1}, "Arp Mode", juce::StringArray{ "Up", "Down", "Up-Down", "Random", "As-Played" }, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::arpOctaves, 1}, "Arp Octaves", juce::NormalisableRange<float>(1.0f, 4.0f, 1.0f), 1.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::arpGate, 1}, "Arp Gate", juce::NormalisableRange<float>(0.05f, 1.0f), 0.5f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::arpSwing, 1}, "Arp Swing", juce::NormalisableRange<float>(0.0f, 0.7f), 0.0f));
    // (#54) Arp velocity is now PER-STEP on the arp grid (state props arp_steps/arp_vel), not a
    // single APVTS knob — the brief "arp_vel" parameter added earlier in this cycle is retired.
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::arpLatch, 1}, "Arp Latch", false));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::arpHold, 1}, "Arp Hold", false));

    // --- Looper (R3) ---------------------------------------------------------
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::loopRec, 1},  "Loop Rec", false));
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::loopPlay, 1}, "Loop Play", false));
    // J2: per-lane loop length. Enum EXTENDED APPEND-ONLY (indices 0-2 == 1/2/4 unchanged) so
    // old sessions restore correctly. lane 1 = loop_bars, lanes 2-4 = loop_bars2/3/4.
    const juce::StringArray loopBarChoices { "1", "2", "4", "8", "16", "32" };
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::loopBars, 1}, "Loop Bars", loopBarChoices, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::loopMode, 1}, "Loop Mode", juce::StringArray{ "MIDI", "AUDIO" }, 0));
    // Lanes 2-4 (P2/P3/P4): per-lane REC / PLAY / MODE / BARS.
    for (auto* r : { ID::loopRec2, ID::loopRec3, ID::loopRec4 })
        params.push_back(std::make_unique<Pb>(juce::ParameterID{r, 1}, juce::String (r), false));
    for (auto* pl : { ID::loopPlay2, ID::loopPlay3, ID::loopPlay4 })
        params.push_back(std::make_unique<Pb>(juce::ParameterID{pl, 1}, juce::String (pl), false));
    for (auto* mo : { ID::loopMode2, ID::loopMode3, ID::loopMode4 })
        params.push_back(std::make_unique<Pc>(juce::ParameterID{mo, 1}, juce::String (mo), juce::StringArray{ "MIDI", "AUDIO" }, 0));
    for (auto* bb : { ID::loopBars2, ID::loopBars3, ID::loopBars4 })
        params.push_back(std::make_unique<Pc>(juce::ParameterID{bb, 1}, juce::String (bb), loopBarChoices, 0));
    // J3: scene launch quantum — when a tapped scene actually switches (aligned to the master bar clock).
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::sceneQuant, 1}, "Scene Launch",
                                          juce::StringArray{ "1 bar", "2 bar", "4 bar", "8 bar", "Loop end" }, 4));   // default: wait for the longest loop
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::clockOut, 1}, "Clock Out", false));   // #85: MIDI clock transmit
    for (auto* q : { ID::loopQuant, ID::loopQuant2, ID::loopQuant3, ID::loopQuant4 })   // 1/32 quantize, default ON
        params.push_back(std::make_unique<Pb>(juce::ParameterID{q, 1}, juce::String (q), true));

    // --- Step sequencer (R3 Group 2) -----------------------------------------
    params.push_back(std::make_unique<Pb>(juce::ParameterID{ID::seqOn, 1},   "Seq", false));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::seqGate, 1}, "Seq Gate", juce::NormalisableRange<float>(0.05f, 1.0f), 0.5f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::seqTarget, 1}, "Seq Target", juce::StringArray{ "P1", "P2", "P3", "P4" }, 3));   // default P4 = the drum-kit part (default scene)

    return { params.begin(), params.end() };
}

// True if `loaded` predates the per-source level mixer (6A): it carries the
// legacy `osc_mix` but no `osc1_level` child. MUST be checked on the raw loaded
// tree BEFORE apvts.replaceState() — replaceState back-fills the missing level
// children into that same (reference-counted) tree, which would hide the gap.
inline bool stateNeedsLevelMigration (const juce::ValueTree& loaded)
{
    for (auto child : loaded)
        if (child.getProperty ("id").toString() == ParamID::osc1Level)
            return false;
    return true;
}

// Derive the three per-source levels from the legacy `osc_mix` crossfade
// (osc1 = 1-mix, osc2 = mix, osc3 off) so patches — including the user's own
// saved presets — sound unchanged. Call AFTER replaceState, only when
// stateNeedsLevelMigration() returned true for the loaded tree.
inline void applyLegacyOscLevelMigration (juce::AudioProcessorValueTreeState& apvts)
{
    namespace ID = ParamID;
    const float mix = apvts.getRawParameterValue (ID::oscMix)->load();
    apvts.getParameter (ID::osc1Level)->setValueNotifyingHost (1.0f - mix);
    apvts.getParameter (ID::osc2Level)->setValueNotifyingHost (mix);
    apvts.getParameter (ID::osc3Level)->setValueNotifyingHost (0.0f);
}

// ============================================================================
// Preset exclusion policy: parameters that are GLOBAL PERFORMANCE controls the
// player owns — they must PERSIST across every preset operation (Init, factory,
// and user load) and are NOT written into a saved preset. Recalling a patch must
// never move them.
//
//   * master_gain — the output level. Like its Randomize exclusion
//     ([[randomizeExclusions]]), the player sets it once and it stays put; a
//     preset is a SOUND, not a level.
//
// This is a SEPARATE, narrower list than randomizeExclusions() (which also pins
// velocity routing, poly mode, glide, etc. — those ARE legitimate patch character
// and are recalled by presets). Kept as one visible list so the rule is auditable.
// ============================================================================
namespace PresetPolicy
{
    inline juce::StringArray excludedParams() { return { ParamID::masterGain }; }

    // Snapshot the excluded params' current normalized values (call BEFORE a load).
    inline std::vector<std::pair<juce::String, float>>
        capture (juce::AudioProcessorValueTreeState& apvts)
    {
        std::vector<std::pair<juce::String, float>> saved;
        for (auto& id : excludedParams())
            if (auto* p = apvts.getParameter (id)) saved.emplace_back (id, p->getValue());
        return saved;
    }

    // Write the snapshotted values back (call AFTER a load) so the preset never
    // changed the player's performance controls.
    inline void restore (juce::AudioProcessorValueTreeState& apvts,
                         const std::vector<std::pair<juce::String, float>>& saved)
    {
        for (auto& kv : saved)
            if (auto* p = apvts.getParameter (kv.first)) p->setValueNotifyingHost (kv.second);
    }

    // Remove the excluded params from a state tree before saving a user preset, so
    // the file carries no level (and can't impose one on another rig).
    inline void stripFromState (juce::ValueTree& state)
    {
        const auto excluded = excludedParams();
        for (int i = state.getNumChildren(); --i >= 0;)
            if (excluded.contains (state.getChild (i).getProperty ("id").toString()))
                state.removeChild (i, nullptr);
    }
}
