#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

// ============================================================================
// Factory presets: read-only patches embedded in the binary. Each is a small
// JSON file listing a name, a category, parameter overrides (in REAL units —
// Hz, seconds, cents, choice index), and an optional FX chain order:
//
//   { "name":"Fat Saw Bass", "category":"Bass",
//     "params": { "filter_cutoff": 520, "osc2_detune": 8, "poly_mode": 1 },
//     "fxOrder": [0,1,2,3] }
//
// Applying a preset first resets every parameter to its default (so it's a full
// "Init" baseline, robust to new parameters), then applies the overrides. Real
// values are converted to normalised via each parameter's own range, so choice
// and bool params take their index / 0-1 directly. Factory presets are never
// written to; the UI's Save makes a user copy instead.
// ============================================================================

struct FactoryPreset
{
    juce::String name;
    juce::String category;
    std::vector<std::pair<juce::String, double>> params;   // paramID -> real value
    juce::Array<int> fxOrder;                              // empty => default 0,1,2,3

    static FactoryPreset fromJson (const juce::String& json, bool& ok)
    {
        FactoryPreset p;
        ok = false;
        auto v = juce::JSON::parse (json);
        if (! v.isObject()) return p;

        p.name     = v.getProperty ("name", juce::String()).toString();
        p.category = v.getProperty ("category", "User").toString();

        const auto paramsVar = v.getProperty ("params", juce::var());
        if (auto* obj = paramsVar.getDynamicObject())
            for (auto& prop : obj->getProperties())
                p.params.emplace_back (prop.name.toString(), (double) prop.value);

        if (auto* ord = v.getProperty ("fxOrder", juce::var()).getArray())
            for (auto& e : *ord) p.fxOrder.add ((int) e);

        // Require a params object so a device-profile JSON is never mistaken for a
        // preset when both are iterated out of BinaryData.
        ok = p.name.isNotEmpty() && paramsVar.isObject();
        return p;
    }

    // Reset to defaults (Init), then apply the overrides. Order is applied by the
    // caller (the processor) since it lives outside the parameter set.
    void applyParams (juce::AudioProcessorValueTreeState& apvts) const
    {
        for (auto* rp : apvts.processor.getParameters())
            rp->setValueNotifyingHost (rp->getDefaultValue());

        for (auto& kv : params)
            if (auto* rp = apvts.getParameter (kv.first))
                rp->setValueNotifyingHost (rp->convertTo0to1 ((float) kv.second));
    }
};

class FactoryPresetLibrary
{
public:
    void add (const juce::String& json)
    {
        bool ok = false;
        auto p = FactoryPreset::fromJson (json, ok);
        if (ok) presets.push_back (std::move (p));
    }

    int size() const { return (int) presets.size(); }
    const std::vector<FactoryPreset>& all() const { return presets; }

    const FactoryPreset* byName (const juce::String& n) const
    {
        for (auto& p : presets) if (p.name == n) return &p;
        return nullptr;
    }

    // Category names in first-seen order (for grouping the Load menu).
    juce::StringArray categories() const
    {
        juce::StringArray c;
        for (auto& p : presets) c.addIfNotAlreadyThere (p.category);
        return c;
    }

private:
    std::vector<FactoryPreset> presets;
};
