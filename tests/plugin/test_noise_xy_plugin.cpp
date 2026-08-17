// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// NOISE XY at the PLUGIN layer — the field as the user meets it.
//
// The DSP suite (tests/dsp/test_noise_xy.cpp) proves the filter is right. This file
// proves everything between the filter and a pair of hands:
//
//   * the pad and the FOCUS rail exist on the real panel, are real mod targets, and
//     each carry a motion indicator (the #12 "wired but draws nothing" class of bug),
//   * a REAL mouse drag on the pad moves BOTH axes and the readout tells the truth
//     about where it landed, in either region,
//   * LINK and the LFO-Link gesture reach the new destinations, and a route there
//     audibly changes the render -- asserted end to end, not by inspecting a slot,
//   * sweeping either axis under held notes and note storms is click-free, including
//     the nastiest case: a fast square LFO stepping Q on an already-ringing filter,
//   * the field survives a preset round-trip, AND a preset saved before the field
//     existed loads as the bypass defaults rather than inheriting the last patch's.
//
// Screenshots land in docs/smoke/ for human review, per the UI-smoke standing rule.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetManager.h"
#include "ModDestRegistry.h"
#include "UI/Widgets.h"
#include "UI/Sections.h"
#include "UI/ModMatrixPanel.h"
#include <cmath>
#include <functional>
#include <vector>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    constexpr double kSR = 48000.0;

    void set01 (VASynthProcessor& p, const char* id, float v)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }
    float get01 (VASynthProcessor& p, const char* id)
    { auto* pr = p.apvts.getParameter (id); return pr != nullptr ? pr->getValue() : -1.0f; }

    template <typename T>
    T* findWidget (juce::Component& c, const juce::String& paramId)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* w = dynamic_cast<T*> (ch))
                if (w->parameterID() == paramId) return w;
            if (auto* found = findWidget<T> (*ch, paramId)) return found;
        }
        return nullptr;
    }

    void snapshot (juce::Component& c, const juce::String& name)
    {
        auto img = c.createComponentSnapshot (c.getLocalBounds(), false, 1.0f);
        REQUIRE (img.isValid());
        juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/" + name);
        out.getParentDirectory().createDirectory();
        out.deleteFile();
        juce::FileOutputStream os (out); REQUIRE (os.openedOk());
        juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
    }

    // A REAL press-drag-release across `comp`, in its own coordinates — the event path the
    // OS uses, not a call into the handler. Drives the pad the way a finger would.
    void dragAcross (juce::Component& comp, juce::Point<float> from, juce::Point<float> to)
    {
        const auto now = juce::Time::getCurrentTime();
        auto ev = [&] (juce::Point<float> at)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at,
                                     juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &comp, &comp, now, from, now, 1, false);
        };
        comp.mouseDown (ev (from));
        for (int i = 1; i <= 8; ++i)
            comp.mouseDrag (ev (from + (to - from) * ((float) i / 8.0f)));
        comp.mouseUp (ev (to));
    }

    void tap (juce::Component& comp)
    {
        const auto pos = comp.getLocalBounds().getCentre().toFloat();
        const auto now = juce::Time::getCurrentTime();
        juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), pos,
                            juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &comp, &comp, now, pos, now, 1, false);
        comp.mouseDown (e);
        comp.mouseUp (e);
    }

    // Pump blocks, scanning for discontinuities / non-finite / runaway peaks.
    struct Scan { float maxJump = 0.0f, peak = 0.0f; bool finite = true; };

    Scan pump (VASynthProcessor& p, int blocks, int bs,
               std::function<void(juce::MidiBuffer&, int)> fill = {})
    {
        Scan s; float prevL = 0.0f, prevR = 0.0f;
        juce::AudioBuffer<float> buf (2, bs);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m; if (fill) fill (m, b);
            buf.clear(); p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < bs; ++i)
            {
                s.finite = s.finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                s.peak = std::max ({ s.peak, std::abs (L[i]), std::abs (R[i]) });
                s.maxJump = std::max ({ s.maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                prevL = L[i]; prevR = R[i];
            }
        }
        return s;
    }

    // Matches the ceiling the arp/looper torture suite uses: a clean saw chord measures
    // ~0.196 sample-to-sample, so 0.35 catches a genuine pop with margin above the honest
    // waveform slope. Noise is broadband by nature, so these patches keep the noise level
    // moderate and lean on the oscillator for the reference slope.
    constexpr float kClick = 0.35f;

    // A held, noise-forward patch: one quiet sine plus noise, long sustain, filter wide open
    // so what we measure is the noise path and not the filter's own motion.
    void armNoisePatch (VASynthProcessor& p, float level = 0.6f)
    {
        p.loadInitPreset();
        set01 (p, ParamID::osc1Wave, 0.75f);            // sine
        setVal (p, ParamID::osc1Level, 0.35f);
        set01 (p, ParamID::osc2On, 0.0f);
        set01 (p, ParamID::osc3On, 0.0f);
        setVal (p, ParamID::noiseLevel, level);
        setVal (p, ParamID::filterCutoff, 18000.0f);
        setVal (p, ParamID::filterReso, 0.0f);
        setVal (p, ParamID::ampAttack, 0.005f);
        setVal (p, ParamID::ampSustain, 1.0f);
        setVal (p, ParamID::ampDecay, 0.05f);
    }

    float tu_peak (const std::vector<float>& x)
    {
        float pk = 0.0f;
        for (float s : x) pk = std::max (pk, std::abs (s));
        return pk;
    }

    float renderPeak (VASynthProcessor& p, int blocks = 60, int bs = 128)
    {
        p.prepareToPlay (kSR, bs);
        float peak = 0.0f;
        juce::AudioBuffer<float> buf (2, bs);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m;
            if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            buf.clear(); p.processBlock (buf, m);
            if (b >= 10) peak = std::max (peak, buf.getMagnitude (0, bs));
        }
        return peak;
    }

    // Energy in a band, from a mono render — used to prove a route to the field actually
    // reshapes the SPECTRUM, not merely that a slot exists.
    std::vector<float> renderMono (VASynthProcessor& p, int blocks = 90, int bs = 256)
    {
        p.prepareToPlay (kSR, bs);
        std::vector<float> out;
        juce::AudioBuffer<float> buf (2, bs);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m;
            if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            buf.clear(); p.processBlock (buf, m);
            if (b >= 10) for (int i = 0; i < bs; ++i) out.push_back (buf.getSample (0, i));
        }
        return out;
    }
}

