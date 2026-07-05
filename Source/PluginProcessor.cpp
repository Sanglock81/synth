#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <chrono>

// Default pitch-bend range in semitones (+/-). The Launchkey touch strip is the
// only bend source in the rig; the Korg B2 sends none (stays centred).
static constexpr float kPitchBendRange = 2.0f;

#ifndef VASYNTH_VERSION
 #define VASYNTH_VERSION "?"
#endif
#ifndef VASYNTH_GIT_HASH
 #define VASYNTH_GIT_HASH "?"
#endif
#ifndef VASYNTH_BUILD_TYPE
 #define VASYNTH_BUILD_TYPE "?"
#endif

// Flush the log with a final marker on a crash so post-mortem logs show where
// things stopped. The AudioHealthLogger installs its FileLogger as the current
// JUCE logger, so writeToLog reaches the same file (and flushes).
static void vaSynthCrashHandler (void*)
{
    juce::Logger::writeToLog ("*** VA SYNTH CRASH — application crash handler ***");
    juce::Logger::writeToLog (juce::SystemStats::getStackBacktrace());
}

VASynthProcessor::VASynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    juce::SystemStats::setApplicationCrashHandler (vaSynthCrashHandler);
}

// Oscillator anti-aliasing quality. Compile-time default is Efficient (glitch-
// free with headroom on the 2-core live ThinkPad); build with
// -DVASYNTH_OSC_QUALITY_HQ for the studio/Windows HQ default. A runtime GUI
// selector (re-preparing voices off the audio thread) is a v2 item.
#ifdef VASYNTH_OSC_QUALITY_HQ
 #define VASYNTH_OSC_QUALITY PolyBlepOscillator::Quality::HQ
#else
 #define VASYNTH_OSC_QUALITY PolyBlepOscillator::Quality::Efficient
#endif

// Max simultaneous voices. Default 12 keeps the Efficient oscillator's worst-case
// (16 saw voices was ~41% of the ThinkPad budget, over the ~30% gate) within
// budget on the 2-core live ThinkPad. Studio/HQ builds can raise it (<= 16).
#ifndef VASYNTH_MAX_VOICES
 #define VASYNTH_MAX_VOICES 12
#endif

void VASynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setOscQuality (VASYNTH_OSC_QUALITY);
    engine.setMaxVoices (VASYNTH_MAX_VOICES);
    engine.prepare (sampleRate);
    // Allocate the mono mixdown buffer ONCE, at the host's max block size.
    // processBlock never resizes it (JUCE guarantees numSamples <= this).
    monoScratch.setSize (1, juce::jmax (1, samplesPerBlock), false, false, true);

    masterGain.reset (sampleRate, 0.02);                       // ~20 ms ramp
    masterGain.setCurrentAndTargetValue (
        apvts.getRawParameterValue (ParamID::masterGain)->load());

    budgetMs = (sampleRate > 0.0) ? (double (samplesPerBlock) / sampleRate * 1000.0) : 2.667;

    // Startup banner (processor-accessible fields; device/type/MIDI-inputs are
    // logged by the standalone app which owns the device manager).
    const char* quality =
        (VASYNTH_OSC_QUALITY == PolyBlepOscillator::Quality::HQ)   ? "HQ"
      : (VASYNTH_OSC_QUALITY == PolyBlepOscillator::Quality::None) ? "None" : "Efficient";
    health.logMessage (juce::String ("VA Synth ") + VASYNTH_VERSION + " (" + VASYNTH_GIT_HASH
                       + ", " + VASYNTH_BUILD_TYPE + ")  wrapper=" + juce::String ((int) wrapperType)
                       + "  osc-quality=" + quality
                       + "  maxVoices=" + juce::String (VASYNTH_MAX_VOICES));
    health.prepare (sampleRate, samplesPerBlock);
}

// Helper: read a float parameter's current value from the APVTS.
// getRawParameterValue returns an atomic<float>* — lock-free, audio-safe.
static float rp (const juce::AudioProcessorValueTreeState& s, const char* id)
{
    return s.getRawParameterValue (id)->load();
}

VoiceParams VASynthProcessor::snapshotParams() const
{
    namespace ID = ParamID;
    VoiceParams p;

    p.osc1Wave   = (int) rp (apvts, ID::osc1Wave);
    p.osc2Wave   = (int) rp (apvts, ID::osc2Wave);
    p.osc1Octave = rp (apvts, ID::osc1Octave);
    p.osc2Octave = rp (apvts, ID::osc2Octave);
    p.osc1Detune = rp (apvts, ID::osc1Detune);
    p.osc2Detune = rp (apvts, ID::osc2Detune);
    p.osc1PW     = rp (apvts, ID::osc1PW);
    p.osc2PW     = rp (apvts, ID::osc2PW);
    p.oscMix     = rp (apvts, ID::oscMix);
    p.noiseLevel = rp (apvts, ID::noiseLevel);

    p.filterType   = (int) rp (apvts, ID::filterType);
    p.cutoffHz     = rp (apvts, ID::filterCutoff);
    p.resonance    = rp (apvts, ID::filterReso);
    p.filterEnvAmt = rp (apvts, ID::filterEnvAmt);
    p.keytrack     = rp (apvts, ID::filterKeytrack);

    p.ampA = rp (apvts, ID::ampAttack);
    p.ampD = rp (apvts, ID::ampDecay);
    p.ampS = rp (apvts, ID::ampSustain);
    p.ampR = rp (apvts, ID::ampRelease);

    p.fltA = rp (apvts, ID::fltAttack);
    p.fltD = rp (apvts, ID::fltDecay);
    p.fltS = rp (apvts, ID::fltSustain);
    p.fltR = rp (apvts, ID::fltRelease);

    p.glideTime = rp (apvts, ID::glideTime);

    return p;
}

void VASynthProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto tStart = std::chrono::steady_clock::now();   // cheap, RT-safe (vDSO)
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    // No resize on the audio thread — monoScratch was sized in prepareToPlay.
    // Guard against a misbehaving host sending an oversized block.
    jassert (numSamples <= monoScratch.getNumSamples());
    auto* mono = monoScratch.getWritePointer (0);
    juce::FloatVectorOperations::clear (mono, numSamples);

    // Merge QWERTY computer-keyboard notes into the MIDI stream (empty and free
    // when no keys are held), so they share the exact hardware-MIDI path below.
    qwertyKeyboardState.processNextMidiBuffer (midi, 0, numSamples, true);

    const auto params = snapshotParams();

    namespace ID = ParamID;
    const float lfoRate  = rp (apvts, ID::lfoRate);
    const float lfoDepth = rp (apvts, ID::lfoDepth);
    const int   lfoShape = (int) rp (apvts, ID::lfoShape);
    const int   lfoDest  = (int) rp (apvts, ID::lfoDest);
    const float master   = rp (apvts, ID::masterGain);

    engine.setPolyMode ((int) rp (apvts, ID::polyMode));   // poly / mono / legato

    // --- sample-accurate MIDI dispatch --------------------------------------
    // Render up to each event, dispatch it, continue. This keeps note timing
    // tight even inside large buffers — important for anything sequenced.
    int renderedUpTo = 0;

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int  pos = metadata.samplePosition;

        if (pos > renderedUpTo)
        {
            engine.render (mono + renderedUpTo, pos - renderedUpTo,
                           params, lfoRate, lfoShape, lfoDepth, lfoDest);
            renderedUpTo = pos;
        }

        if (msg.isNoteOn())
            engine.noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            engine.noteOff (msg.getNoteNumber());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            engine.allNotesOff();
        else if (msg.isPitchWheel())
        {
            // 14-bit, centre 8192 -> +/- kPitchBendRange semitones (Launchkey strip).
            const float norm = (msg.getPitchWheelValue() - 8192) / 8192.0f;
            engine.setPitchBend (norm * kPitchBendRange);
        }
        else if (msg.isController())
        {
            const int cc  = msg.getControllerNumber();
            const int val = msg.getControllerValue();

            if (cc == 64)                                   // sustain pedal (Korg B2 damper)
                engine.setSustainPedal (val >= 64);
            else
            {
                if (cc == 1)                                // mod wheel -> vibrato depth
                    engine.setModWheel (val / 127.0f);
                midiLearn.handleCC (msg.getChannel(), cc, val);
            }
        }
    }

    if (renderedUpTo < buffer.getNumSamples())
        engine.render (mono + renderedUpTo, buffer.getNumSamples() - renderedUpTo,
                       params, lfoRate, lfoShape, lfoDepth, lfoDest);

    // --- master gain: bake the per-sample ramp into the mono buffer, then copy
    //     to every output channel. The ramp (SmoothedValue) kills zipper on gain
    //     steps/automation without a per-channel double-ramp.
    masterGain.setTargetValue (master);
    if (masterGain.isSmoothing())
        for (int i = 0; i < numSamples; ++i)
            mono[i] *= masterGain.getNextValue();
    else
        juce::FloatVectorOperations::multiply (mono, masterGain.getTargetValue(), numSamples);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.addFrom (ch, 0, mono, numSamples, 1.0f);

    // --- audio-health telemetry (RT-safe: pushes POD events only) -----------
    const auto tEnd = std::chrono::steady_clock::now();
    const float renderMs = (float) std::chrono::duration<double, std::milli> (tEnd - tStart).count();
    health.logRenderTime (renderMs, blockIndex);
    if (renderMs > (float) budgetMs)
        health.logOverrun (renderMs, (float) budgetMs, blockIndex);   // xrun early-warning
    health.logVoiceCount (engine.activeVoiceCount());
    const std::uint64_t steals = engine.stealCount();
    health.logSteals ((int) (steals - lastSteals));
    lastSteals = steals;
    ++blockIndex;
}

// --- state ---------------------------------------------------------------
void VASynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    midiLearn.saveToTree (state);                 // append MIDILEARN child
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VASynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        midiLearn.loadFromTree (tree);            // read MIDILEARN child (if any)
        apvts.replaceState (tree);                // APVTS ignores the extra child
    }
}

juce::AudioProcessorEditor* VASynthProcessor::createEditor()
{
    return new VASynthEditor (*this);
}

// JUCE entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VASynthProcessor();
}
