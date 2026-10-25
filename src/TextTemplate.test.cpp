#include <TextTemplate.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// Unit tests for the template formatter.
//
// Grammar defaults are English, which is what today's plot sentences render
// with; the other languages' rules are exercised through the plural families
// and the grammar flags a language file can change.

namespace
{
    using namespace NarrativeEngine::TextTemplate;

    constexpr std::string_view kUlfrikUpper = "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA"; // Ульфрик
    constexpr std::string_view kUlfrikLower = "\xD1\x83\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA"; // ульфрик
    constexpr std::string_view kIdeographicFullStop = "\xE3\x80\x82";                                     // U+3002

    Message MustParse(std::string_view text)
    {
        auto parsed = Parse(text);
        INFO(text);
        REQUIRE(parsed.Ok());
        return std::move(parsed.message);
    }

    std::string RenderText(std::string_view text, const Arguments& arguments, RenderOptions options = {})
    {
        return Render(MustParse(text), arguments, options);
    }

    std::string FirstProblem(std::string_view text)
    {
        const auto parsed = Parse(text);
        return parsed.problems.empty() ? std::string{} : parsed.problems.front();
    }
} // namespace

TEST_CASE("TextTemplate::Parse accepts the subset", "[TextTemplate]")
{
    for (const std::string_view text : {
             "plain text, with an apostrophe's worth of punctuation.",
             "{agent}",
             "{ agent }",
             "I sent {agent} to watch {mark}.",
             "{agent, select, female {she} male {he} other {they}}",
             "{days, plural, one {# day} other {# days}}",
             "{days,plural,one{#}other{#}}",
             "{names, list}",
             "{scheme, phrase}",
             "{agent, select, female {{days, plural, one {her # day} other {her # days}}} other {{agent}}}",
             "",
         }) {
        INFO(text);
        CHECK(Parse(text).Ok());
    }
}

TEST_CASE("TextTemplate::Parse refuses what the subset does not define", "[TextTemplate]")
{
    SECTION("a literal brace")
    {
        CHECK(FirstProblem("a { brace") != "");
        CHECK(FirstProblem("a } brace") != "");
        CHECK(FirstProblem("{}") != "");
    }
    SECTION("an unclosed placeholder or branch")
    {
        CHECK(FirstProblem("{agent") != "");
        CHECK(FirstProblem("{agent, select, other {x}") != "");
        CHECK(FirstProblem("{agent, select, other {x") != "");
    }
    SECTION("an unknown format, or a select with no branches")
    {
        CHECK(FirstProblem("{agent, number}") != "");
        CHECK(FirstProblem("{agent, select, }") != "");
        CHECK(FirstProblem("{agent, select}") != "");
    }
    SECTION("a problem leaves no half-parsed message behind")
    {
        const auto parsed = Parse("ok so far {broken");
        CHECK_FALSE(parsed.Ok());
        CHECK(parsed.message.empty());
    }
}

TEST_CASE("TextTemplate::Validate", "[TextTemplate]")
{
    const Signature signature = {
        {"agent", Kind::Person},
        {"mark", Kind::Person},
        {"scheme", Kind::Text},
        {"days", Kind::Number},
        {"names", Kind::List},
    };
    const auto problems = [&](std::string_view text) { return Validate(MustParse(text), signature); };

    SECTION("a sound template has no problems")
    {
        CHECK(problems("I sent {agent} to watch {mark} as part of my plan to {scheme, phrase}.").empty());
        CHECK(problems("{agent, select, female {she} other {they}} waited {days, plural, one {# day} other {# days}}")
                  .empty());
        CHECK(problems("I told {names, list}.").empty());
    }
    SECTION("an undeclared placeholder, at any depth")
    {
        CHECK(problems("{target}").size() == 1);
        CHECK(problems("{agent, select, other {{target}}}").size() == 1);
    }
    SECTION("a format the parameter's kind cannot take")
    {
        CHECK(problems("{scheme, plural, other {#}}").size() == 1);
        CHECK(problems("{scheme, list}").size() == 1);
        CHECK(problems("{agent, phrase}").size() == 1);
        CHECK(problems("{days, select, other {x}}").size() == 1);
    }
    SECTION("no other branch, a repeated key, a key CLDR lacks")
    {
        CHECK(problems("{agent, select, female {she}}").size() == 1);
        CHECK(problems("{days, plural, one {a} one {b} other {c}}").size() == 1);
        CHECK(problems("{days, plural, single {a} other {b}}").size() == 1);
    }
    SECTION("names in order of first use")
    {
        CHECK(PlaceholderNames(MustParse("{mark} {agent, select, other {{mark} {days, plural, other {#}}}}"))
              == std::vector<std::string>{"mark", "agent", "days"});
    }
}

