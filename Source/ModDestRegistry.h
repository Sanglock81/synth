#pragma once
#include <juce_core/juce_core.h>
#include "DSP/ModMatrix.h"
#include "Parameters.h"

// ============================================================================
// Mod-matrix DESTINATION REGISTRY (#56 G4). One table describing every routable
// destination of the FOCUSED part: its ModMatrix::Dest id, the APVTS parameter it
// maps to (for block-tier: base value + range; empty for the voice-tier pure-mod
// dests like Pitch/Amp), a display name, and a category for the grouped dropdown.
//
// VOICE-TIER dests (Pitch..Osc3Level) are applied per-voice in SynthVoice; they have
// no paramId (they are additive mod points, not knobs). BLOCK-TIER dests carry a
// paramId — the processor reads its normalized value + range, adds the matrix's
// normalized offset, and writes the result to the engine's per-block param struct.
//
// "any function is linkable" = this table. Excluded on purpose (documented): choice/
// mode selectors (osc wave, filter type, poly mode, LFO shape/dest), on/off enables,
// and master gain — modulating a discrete switch or the master isn't musical.
// ============================================================================
namespace moddest
{
    enum Category { Osc = 0, Filter, Env, Lfo, Fx, Part, kNumCategories };

    inline const char* categoryName (int c)
    {
        switch (c) { case Osc: return "Osc"; case Filter: return "Filter"; case Env: return "Env";
                     case Lfo: return "LFO"; case Fx: return "FX"; case Part: return "Part"; default: return "?"; }
    }

    struct Entry { int dest; const char* paramId; const char* name; int category; };

