// ============================================================================
// Section guide (#133) coverage — institutionalizes the noise_level lesson: every
// param-attached control in a COVERED section must have a guide entry, and every
// guide entry must reference a real control. A future control added to a covered
// section without help text fails this gate.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UI/GuideContent.h"
#include <set>
#include <memory>

namespace
{
    // Normalize a control's param id to its documentation "class": repeated rows (3 oscillators,
    // 3 LFOs, 4 parts, 4 looper lanes, amp/mod ADSR, 8 macros) are documented ONCE via a
    // representative, so the guide entry for that class covers all of them.
    juce::String klass (juce::String id)
    {
        if (id.startsWith ("osc1_") || id.startsWith ("osc2_") || id.startsWith ("osc3_"))
        {
            id = id.substring (5);
            if (id == "wt_pos") id = "pw";                       // PW + WT POS share the slot + entry
            return id;
        }
        if (id.startsWith ("part") && id.length() > 6 && juce::CharacterFunctions::isDigit (id[4]))
            return "part_" + id.substring (6);                   // part0_level -> part_level
        if (id.startsWith ("macro")) return "macro";             // macro1..8 -> macro
        for (auto* stg : { "attack", "decay", "sustain", "release" })
            if (id == "amp_" + juce::String (stg) || id == "flt_" + juce::String (stg)) return stg;   // ADSR
        if (id.startsWith ("lfo2_") || id.startsWith ("lfo3_")) return "lfo_" + id.substring (5);
        if (id.startsWith ("loop_"))                             // looper lanes 2-4 -> lane 1
        {
            const juce::juce_wchar c = id.getLastCharacter();
            if (c >= '2' && c <= '4') return id.dropLastCharacters (1);
        }
        return id;
    }
}

TEST_CASE ("section guide: entries reference real controls; covered sections cover every control", "[plugin][guide][coverage]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);                       // full recursive layout so controls exist
    auto* ve = dynamic_cast<VASynthEditor*> (ed.get());
    REQUIRE (ve != nullptr);

    const auto& secs = guide::sections();
    int covered = 0;
    for (int i = 0; i < (int) secs.size(); ++i)
    {
        const auto& sec = secs[(std::size_t) i];

        // (a) every entry references a real APVTS parameter (no dangling controlIds).
        for (auto& e : sec.entries)
            if (juce::String (e.controlId).isNotEmpty())
            {
                INFO ("section '" << sec.id << "' entry '" << e.name << "' controlId=" << e.controlId);
                REQUIRE (p.apvts.getParameter (e.controlId) != nullptr);
            }

        if (! sec.covered) continue;
        ++covered;
        REQUIRE_FALSE (sec.entries.empty());

        // (b) every param-attached control CLASS in the section has a guide entry.
        std::set<juce::String> documented;
        for (auto& e : sec.entries)
            if (juce::String (e.controlId).isNotEmpty()) documented.insert (klass (e.controlId));

        for (auto& id : ve->sectionControlParamIds (i))
        {
            INFO ("section '" << sec.id << "' control '" << id << "' (class '" << klass (id) << "') has no guide entry");
            REQUIRE (documented.count (klass (id)) == 1);
        }
    }
    REQUIRE (covered >= 1);   // at least Oscillators is authored + enforced
}

// Export the same guide data to docs/guide.md (the reviewable, PR-diffable form) -- generated, never
// hand-edited. Mirrors how the smoke tests regenerate their screenshots.
TEST_CASE ("section guide: generate docs/guide.md from the content", "[plugin][guide][docs]")
{
    juce::String md;
    md << "# Section guide\n\n"
       << "Generated from `Source/UI/GuideContent.h` -- the in-app `?` -> section reference "
          "(spotlight + numbered markers + card). Do not edit by hand; edit the content header + rerun "
          "the guide test.\n";
    for (auto& sec : guide::sections())
    {
        md << "\n## " << sec.title << "\n\n" << sec.intro << "\n\n";
        int n = 1;
        for (auto& e : sec.entries)
            md << n++ << ". **" << e.name << "** - " << e.what << " _" << e.how << "_\n";
    }
   #ifdef VASYNTH_DOCS_DIR
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/guide.md");
    REQUIRE (out.replaceWithText (md));
    REQUIRE (out.existsAsFile());
   #endif
}