TEST_CASE("TextTemplate::Render", "[TextTemplate]")
{
    SECTION("each kind of value")
    {
        Arguments arguments;
        arguments.Set("text", std::string{"the amulet"})
            .Set("person", Person{"Hulda", Sex::Female})
            .Set("number", std::int64_t{3})
            .Set("list", List{"Ysolda", "Amren", "Saffir"});
        CHECK(RenderText("{text}|{person}|{number}|{list}", arguments)
              == "the amulet|Hulda|3|Ysolda, Amren, and Saffir");
    }
    SECTION("a missing value renders by its name")
    {
        RenderOptions options;
        options.missing = [](std::string_view name) { return name == "agent" ? "someone" : "somewhere"; };
        CHECK(RenderText("{agent} went {place}.", {}, options) == "someone went somewhere.");
        CHECK(RenderText("{agent} went.", {}) == " went.");
    }
    SECTION("select on a person's sex, on text, and when missing")
    {
        const auto text = std::string_view{"{who, select, female {she} male {he} other {they}}"};
        CHECK(RenderText(text, Arguments{}.Set("who", Person{"Hulda", Sex::Female})) == "she");
        CHECK(RenderText(text, Arguments{}.Set("who", Person{"Amren", Sex::Male})) == "he");
        CHECK(RenderText(text, Arguments{}.Set("who", Person{"Someone", Sex::Other})) == "they");
        CHECK(RenderText(text, {}) == "they");
        CHECK(RenderText("{kind, select, rain {wet} other {dry}}", Arguments{}.Set("kind", std::string{"rain"}))
              == "wet");
    }
    SECTION("plural, with # as the number")
    {
        const auto text = std::string_view{"{n, plural, one {# day} other {# days}}"};
        CHECK(RenderText(text, Arguments{}.Set("n", std::int64_t{1})) == "1 day");
        CHECK(RenderText(text, Arguments{}.Set("n", std::int64_t{2})) == "2 days");
        CHECK(RenderText(text, Arguments{}.Set("n", std::int64_t{0})) == "0 days");
    }
    SECTION("a category the template lacks falls back to other")
    {
        RenderOptions russian;
        russian.grammar.pluralRule = PluralRule::Russian;
        CHECK(RenderText("{n, plural, one {a} other {b}}", Arguments{}.Set("n", std::int64_t{3}), russian) == "b");
    }
    SECTION("# is the number only directly inside a plural's branch, as in ICU")
    {
        CHECK(RenderText("#1", {}) == "#1");
        const auto arguments =
            Arguments{}.Set("n", std::int64_t{4}).Set("m", std::int64_t{2}).Set("who", Person{"Hulda", Sex::Female});
        // Inside a select nested in the plural, # is literal; {n} says it.
        CHECK(RenderText("{n, plural, other {{who, select, other {# {n} for {who}}}}}", arguments) == "# 4 for Hulda");
        // A plural nested in a plural: each # is its own plural's number.
        CHECK(RenderText("{n, plural, other {# and {m, plural, other {#}}}}", arguments) == "4 and 2");
    }
    SECTION("a sentence opens with a capital, in any cased script")
    {
        RenderOptions sentence;
        sentence.sentence = true;
        sentence.missing = [](std::string_view) { return std::string{"someone"}; };
        CHECK(RenderText("{agent} watched.", {}, sentence) == "Someone watched.");
        CHECK(RenderText("{agent} watched.", Arguments{}.Set("agent", std::string{kUlfrikLower}), sentence)
              == std::string{kUlfrikUpper} + " watched.");
        sentence.grammar.capitalizeSentenceStart = false;
        CHECK(RenderText("{agent} watched.", {}, sentence) == "someone watched.");
    }
}

