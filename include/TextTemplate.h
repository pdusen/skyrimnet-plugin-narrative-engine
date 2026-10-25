#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The formatter every language-file template renders through.
//
// The syntax is a small subset of ICU MessageFormat, so a translator who has
// localized software before already knows it:
//
//   {agent}                                     the argument's text; a person's name
//   {agent, select, female {...} male {...} other {...}}
//                                               on a person, their sex; on text, its value
//   {days, plural, one {# day} other {# days}}  the CLDR plural category; # is the number
//   {names, list}                               a list, joined with the language's separators
//   {scheme, phrase}                            text trimmed to sit inside a sentence
//
// As in ICU, `#` is the number only directly inside a plural's own branch: in
// a select nested inside one it is a literal `#`, and the number is `{days}`.
//
// There is no escaping. A brace that does not open or close a placeholder is
// a parse error, and so is anything else this subset does not define; the
// caller falls back to English rather than rendering a half-understood
// template. An apostrophe is always literal ("Veleth's"), unlike in full ICU.
//
// Pure: no engine, no files. The language's rules arrive as a Grammar.
namespace NarrativeEngine::TextTemplate
{
    enum class Sex
    {
        Male,
        Female,
        Other,
    };

    struct Person
    {
        std::string name;
        Sex sex = Sex::Other;
    };

    using List = std::vector<std::string>;
    using Value = std::variant<std::string, Person, std::int64_t, List>;

    // What kind of value a placeholder takes, for validation.
    enum class Kind
    {
        Text,
        Person,
        Number,
        List,
    };

    // CLDR's cardinal plural categories.
    enum class PluralCategory
    {
        Zero,
        One,
        Two,
        Few,
        Many,
        Other,
    };

    // The rule families the shipped languages need. CLDR's rules are
    // arithmetic, so they live here rather than in a file: a language picks
    // the family it belongs to.
    enum class PluralRule
    {
        None,     // Japanese, Chinese: everything is `other`
        OneOther, // English, German, Italian, Spanish: 1 is `one`
        French,   // 0 and 1 are `one`
        Russian,  // one / few / many by the last two digits
        Polish,   // 1 is `one`; few / many by the last two digits
    };

    [[nodiscard]] PluralCategory Plural(PluralRule rule, std::int64_t n) noexcept;

    // The parts of a language that templates cannot say for themselves.
    struct Grammar
    {
        PluralRule pluralRule = PluralRule::OneOther;

        // Joining a list: two items take `listPairSeparator`; three or more
        // take `listSeparator` between all but the last two, and
        // `listFinalSeparator` before the last.
        std::string listSeparator = ", ";
        std::string listPairSeparator = " and ";
        std::string listFinalSeparator = ", and ";

        // A sentence's first letter is upper-cased, because a sentence can
        // open with an unfilled slot that reads "someone".
        bool capitalizeSentenceStart = true;

        // `phrase`: whitespace is trimmed from both ends, and these from the
        // end; then the first letter is lower-cased if the language does that
        // to a clause it moves into the middle of a sentence.
        std::string phraseTrailingPunctuation = ".!?;,";
        bool phraseLowercaseFirst = true;
    };

    struct Node;
    using Message = std::vector<Node>;

    struct Branch
    {
        std::string key;
        Message message;
    };

    struct Placeholder
    {
        enum class Format
        {
            Simple,
            Select,
            Plural,
            List,
            Phrase,
        };

        std::string name;
        Format format = Format::Simple;
        std::vector<Branch> branches; // Select and Plural only
    };

    // `#` inside a plural branch: the number being pluralized.
    struct PluralNumber
    {};

    struct Node
    {
        std::variant<std::string, Placeholder, PluralNumber> value;
    };

    // A parsed template, or why it could not be parsed.
    struct Parsed
    {
        Message message;
        std::vector<std::string> problems;

        [[nodiscard]] bool Ok() const noexcept
        {
            return problems.empty();
        }
    };

    [[nodiscard]] Parsed Parse(std::string_view text);

    // The placeholders a template may use, and what each one takes.
    struct Parameter
    {
        std::string name;
        Kind kind = Kind::Text;
    };
    using Signature = std::vector<Parameter>;

    // Every way `message` misuses `signature`: a placeholder it does not
    // declare, a format the parameter's kind cannot take, a select or plural
    // with no `other` branch, or a plural key CLDR does not have. Empty
    // when the template is sound.
    [[nodiscard]] std::vector<std::string> Validate(const Message& message, const Signature& signature);

    // Every placeholder name `message` uses, in order of first use.
    [[nodiscard]] std::vector<std::string> PlaceholderNames(const Message& message);

    class Arguments
    {
    public:
        Arguments& Set(std::string name, Value value);

        // The value given for `name`, or null when none was.
        [[nodiscard]] const Value* Find(std::string_view name) const noexcept;

    private:
        std::vector<std::pair<std::string, Value>> values_;
    };

    struct RenderOptions
    {
        Grammar grammar;

        // What an argument with no value renders as, by its name. An unfilled
        // select or plural takes its `other` branch.
        std::function<std::string(std::string_view name)> missing;

        // Upper-case the first letter of the result, when the grammar does.
        bool sentence = false;
    };

    [[nodiscard]] std::string Render(const Message& message, const Arguments& arguments, const RenderOptions& options);

    // `phrase` on its own: what `{x, phrase}` makes of `text`.
    [[nodiscard]] std::string Phrase(std::string_view text, const Grammar& grammar);

    // `list` on its own: what `{x, list}` makes of `items`.
    [[nodiscard]] std::string JoinList(const List& items, const Grammar& grammar);
} // namespace NarrativeEngine::TextTemplate