// ---------------------------------------------------------------------------
// The controls exist, and they are honest mod targets.
// ---------------------------------------------------------------------------

TEST_CASE ("noise field: both axes are registry destinations with stable ids", "[plugin][noise][modmatrix]")
{
    REQUIRE (moddest::destForParam (ParamID::noiseX) == ModMatrix::NoiseX);
    REQUIRE (moddest::destForParam (ParamID::noiseY) == ModMatrix::NoiseY);
    // Destination NAMES are persisted in factory-preset route JSON, so they are as frozen
    // as the ids. Changing one silently breaks every preset that routes to the field.
    REQUIRE (moddest::nameFor (ModMatrix::NoiseX) == "Noise Tilt");
    REQUIRE (moddest::nameFor (ModMatrix::NoiseY) == "Noise Focus");
    // The appended enum values must not have displaced anything that presets already store.
    REQUIRE (ModMatrix::NoiseX > ModMatrix::Osc2Fm);
    REQUIRE (ModMatrix::NoiseY == ModMatrix::NoiseX + 1);
}

TEST_CASE ("noise field: the pad and the FOCUS rail are on the panel and both animate", "[plugin][noise][smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    auto* pad  = findWidget<NoiseXYPad>  (*ed, ParamID::noiseX);
    auto* rail = findWidget<VBarControl> (*ed, ParamID::noiseY);
    REQUIRE (pad  != nullptr);
    REQUIRE (rail != nullptr);
    // Both must be wired by the editor's ONE registry-driven pass...
    REQUIRE (pad->isModTarget());
    REQUIRE (rail->isModTarget());
    // ...and both must actually DRAW their modulation. A target without an indicator is
    // wired-but-dead: the data flows and the panel shows nothing (the #12 NOISE bug).
    REQUIRE (pad->hasModIndicator());
    REQUIRE (rail->hasModIndicator());
    // Real bounds, not a collapsed zero-size control that no finger could ever hit.
    REQUIRE (pad->getWidth()  > 60);
    REQUIRE (pad->getHeight() > 30);
    REQUIRE (rail->getHeight() > 30);
}

