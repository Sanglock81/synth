#pragma once
#include <juce_core/juce_core.h>

// ============================================================================
// UTF-8 string convention (see the mojibake defect, #56 follow-up).
//
// CONVENTION: juce::String's `const char*` constructor decodes as ASCII/Latin-1,
// NOT UTF-8 (juce_String.cpp: `CharPointer_ASCII`). So a literal like
// "\xe2\x80\x94" (an em-dash in UTF-8) becomes THREE garbage codepoints
// (0xE2 -> "â", ...). In a Debug build a jassert fires; in RELEASE it is
// stripped, so the mojibake ships silently — exactly how the MOD overlay's
// arrows/dashes turned into "â".
//
// RULE: any string literal containing a byte > 0x7f MUST be wrapped in u8(),
// which routes it through CharPointer_UTF8 and decodes correctly. Prefer
// DRAWING decorative glyphs (arrows, the remove ✕) as juce::Path instead of
// text — no font-glyph dependency and no encoding hazard at all. Reserve u8()
// for genuine text (a real em-dash label, degree/times signs, accented names).
// grep for `\x` escapes in Source/UI to audit.
// ============================================================================
namespace uitext
{
    inline juce::String u8 (const char* utf8Literal) { return juce::String::fromUTF8 (utf8Literal); }
}
