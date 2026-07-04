#include "PluginProcessor.h"
#include "PluginEditor.h"

VASynthProcessor::VASynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
}

void VASynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    monoScratch.setSize (1, samplesPerBlock);
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

    return p;
}

void VASynthProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    monoScratch.setSize (1, buffer.getNumSamples(), false, false, true);
    monoScratch.clear();

    auto* mono = monoScratch.getWritePointer (0);
    const auto params = snapshotParams();

    namespace ID = ParamID;
    const float lfoRate  = rp (apvts, ID::lfoRate);
    const float lfoDepth = rp (apvts, ID::lfoDepth);
    const int   lfoShape = (int) rp (apvts, ID::lfoShape);
    const int   lfoDest  = (int) rp (apvts, ID::lfoDest);
    const float master   = rp (apvts, ID::masterGain);

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
        else if (msg.isController())
            midiLearn.handleCC (msg.getChannel(),
                                msg.getControllerNumber(),
                                msg.getControllerValue());
        // TODO: pitch bend, mod wheel (CC1) -> hardwired vibrato/cutoff,
        //       sustain pedal (CC64) — the Korg B2 sends all three.
    }

    if (renderedUpTo < buffer.getNumSamples())
        engine.render (mono + renderedUpTo, buffer.getNumSamples() - renderedUpTo,
                       params, lfoRate, lfoShape, lfoDepth, lfoDest);

    // --- mono -> stereo out, master gain -------------------------------------
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.addFrom (ch, 0, mono, buffer.getNumSamples(), master);
}

// --- state ---------------------------------------------------------------
void VASynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VASynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
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