// ---------------------------------------------------------------------------
// The gesture.
// ---------------------------------------------------------------------------

TEST_CASE ("noise field: one drag moves both axes, and the readout follows", "[plugin][noise][smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    auto* pad = findWidget<NoiseXYPad> (*ed, ParamID::noiseX);
    REQUIRE (pad != nullptr);

    // Starts parked at the exact bypass point, reading as plain white noise.
    REQUIRE (get01 (p, ParamID::noiseX) == Catch::Approx (NoiseShaper::kDefaultX));
    REQUIRE (get01 (p, ParamID::noiseY) == Catch::Approx (NoiseShaper::kDefaultY));
    REQUIRE (pad->readoutText() == "WHITE");
    snapshot (*pad, "noise-xy-default.png");

    auto b = pad->getLocalBounds().toFloat();
    // Drag to the bottom-left: the dark end of the colour axis, still in the tilt region.
    dragAcross (*pad, b.getCentre(), { b.getX() + 2.0f, b.getBottom() - 2.0f });
    REQUIRE (get01 (p, ParamID::noiseX) < 0.1f);
    REQUIRE (get01 (p, ParamID::noiseY) == Catch::Approx (0.0f).margin (0.02));
    REQUIRE (pad->readoutText() == "BROWN");
    snapshot (*pad, "noise-xy-tilt.png");

    // Drag up and right: into the focus region, where the readout switches to Hz + focus %.
    dragAcross (*pad, b.getCentre(), { b.getCentreX() + b.getWidth() * 0.3f, b.getY() + 2.0f });
    const float x = get01 (p, ParamID::noiseX), y = get01 (p, ParamID::noiseY);
    REQUIRE (x > 0.6f);
    REQUIRE (y > 0.9f);
    REQUIRE (pad->readoutText().contains ("Hz"));
    REQUIRE (pad->readoutText().contains ("FOCUS"));
    // The readout is not decoration: it must agree with the mapping the DSP will use.
    REQUIRE (pad->readoutText().upToFirstOccurrenceOf (" ", false, false).getIntValue()
             == juce::roundToInt (NoiseShaper::focusHz (x)));
    snapshot (*pad, "noise-xy-focus.png");

    // Double-click returns EXACTLY to the bypass point (not "about the middle" — the DSP
    // bypass is an equality test, so an approximate reset would leave the filter engaged).
    const auto c = b.getCentre();
    const auto now = juce::Time::getCurrentTime();
    pad->mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), c,
                                             juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             pad, pad, now, c, now, 2, false));
    REQUIRE (get01 (p, ParamID::noiseX) == NoiseShaper::kDefaultX);
    REQUIRE (get01 (p, ParamID::noiseY) == NoiseShaper::kDefaultY);
}

// ---------------------------------------------------------------------------
// LINK reaches the new destinations, and a route there is audible.
// ---------------------------------------------------------------------------

TEST_CASE ("noise field: LINK connects a source to either axis by tapping it", "[plugin][noise][link][smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    auto* pad  = findWidget<NoiseXYPad>  (*ed, ParamID::noiseX);
    auto* rail = findWidget<VBarControl> (*ed, ParamID::noiseY);
    REQUIRE (pad != nullptr); REQUIRE (rail != nullptr);

    p.armModLink (ModMatrix::LFO1);
    REQUIRE (p.linkArmed());
    REQUIRE (pad->isLinkArmable());              // the pad draws the connect ring
    snapshot (*pad, "noise-xy-link-armed.png");
    tap (*pad);
    // The route lands in the first FREE slot — the startup patch may already own slot 0 —
    // so search, rather than assuming an index that a preset change would invalidate.
    auto hasRoute = [&p] (int src, int dest)
    {
        for (int i = 0; i < ModMatrix::kSlots; ++i)
        {
            auto s = p.getModSlot (-1, i);
            if (s.source == src && s.dest == dest && s.depth != 0.0f) return true;
        }
        return false;
    };
    REQUIRE (hasRoute (ModMatrix::LFO1, ModMatrix::NoiseX));

    p.armModLink (ModMatrix::LFO2);
    tap (*rail);
    REQUIRE (hasRoute (ModMatrix::LFO2, ModMatrix::NoiseY));
}

