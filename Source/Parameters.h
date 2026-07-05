#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

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

    // LFO 1 (global). v1: routed to a single selectable destination.
    inline constexpr auto lfoRate     = "lfo_rate";
    inline constexpr auto lfoDepth    = "lfo_depth";
    inline constexpr auto lfoShape    = "lfo_shape";     // tri / sine / square / s&h
    inline constexpr auto lfoDest     = "lfo_dest";      // off / pitch / cutoff / pw

    // Global
    inline constexpr auto glideTime   = "glide_time";
    inline constexpr auto masterGain  = "master_gain";
    inline constexpr auto polyMode    = "poly_mode";     // poly / mono / legato
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
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc1Detune, 1},"Osc1 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc1PW, 1},    "Osc1 PW", juce::NormalisableRange<float>(0.05f, 0.95f), 0.5f));

    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::osc2Wave, 1},  "Osc2 Wave", waveNames, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2Octave, 1},"Osc2 Octave", juce::NormalisableRange<float>(-2.0f, 2.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2Detune, 1},"Osc2 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 7.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc2PW, 1},    "Osc2 PW", juce::NormalisableRange<float>(0.05f, 0.95f), 0.5f));

    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::osc3Wave, 1},  "Osc3 Wave", waveNames, 0));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc3Octave, 1},"Osc3 Octave", juce::NormalisableRange<float>(-2.0f, 2.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::osc3Detune, 1},"Osc3 Detune", juce::NormalisableRange<float>(-100.0f, 100.0f), 0.0f));
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
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterCutoff, 1}, "Cutoff", cutoffRange, 2000.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterReso, 1},   "Resonance", juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterEnvAmt, 1}, "Filter Env Amt", juce::NormalisableRange<float>(-1.0f, 1.0f), 0.3f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::filterKeytrack, 1},"Keytrack", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));

    // --- Envelopes ----------------------------------------------------------
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampAttack, 1},  "Amp Attack",  timeRange, 0.005f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampDecay, 1},   "Amp Decay",   timeRange, 0.1f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampSustain, 1}, "Amp Sustain", juce::NormalisableRange<float>(0.0f, 1.0f), 0.8f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::ampRelease, 1}, "Amp Release", timeRange, 0.15f));

    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltAttack, 1},  "Flt Attack",  timeRange, 0.005f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltDecay, 1},   "Flt Decay",   timeRange, 0.2f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltSustain, 1}, "Flt Sustain", juce::NormalisableRange<float>(0.0f, 1.0f), 0.3f));
    params.push_back(std::make_unique<P>(juce::ParameterID{ID::fltRelease, 1}, "Flt Release", timeRange, 0.2f));

    // --- LFO -----------------------------------------------------------------
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfoRate, 1},  "LFO Rate", juce::NormalisableRange<float>(0.01f, 30.0f, 0.0f, 0.4f), 2.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::lfoDepth, 1}, "LFO Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfoShape, 1}, "LFO Shape", juce::StringArray{ "Triangle", "Sine", "Square", "S&H" }, 0));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::lfoDest, 1},  "LFO Dest",  juce::StringArray{ "Off", "Pitch", "Cutoff", "PW" }, 0));

    // --- Global --------------------------------------------------------------
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::glideTime, 1}, "Glide", juce::NormalisableRange<float>(0.0f, 2.0f, 0.0f, 0.4f), 0.0f));
    params.push_back(std::make_unique<P >(juce::ParameterID{ID::masterGain, 1},"Master", juce::NormalisableRange<float>(0.0f, 1.0f), 0.7f));
    params.push_back(std::make_unique<Pc>(juce::ParameterID{ID::polyMode, 1},  "Mode", juce::StringArray{ "Poly", "Mono", "Legato" }, 0));

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