    // paramId "" => voice-tier pure-mod dest (no knob). Order groups by category for the menu.
    inline const std::vector<Entry>& table()
    {
        namespace P = ParamID;
        static const std::vector<Entry> t {
            // Osc — voice-tier dests carry the paramId of THEIR knob so the control resolves to
            // them (Pitch/WavePos have no single knob; they stay overlay-only). PulseWidth is one
            // offset shared by all 3 osc PW knobs (see destForParam).
            { ModMatrix::Pitch,      "",             "Pitch",        Osc },
            { ModMatrix::PulseWidth, P::osc1PW,      "Pulse Width",  Osc },
            { ModMatrix::WavePos,    "",             "Wave Pos",     Osc },
            { ModMatrix::Osc1Level,  P::osc1Level,   "Osc 1 Level",  Osc },
            { ModMatrix::Osc2Level,  P::osc2Level,   "Osc 2 Level",  Osc },
            { ModMatrix::Osc3Level,  P::osc3Level,   "Osc 3 Level",  Osc },
            { ModMatrix::Osc1Octave, P::osc1Octave,  "Osc 1 Octave", Osc },
            { ModMatrix::Osc1Detune, P::osc1Detune,  "Osc 1 Detune", Osc },
            { ModMatrix::Osc2Octave, P::osc2Octave,  "Osc 2 Octave", Osc },
            { ModMatrix::Osc2Detune, P::osc2Detune,  "Osc 2 Detune", Osc },
            { ModMatrix::Osc3Octave, P::osc3Octave,  "Osc 3 Octave", Osc },
            { ModMatrix::Osc3Detune, P::osc3Detune,  "Osc 3 Detune", Osc },
            // Filter
            { ModMatrix::Cutoff,        P::filterCutoff, "Cutoff",       Filter },
            { ModMatrix::Resonance,     P::filterReso,   "Resonance",    Filter },
            { ModMatrix::FilterEnvAmt,  P::filterEnvAmt, "Filter EnvAmt",Filter },
            { ModMatrix::FilterKeytrack,P::filterKeytrack,"Filter Keytrk",Filter },
            { ModMatrix::VelToCutoff,   P::velToCutoff,  "Vel>Cutoff",   Filter },
            // Env
            { ModMatrix::Amp,          "",             "Amp",          Env },
            { ModMatrix::VelToAmp,     P::velToAmp,    "Vel>Amp",      Env },
            { ModMatrix::AmpAttack,    P::ampAttack,   "Amp Attack",   Env },
            { ModMatrix::AmpDecay,     P::ampDecay,    "Amp Decay",    Env },
            { ModMatrix::AmpSustain,   P::ampSustain,  "Amp Sustain",  Env },
            { ModMatrix::AmpRelease,   P::ampRelease,  "Amp Release",  Env },
            { ModMatrix::FltAttack,    P::fltAttack,   "Flt Attack",   Env },
            { ModMatrix::FltDecay,     P::fltDecay,    "Flt Decay",    Env },
            { ModMatrix::FltSustain,   P::fltSustain,  "Flt Sustain",  Env },
            { ModMatrix::FltRelease,   P::fltRelease,  "Flt Release",  Env },
            { ModMatrix::FltEnvToPitch,P::fltEnvToPitch,"ModEnv>Pitch", Env },
            // LFO
            { ModMatrix::Lfo1Rate,  P::lfoRate,   "LFO 1 Rate",  Lfo },
            { ModMatrix::Lfo1Depth, P::lfoDepth,  "LFO 1 Depth", Lfo },
            { ModMatrix::Lfo2Rate,  P::lfo2Rate,  "LFO 2 Rate",  Lfo },
            { ModMatrix::Lfo2Depth, P::lfo2Depth, "LFO 2 Depth", Lfo },
            { ModMatrix::Lfo3Rate,  P::lfo3Rate,  "LFO 3 Rate",  Lfo },
            { ModMatrix::Lfo3Depth, P::lfo3Depth, "LFO 3 Depth", Lfo },
            // FX
            { ModMatrix::ChorusRate,   P::chorusRate,    "Chorus Rate",   Fx },
            { ModMatrix::ChorusDepth,  P::chorusDepth,   "Chorus Depth",  Fx },
            { ModMatrix::ChorusMix,    P::chorusMix,     "Chorus Mix",    Fx },
            { ModMatrix::DelayTime,    P::delayTime,     "Delay Time",    Fx },
            { ModMatrix::DelayFeedback,P::delayFeedback, "Delay Feedback",Fx },
            { ModMatrix::DelayMix,     P::delayMix,      "Delay Mix",     Fx },
            { ModMatrix::DelaySpread,  P::delaySpread,   "Delay Spread",  Fx },
            { ModMatrix::ReverbSize,   P::reverbSize,    "Reverb Size",   Fx },
            { ModMatrix::ReverbDamp,   P::reverbDamp,    "Reverb Damp",   Fx },
            { ModMatrix::ReverbWidth,  P::reverbWidth,   "Reverb Width",  Fx },
            { ModMatrix::ReverbMix,    P::reverbMix,     "Reverb Mix",    Fx },
            { ModMatrix::StereoWidth,  P::stereoWidth,   "Stereo Width",  Fx },
            { ModMatrix::EqB1Gain,     P::peqB1Gain,     "EQ Low Gain",    Fx },
            { ModMatrix::EqB2Gain,     P::peqB2Gain,     "EQ L-Mid Gain",  Fx },
            { ModMatrix::EqB3Gain,     P::peqB3Gain,     "EQ H-Mid Gain",  Fx },
            { ModMatrix::EqB4Gain,     P::peqB4Gain,     "EQ High Gain",   Fx },
            { ModMatrix::EqB5Gain,     P::peqB5Gain,     "EQ Air Gain",    Fx },
            // Part
            { ModMatrix::GlideTime, P::glideTime, "Glide",   Part },
            // PartLevel / PartPan reserved (enum stable) — they modulate the mixer, a later seam.
        };
        return t;
    }

    inline const Entry* find (int dest)
    {
        for (auto& e : table()) if (e.dest == dest) return &e;
        return nullptr;
    }
    inline juce::String nameFor (int dest)
    {
        if (dest == ModMatrix::DstNone) return {};
        if (auto* e = find (dest)) return e->name;
        return {};
    }

    // Resolve a control's APVTS parameter to its mod destination (DstNone if not a target).
    // This is the SINGLE source of truth: any control whose paramId resolves here becomes a
    // LINK target + animates; anything else (choice/mode/enable/master gain) gets nothing.
    inline int destForParam (const juce::String& paramId)
    {
        if (paramId.isEmpty()) return ModMatrix::DstNone;
        // Osc 2/3 PW share the single PulseWidth offset with osc 1.
        if (paramId == ParamID::osc2PW || paramId == ParamID::osc3PW) return ModMatrix::PulseWidth;
        for (auto& e : table()) if (e.paramId != nullptr && *e.paramId != 0 && paramId == e.paramId) return e.dest;
        return ModMatrix::DstNone;
    }
}