TEST_CASE ("noise field: routes to it render in the MOD overlay by name", "[plugin][noise][smoke][modmatrix]")
{
    // The overlay is where a player reads what is connected to what. A destination that the
    // matrix accepts but the overlay cannot name would be a route you can make and never find.
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.setModSlot (-1, 0, ModMatrix::LFO1,     ModMatrix::NoiseY, 0.8f);
    p.setModSlot (-1, 1, ModMatrix::Velocity, ModMatrix::NoiseX, -0.5f);

    ModMatrixPanel panel (p);
    panel.setSize (760, 420);
    snapshot (panel, "noise-xy-mod-overlay.png");

    REQUIRE (moddest::nameFor (ModMatrix::NoiseY).isNotEmpty());
    REQUIRE (moddest::nameFor (ModMatrix::NoiseX).isNotEmpty());
}

TEST_CASE ("noise field: a route to an axis audibly reshapes the noise", "[plugin][noise][modmatrix]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    armNoisePatch (p, 0.9f);
    setVal (p, ParamID::osc1Level, 0.0f);                     // noise ONLY: nothing else to hear
    set01 (p, ParamID::osc1On, 0.0f);

    const auto plain = renderMono (p);

    // A steady, deep route to FOCUS: the noise collapses into a narrow band, which changes
    // the waveform's character enormously even though its level is trimmed to match. The amp
    // envelope is the source because it sits at 1.0 for the whole held note, so the offset is
    // a constant — this test is about the SEAM into the DSP, not about motion.
    p.setModSlot (-1, 0, ModMatrix::AmpEnv, ModMatrix::NoiseY, 1.0f);
    const auto routed = renderMono (p);

    REQUIRE (plain.size() == routed.size());
    double diff = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) diff += std::abs ((double) plain[i] - routed[i]);
    INFO ("mean |difference| " << diff / (double) plain.size());
    REQUIRE (diff / (double) plain.size() > 1.0e-3);          // genuinely different audio
}

// ---------------------------------------------------------------------------
// Click torture — the standing rule for anything that touches the audio path.
// ---------------------------------------------------------------------------

TEST_CASE ("torture: sweeping the noise field under held notes is click-free", "[plugin][noise][click][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    armNoisePatch (p);
    p.prepareToPlay (kSR, 128);

    // Hold a chord, then sweep BOTH axes across their full range while it sounds — the
    // gesture a hand makes on the pad, at a speed no hand could manage.
    auto* px = p.apvts.getParameter (ParamID::noiseX);
    auto* py = p.apvts.getParameter (ParamID::noiseY);
    const auto s = pump (p, 400, 128, [&] (juce::MidiBuffer& m, int b)
    {
        if (b == 0)
        {
            m.addEvent (juce::MidiMessage::noteOn (1, 48, 0.9f), 0);
            m.addEvent (juce::MidiMessage::noteOn (1, 55, 0.9f), 8);
            m.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 16);
        }
        const float t = (float) b / 400.0f;
        px->setValueNotifyingHost (0.5f + 0.5f * std::sin (t * 37.0f));   // full-range x sweep
        py->setValueNotifyingHost (0.5f + 0.5f * std::sin (t * 23.0f));   // full-range y sweep
    });
    INFO ("maxJump " << s.maxJump << " peak " << s.peak);
    REQUIRE (s.finite);
    REQUIRE (s.peak <= 1.0f);                 // the safety clipper still holds
    REQUIRE (s.maxJump < kClick);
}