TEST_CASE("TextTemplate::Plural families at their boundaries", "[TextTemplate]")
{
    using enum PluralCategory;
    SECTION("None")
    {
        for (const std::int64_t n : {0, 1, 2, 5, 21}) {
            CHECK(Plural(PluralRule::None, n) == Other);
        }
    }
    SECTION("OneOther")
    {
        CHECK(Plural(PluralRule::OneOther, 0) == Other);
        CHECK(Plural(PluralRule::OneOther, 1) == One);
        CHECK(Plural(PluralRule::OneOther, 2) == Other);
        CHECK(Plural(PluralRule::OneOther, 21) == Other);
    }
    SECTION("Spanish: one is one, and an exact million is many")
    {
        CHECK(Plural(PluralRule::Spanish, 0) == Other);
        CHECK(Plural(PluralRule::Spanish, 1) == One);
        CHECK(Plural(PluralRule::Spanish, 2) == Other);
        CHECK(Plural(PluralRule::Spanish, 1'000'000) == Many);
        CHECK(Plural(PluralRule::Spanish, 1'000'001) == Other);
        CHECK(Plural(PluralRule::Spanish, 3'000'000) == Many);
    }
    SECTION("French: zero and one are one, and an exact million is many")
    {
        CHECK(Plural(PluralRule::French, 0) == One);
        CHECK(Plural(PluralRule::French, 1) == One);
        CHECK(Plural(PluralRule::French, 2) == Other);
        CHECK(Plural(PluralRule::French, 1'000'000) == Many);
        CHECK(Plural(PluralRule::French, 1'000'001) == Other);
    }
    SECTION("Russian")
    {
        CHECK(Plural(PluralRule::Russian, 1) == One);
        CHECK(Plural(PluralRule::Russian, 2) == Few);
        CHECK(Plural(PluralRule::Russian, 5) == Many);
        CHECK(Plural(PluralRule::Russian, 11) == Many);
        CHECK(Plural(PluralRule::Russian, 21) == One);
        CHECK(Plural(PluralRule::Russian, 22) == Few);
        CHECK(Plural(PluralRule::Russian, 25) == Many);
        CHECK(Plural(PluralRule::Russian, 0) == Many);
    }
    SECTION("Polish")
    {
        CHECK(Plural(PluralRule::Polish, 1) == One);
        CHECK(Plural(PluralRule::Polish, 2) == Few);
        CHECK(Plural(PluralRule::Polish, 5) == Many);
        CHECK(Plural(PluralRule::Polish, 12) == Many);
        CHECK(Plural(PluralRule::Polish, 22) == Few);
        CHECK(Plural(PluralRule::Polish, 21) == Many);
    }
}

TEST_CASE("TextTemplate::Phrase", "[TextTemplate]")
{
    const Grammar english;
    SECTION("English trims, then lowercases the first letter")
    {
        CHECK(Phrase("  Spread a rumor that Hulda drinks. ", english) == "spread a rumor that Hulda drinks");
        CHECK(Phrase("Steal it!?;,", english) == "steal it");
        CHECK(Phrase("Ruin Belethor, then leave", english) == "ruin Belethor, then leave");
        CHECK(Phrase("", english).empty());
        CHECK(Phrase(" . ", english).empty());
    }
    SECTION("a language that keeps the case, and its own full stop")
    {
        Grammar german;
        german.phraseLowercaseFirst = false;
        CHECK(Phrase("Das Amulett zu stehlen.", german) == "Das Amulett zu stehlen");

        Grammar japanese;
        japanese.phraseTrailingPunctuation = std::string{kIdeographicFullStop};
        CHECK(Phrase(std::string{"x"} + std::string{kIdeographicFullStop}, japanese) == "x");
    }
    SECTION("through a template")
    {
        CHECK(RenderText("my plan to {scheme, phrase}.", Arguments{}.Set("scheme", std::string{"Steal the amulet."}))
              == "my plan to steal the amulet.");
    }
}

TEST_CASE("TextTemplate::JoinList", "[TextTemplate]")
{
    const Grammar english;
    CHECK(JoinList({}, english).empty());
    CHECK(JoinList({"Ysolda"}, english) == "Ysolda");
    CHECK(JoinList({"Ysolda", "Amren"}, english) == "Ysolda and Amren");
    CHECK(JoinList({"Ysolda", "Amren", "Saffir"}, english) == "Ysolda, Amren, and Saffir");
    CHECK(JoinList({"A", "B", "C", "D"}, english) == "A, B, C, and D");

    Grammar german;
    german.listPairSeparator = " und ";
    german.listFinalSeparator = " und ";
    CHECK(JoinList({"A", "B", "C"}, german) == "A, B und C");
}
