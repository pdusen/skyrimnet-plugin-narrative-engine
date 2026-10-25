#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Characters, not bytes.
//
// Everything that looks at the letters of a string -- its case, where its
// words begin and end, how long it is -- goes through here, because the
// text it looks at is in the player's language. `std::tolower`,
// `std::isalpha` and friends classify bytes in the "C" locale, so every byte
// of a Cyrillic or Japanese letter reads to them as "not a letter": a name
// in either splits into no words at all. Case and categories come from
// utf8proc, which covers all of Unicode.
//
// Input is UTF-8. A byte that does not begin a well-formed sequence decodes
// as U+FFFD, is never a letter or a word character, and counts as one code
// point. The case mappings copy it through unchanged and Fold replaces it;
// only RepairWindows1252 turns it back into the character it meant.
namespace NarrativeEngine::TextUtf8
{
    // One code point read out of a string: the value, and how many bytes it
    // took. A malformed byte reads as U+FFFD with a length of 1.
    struct Decoded
    {
        char32_t codePoint = 0;
        std::size_t length = 0;
    };

    // The length of the well-formed UTF-8 sequence starting at `at`, or 0
    // when it is not one. The ranges are Unicode's table of well-formed
    // sequences: no overlongs, no surrogates, nothing past U+10FFFF.
    [[nodiscard]] std::size_t SequenceLength(std::string_view text, std::size_t at) noexcept;

    // The code point at byte `at`. `at` must be inside `text`.
    [[nodiscard]] Decoded DecodeAt(std::string_view text, std::size_t at) noexcept;

    // Whether every byte of `text` belongs to a well-formed sequence.
    [[nodiscard]] bool IsValid(std::string_view text) noexcept;

    void AppendCodePoint(std::string& out, char32_t codePoint);

    // Valid UTF-8 comes back untouched. Otherwise every byte that does not
    // begin a valid UTF-8 sequence is read as Windows-1252 (0x80-0x9F through
    // its table, the rest as Latin-1) and re-encoded; valid sequences around
    // it are kept. Non-localized plugins written by ANSI tools carry such
    // bytes, and the engine hands them back unchanged.
    [[nodiscard]] std::string RepairWindows1252(std::string_view text);

    // How many code points `text` holds, a malformed byte counting as one.
    [[nodiscard]] std::size_t CodePointCount(std::string_view text) noexcept;

    // Shorten `text` to at most `maxCodePoints` code points. The cut always
    // lands on a code-point boundary. No-op when the string already fits.
    void TruncateToCodePoints(std::string& text, std::size_t maxCodePoints);

    // Simple (one-to-one) case mapping of every code point. Caseless
    // scripts, digits and punctuation pass through.
    [[nodiscard]] std::string ToLower(std::string_view text);
    [[nodiscard]] std::string ToUpper(std::string_view text);

    // The same mapping applied to the first code point only. Nothing else in
    // the string changes, so "the amulet" becomes "The amulet" and "Élise"
    // becomes "élise".
    [[nodiscard]] std::string UppercaseFirst(std::string_view text);
    [[nodiscard]] std::string LowercaseFirst(std::string_view text);

    // Full Unicode case folding, NFC-composed, for comparing two strings
    // without regard to case: "Straße" and "STRASSE" fold to the same thing,
    // as do a precomposed "é" and an "e" with a combining accent. The result
    // is for comparison only; never show it to anyone.
    [[nodiscard]] std::string Fold(std::string_view text);
    [[nodiscard]] bool EqualsFolded(std::string_view a, std::string_view b);

    [[nodiscard]] bool IsLetter(char32_t codePoint) noexcept;
    [[nodiscard]] bool IsUpper(char32_t codePoint) noexcept;
    [[nodiscard]] bool IsLower(char32_t codePoint) noexcept;

    // Letters, combining marks and digits, plus the hyphen and the
    // apostrophe, so that "Snow-Shod" and "Ri'saad" are one word each.
    [[nodiscard]] bool IsWordChar(char32_t codePoint) noexcept;

    // The words of `text`, in order: each a maximal run of word characters,
    // with any hyphens and apostrophes at either end trimmed off. The views
    // point into `text`. A script written without spaces comes back as one
    // word per run, which is all a splitter can honestly say about it.
    [[nodiscard]] std::vector<std::string_view> Words(std::string_view text);

    // How an apostrophe straight after a match is read when deciding whether
    // the match stands alone.
    enum class TrailingApostrophe
    {
        // Part of the word: "O'" in "O'Ryan" continues it, so "O" does not
        // stand alone there.
        PartOfWord,
        // Ends the word: "Veleth" stands alone in "Veleth's movements".
        EndsWord,
    };

    // Whether the bytes [at, at + length) of `text` form a whole word rather
    // than a fragment of a longer one: no word character touches the match
    // on either side. A leading apostrophe always continues a word (an
    // O'Ryan is not a Ryan); a trailing one does as `trailing` says.
    [[nodiscard]] bool StandsAlone(std::string_view text,
                                   std::size_t at,
                                   std::size_t length,
                                   TrailingApostrophe trailing = TrailingApostrophe::PartOfWord) noexcept;
} // namespace NarrativeEngine::TextUtf8