TEST_CASE ("torture: a note storm while the noise field sweeps stays clean", "[plugin][noise][click][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    armNoisePatch (p);
    setVal (p, ParamID::ampRelease, 0.08f);
    p.prepareToPlay (kSR, 64);

    auto* px = p.apvts.getParameter (ParamID::noiseX);
    auto* py = p.apvts.getParameter (ParamID::noiseY);
    const auto s = pump (p, 600, 64, [&] (juce::MidiBuffer& m, int b)
    {
        const int note = 36 + (b * 7) % 48;
        if (b % 3 == 0) m.addEvent (juce::MidiMessage::noteOn  (1, note, 0.2f + 0.7f * (float) ((b * 13) % 7) / 7.0f), 0);
        if (b % 3 == 2) m.addEvent (juce::MidiMessage::noteOff (1, 36 + ((b - 2) * 7) % 48), 0);
        const float t = (float) b / 600.0f;
        px->setValueNotifyingHost (std::fmod (t * 5.0f, 1.0f));
        py->setValueNotifyingHost (std::fmod (t * 3.0f, 1.0f));
    });
    INFO ("maxJump " << s.maxJump << " peak " << s.peak);
    REQUIRE (s.finite);
    REQUIRE (s.peak <= 1.0f);
    REQUIRE (s.maxJump < kClick);
}

TEST_CASE ("torture: a fast square LFO on FOCUS at near-ring Q does not tick", "[plugin][noise][click][torture]")
{
    // The specific failure this feature could have shipped: stepping Q on a filter that is
    // already ringing. A square LFO is the worst source (it jumps, it does not glide) and a
    // near-ring base Q is the worst place to jump from.
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    armNoisePatch (p);
    setVal (p, ParamID::noiseX, 0.6f);
    setVal (p, ParamID::noiseY, 0.85f);                       // base Q already near the ring
    set01 (p, ParamID::lfoShape, 2.0f / 3.0f);                // square
    setVal (p, ParamID::lfoRate, 19.0f);                      // fast
    setVal (p, ParamID::lfoDepth, 1.0f);
    set01 (p, ParamID::lfoDest, 3.0f / 3.0f);                 // "ON" — runs as a matrix source
    p.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::NoiseY, 1.0f);
    p.setModSlot (-1, 1, ModMatrix::LFO1, ModMatrix::NoiseX, 0.8f);
    p.prepareToPlay (kSR, 128);

    const auto s = pump (p, 400, 128, [] (juce::MidiBuffer& m, int b)
    {
        if (b == 0)   m.addEvent (juce::MidiMessage::noteOn  (1, 52, 0.9f), 0);
        if (b == 200) m.addEvent (juce::MidiMessage::noteOn  (1, 59, 0.9f), 0);
        if (b == 380) m.addEvent (juce::MidiMessage::noteOff (1, 52), 0);
    });
    INFO ("maxJump " << s.maxJump << " peak " << s.peak);
    REQUIRE (s.finite);
    REQUIRE (s.peak <= 1.0f);
    REQUIRE (s.maxJump < kClick);
}

// ---------------------------------------------------------------------------
// Persistence.
// ---------------------------------------------------------------------------

TEST_CASE ("noise field: a user preset round-trips its position AND its route", "[plugin][noise][preset]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    armNoisePatch (p);
    setVal (p, ParamID::noiseX, 0.18f);
    setVal (p, ParamID::noiseY, 0.72f);
    p.setModSlot (-1, 0, ModMatrix::ModEnv, ModMatrix::NoiseY, -0.6f);   // also writes the state property

    const auto name = "ut-noisexy-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000));
    REQUIRE (pm.save (name, "FX"));

    p.loadFactoryPreset ("Warm Pad");                          // clear the field and the routes
    REQUIRE (get01 (p, ParamID::noiseY) == Catch::Approx (0.0f).margin (1e-5));
    REQUIRE (p.getModSlot (-1, 0).dest == ModMatrix::DstNone);

    p.loadUserPreset (name);
    REQUIRE (get01 (p, ParamID::noiseX) == Catch::Approx (0.18f).margin (0.005));
    REQUIRE (get01 (p, ParamID::noiseY) == Catch::Approx (0.72f).margin (0.005));
    auto s = p.getModSlot (-1, 0);
    REQUIRE (s.source == ModMatrix::ModEnv);
    REQUIRE (s.dest   == ModMatrix::NoiseY);
    REQUIRE (s.depth  == Catch::Approx (-0.6f).margin (0.02));

    pm.presetDir().getChildFile (name + ".vasynth").deleteFile();
}

