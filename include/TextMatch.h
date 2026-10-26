#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The shared vocabulary every content check is written in, per language.
//
// A check that reads what a model wrote -- does a scheme open like an
// instruction, does a name say "ghost", does a finish name the player -- is
// English twice over: in its words, and in where it looks for them. So each is
// written in the language file's [Matching] section as lists keyed by
// position, and this module evaluates them. See "Validators that read content"
// in docs/implementation/PHASE_18_INTERNATIONALIZATION.md.
//
//   <Check>.Reject.Start    passes when the text does not begin with an entry
//   <Check>.Reject.End      passes when it does not end with one
//   <Check>.Require.Start   passes when it begins with one, if any are listed
//   <Check>.Require.End     passes when it ends with one, if any are listed
//   <Check>.Match.Start     a detector: fires when the text begins with one
//   <Check>.Match.End       ... ends with one
//   <Check>.Match.Anywhere  ... holds one anywhere
//   <Check>.Disabled        the check is off in this language, and why
//
// A list is comma-separated. Each entry is a sequence of word patterns
// separated by spaces, and `*` in a pattern stands for any letters, so `* is`
// is "any word, then is", and `drachenblut*` matches "Drachenbluts". Text and
// entries are compared case-folded. In a language written without spaces
// between words ([Grammar] WordSeparated = false), each entry is instead a run
// of characters, matched at its position in the text with the punctuation and
// spaces at either end set aside.
//
// Any other [Matching] key is a plain list read by name: `Title.Strip.Start`,
// `Name.Suffixes`.
//
// Pure: no engine, no files. LanguageTable builds the rules a language has.
namespace NarrativeEngine::TextMatch
{
    // One entry: its word patterns, folded. A single pattern when words are
    // not separated.
    struct Entry
    {
        std::vector<std::string> patterns;
    };

    using List = std::vector<Entry>;

    enum class Position
    {
        Start,
        End,
        Anywhere,
    };

    struct Options
    {
        bool wordSeparated = true;
    };

    // The fewest code points a part of a person's name needs before a
    // matcher counts it as naming them. In a script written with spaces that
    // is the matcher's own floor, `separated`, set against the short words
    // that recur in sentences about nobody ("Al", "of"). In one written
    // without (Japanese, Chinese) a part is found as a run of characters, and
    // two already make a whole given name (紮格, フルダ) and a run too
    // particular to occur by chance; one character is a syllable or an
    // everyday word, and is evidence of nobody.
    [[nodiscard]] constexpr std::size_t ShortestNamePart(const Options& options, std::size_t separated) noexcept
    {
        return options.wordSeparated ? separated : 2;
    }

    // A [Matching] value, split on commas and trimmed: what a plain list holds.
    [[nodiscard]] std::vector<std::string> Items(std::string_view value);

    // A [Matching] value as entries.
    [[nodiscard]] List Parse(std::string_view value, const Options& options);

    // Whether any entry of `list` matches `text` at `position`.
    [[nodiscard]] bool Matches(std::string_view text, const List& list, Position position, const Options& options);

    struct Check
    {
        List rejectStart;
        List rejectEnd;
        List requireStart;
        List requireEnd;
        List matchStart;
        List matchEnd;
        List matchAnywhere;
        // Non-empty when the check is off in this language: the file's reason.
        std::string disabled;

        // Whether the file says anything of this check: a rule, or a reason
        // it has none.
        [[nodiscard]] bool Defined() const noexcept;

        // For a gate: nothing rejected, and every require list that has
        // entries satisfied. A disabled check passes everything.
        [[nodiscard]] bool Passes(std::string_view text, const Options& options) const;

        // For a detector: some match list matches. A disabled check never
        // fires.
        [[nodiscard]] bool Fires(std::string_view text, const Options& options) const;
    };

    // The suffix that makes a [Matching] key part of a check, and the check's
    // name, or an empty name for a plain list key.
    [[nodiscard]] std::string_view CheckName(std::string_view key) noexcept;

    // Whether `word`, a word of some text, is `name`: bare, or carrying one
    // of `suffixes`, a language's Name.Suffixes ("Varian's", "Ульфрика").
    // With `foldCase`, case does not matter.
    [[nodiscard]] bool IsNameWord(std::string_view word,
                                  std::string_view name,
                                  const std::vector<std::string>& suffixes,
                                  bool foldCase = false);

    // Whether `word` reads as the start of a name: a capital, in a script
    // with case; any letter, in one without.
    [[nodiscard]] bool StartsName(std::string_view word);

    struct Rules
    {
        Options options;
        std::map<std::string, Check, std::less<>> checks;
        // Plain lists, by key, as written.
        std::map<std::string, std::string, std::less<>> lists;

        // The check of that name, or null when the language says nothing of
        // it.
        [[nodiscard]] const Check* Find(std::string_view name) const;

        // A plain list's items; empty when there is none.
        [[nodiscard]] std::vector<std::string> List(std::string_view key) const;

        // Set one [Matching] key's value: a check's list or reason, or a plain
        // list.
        void Set(std::string_view key, std::string_view value);
    };
} // namespace NarrativeEngine::TextMatch
