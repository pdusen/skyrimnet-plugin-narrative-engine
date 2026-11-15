#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Fitting the game's name for a made thing to the sentence it is put in.
//
// A made thing reaches a template as the game names it: bare and in the
// nominative ("Daedrischer Bogen", "Стальная булава", "Flèche elfique").
// The language's [Grammar] says what its slots need instead, as lists:
//
//   Item.Gender.<class>   the words that decide which article a name takes;
//                         the first word of a name any list knows decides,
//                         and its class becomes `{gender}` in [Plot.Item]
//   Item.Case.Modifier    rewrites, read from a name's first word, of the
//                         words before its head ("*ая > *ую"); each one
//                         matched is rewritten and the next word read
//   Item.Case.Head        rewrites of the head itself ("*а > *у"); the word
//                         is rewritten and reading stops
//   Item.Plural           rewrites of every word of ammunition's name
//                         ("*pfeil > *pfeile")
//
// An entry is a whole word (`hacha`) or, with `*`, an ending (`*axt`),
// compared case-folded. The longest match wins, and a whole word beats any
// ending. A word's trailing punctuation is set aside while it is matched
// ("Mikstura:"), and a rewritten word keeps its capital.
//
// Reading the case rules stops at the first word neither list matches, so a
// genitive after the head ("Яд нетча") is never declined, and a name whose
// first word needs nothing (a masculine one, in Russian) is left alone.
//
// Pure: no engine, no files. LanguageTable builds the rules a language has.
namespace NarrativeEngine::ItemGrammar
{
    // A whole word or an ending, folded for matching.
    struct Pattern
    {
        std::string folded;
        // How many code points the ending is, as written: what a match cuts
        // from the end of a word.
        std::size_t length = 0;
        bool ending = false;
    };

    struct Rewrite
    {
        Pattern from;
        // The replacement as written, without its `*`.
        std::string to;
    };

    struct Rules
    {
        // Every Item.Gender entry with its class, in file order.
        std::vector<std::pair<Pattern, std::string>> classes;
        std::vector<Rewrite> caseModifier;
        std::vector<Rewrite> caseHead;
        std::vector<Rewrite> plural;
    };

    // An Item.Gender list, or nullopt when an entry is not a word or an
    // ending ("*", "a*b").
    [[nodiscard]] std::optional<std::vector<Pattern>> ParsePatterns(std::string_view value);

    // An Item.Case or Item.Plural list, or nullopt when an entry is not
    // `from > to` with a `*` on both sides or neither.
    [[nodiscard]] std::optional<std::vector<Rewrite>> ParseRewrites(std::string_view value);

    struct Fitted
    {
        std::string name;
        // The Item.Gender class that decided, or "other".
        std::string gender;
    };

    // `name` as a made thing's slot says it, or as ammunition when `ammo`.
    // With Item.Gender lists that know no word of it, it comes back as it
    // is, as `other`: a modded thing reads no worse than the game's name.
    [[nodiscard]] Fitted Fit(const Rules& rules, std::string_view name, bool ammo);
} // namespace NarrativeEngine::ItemGrammar