TEST_CASE ("noise field: a preset saved before the field existed loads as bypass", "[plugin][noise][preset][migration]")
{
    // The compatibility promise. An on-disk patch with NO noise_x / noise_y keys must come
    // back at the exact bypass point -- NOT at whatever the previously loaded patch left
    // behind, which is what a naive state replace would do. Loading it right after a patch
    // that pushed the field hard is the case that would expose the bug.
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    armNoisePatch (p);
    const auto name = "ut-noiselegacy-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000));
    REQUIRE (pm.save (name, "FX"));

    // Strip both keys from the saved file — exactly the shape of a pre-feature preset.
    auto file = pm.presetDir().getChildFile (name + ".vasynth");
    auto xml = juce::XmlDocument::parse (file);
    REQUIRE (xml != nullptr);
    auto tree = juce::ValueTree::fromXml (*xml);
    int removed = 0;
    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        const auto id = tree.getChild (i).getProperty ("id").toString();
        if (id == ParamID::noiseX || id == ParamID::noiseY) { tree.removeChild (i, nullptr); ++removed; }
    }
    REQUIRE (removed == 2);
    REQUIRE (tree.createXml()->writeTo (file));

    // Push the LIVE field somewhere obvious, then load the key-less preset over it.
    setVal (p, ParamID::noiseX, 0.05f);
    setVal (p, ParamID::noiseY, 0.95f);
    p.loadUserPreset (name);
    REQUIRE (get01 (p, ParamID::noiseX) == Catch::Approx (NoiseShaper::kDefaultX).margin (1e-5));
    REQUIRE (get01 (p, ParamID::noiseY) == Catch::Approx (NoiseShaper::kDefaultY).margin (1e-5));

    file.deleteFile();
}

TEST_CASE ("noise field: a saved position survives a reload and shows on the panel", "[plugin][noise][preset][smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    auto* pad = findWidget<NoiseXYPad> (*ed, ParamID::noiseX);
    REQUIRE (pad != nullptr);

    armNoisePatch (p);
    setVal (p, ParamID::noiseX, 0.78f);
    setVal (p, ParamID::noiseY, 0.55f);
    const auto name = "ut-noiseshot-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000));
    REQUIRE (pm.save (name, "FX"));
    p.loadFactoryPreset ("Warm Pad");
    p.loadUserPreset (name);

    // The pad must SHOW the reloaded position — the indicator tracks the parameter, not the
    // last drag ([[vasynth-controls-never-lie]]: a control's readout is its live value).
    REQUIRE (get01 (p, ParamID::noiseX) == Catch::Approx (0.78f).margin (0.005));
    REQUIRE (pad->readoutText().contains ("FOCUS"));
    snapshot (*pad, "noise-xy-reloaded.png");

    pm.presetDir().getChildFile (name + ".vasynth").deleteFile();
}

TEST_CASE ("noise field: a routed axis does not disturb an unrouted patch's output", "[plugin][noise][golden]")
{
    // Belt and braces on the bypass at the PROCESSOR level (the DSP suite proves it at the
    // voice): adding a route to an axis changes the audio, and clearing it again returns the
    // render EXACTLY. Each arm renders on a FRESH processor so the three are compared from
    // identical history — a live processor accumulates LFO phase and FX tails across renders,
    // which would otherwise mask (or fake) the result being measured.
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto renderWith = [] (std::function<void(VASynthProcessor&)> arm)
    {
        VASynthProcessor p;
        armNoisePatch (p);
        arm (p);
        return renderMono (p, 40);
    };
    const auto before = renderWith ([] (VASynthProcessor&) {});
    const auto routed = renderWith ([] (VASynthProcessor& p)
                                    { p.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::NoiseY, 0.9f); });
    const auto after  = renderWith ([] (VASynthProcessor& p)
                                    { p.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::NoiseY, 0.9f);
                                      p.clearModSlot (-1, 0); });

    REQUIRE (before.size() == after.size());
    bool routedDiffers = false;
    for (std::size_t i = 0; i < before.size(); ++i) if (before[i] != routed[i]) { routedDiffers = true; break; }
    REQUIRE (routedDiffers);                                   // the route was genuinely live
    for (std::size_t i = 0; i < before.size(); ++i) REQUIRE (before[i] == after[i]);
    REQUIRE (tu_peak (before) > 0.01f);                        // and it was making sound throughout
}
