#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Sanitize free-form strings returned by an LLM before they're saved,
// rendered, persisted, or fed into any downstream subsystem.
//
// Why: LLMs routinely return typographic / invisible / "smart" Unicode
// characters (curly quotes, em-dashes, ellipses, NBSPs, zero-width
// formatting marks) that look fine in a chat client but cause visible-text
// problems in Skyrim — missing glyphs in the bundled fonts, silent
// truncation in ASCII-only engine fields, garbled co-save payloads. The
// fix is a single canonical pass that transforms a fixed list of known-bad
// codepoints to ASCII equivalents (or drops them) and trims surrounding
// whitespace.
//
// **Codepoints not on the transform list pass through verbatim.** That
// includes accented Latin letters (à, é, ñ, ö, ü), Latin ligatures (æ, œ,
// ß), language-native punctuation (« », U+2015 horizontal bar), and any
// other script — Cyrillic, Greek, Hebrew, Arabic, CJK — so players in
// non-English locales keep their letters intact. We only filter the
// specific LLM artifacts; we don't romanize the text.
//
// See `docs/LLM_RESPONSE_HANDLING.md` for the substitution table and the
// project-wide rule that EVERY LLM-derived string MUST pass through this
// function at the point of extraction from the response JSON, before being
// stored or used elsewhere.
namespace NarrativeEngine::LLMTextSanitizer
{
    // Apply the canonical sanitization pass. Input is treated as UTF-8.
    // Output is UTF-8: ASCII bytes for codepoints on the transform list,
    // original bytes preserved for everything else. Leading and trailing
    // whitespace is removed.
    //
    // The function never throws. Invalid UTF-8 sequences are dropped at
    // the offending byte (one byte consumed, no output emitted).
    std::string Sanitize(std::string_view input);

    // Shorten `text` to at most `maxBytes` bytes without splitting a
    // UTF-8 character. No-op when the string already fits.
    //
    // Why this exists: `Sanitize` only rewrites the typographic-noise
    // codepoints and passes every other script through verbatim, so its
    // output is UTF-8 but NOT necessarily ASCII. A plain
    // `s.resize(200)` therefore cuts mid-character for any non-Latin
    // player -- Cyrillic and Greek are 2 bytes per letter, CJK 3 --
    // leaving a dangling lead byte. nlohmann::json refuses to serialize
    // invalid UTF-8, so the corrupt string throws type_error.316 out of
    // every later `dump()` that touches it, not at the truncation site.
    // Use this for EVERY byte-budget clamp applied to LLM-derived text.
    //
    // The cut moves backwards to the nearest character boundary, so the
    // result can be up to 3 bytes shorter than `maxBytes`. Never throws.
    void TruncateUTF8(std::string& text, std::size_t maxBytes);
} // namespace NarrativeEngine::LLMTextSanitizer
