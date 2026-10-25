#include <LanguageTable.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

// Unit tests for building a language table: where each key's text comes from,
// and what the census says about what could not be used.
//
// The INI texts are written inline, so each case shows the whole input. The
// real shipped files are checked by LanguageFiles.engine.test.cpp, which can
// see every key the plugin declares.

namespace
{
    using namespace NarrativeEngine;
    using LanguageTable::Sources;
    using LanguageTable::Table;

    constexpr std::array kGreetParams{TextKeys::Param{"who", TextTemplate::Kind::Person}};
    constexpr std::array kWaitParams{TextKeys::Param{"days", TextTemplate::Kind::Number}};
    constexpr TextKeys::Key kGreet{"Test", "greet", kGreetParams};
    constexpr TextKeys::Key kWait{"Test", "wait", kWaitParams};
    constexpr TextKeys::Key kFarewell{"Test", "farewell", {}};
    constexpr std::array kKeys{kGreet, kWait, kFarewell};

    constexpr std::string_view kEnglish = R"ini(
[Language]
Name = English
NativeName = English
Code = en
Status = reviewed
PluralRule = one_other
Fonts = en, pl

[Grammar]
ListSeparator = ", "
ListPairSeparator = " and "
ListFinalSeparator = ", and "
CapitalizeSentenceStart = true
PhraseTrailingPunctuation = ".!?;,"
PhraseLowercaseFirst = true

[Test]
greet = Hello, {who}.
wait = {days, plural, one {# day} other {# days}}
farewell = Goodbye.
)ini";

    constexpr std::string_view kGerman = R"ini(
[Language]
Name = German
NativeName = Deutsch
Code = DE
Status = draft
PluralRule = one_other
Fonts = en

[Grammar]
ListPairSeparator = " und "
ListFinalSeparator = " und "
PhraseLowercaseFirst = false

[Test]
greet = Hallo, {who, select, female {Frau {who}} other {{who}}}.
wait = {days, plural, one {# Tag} other {# Tage}}
)ini";

    std::string Rendered(const Table& table, const TextKeys::Key& key, const TextTemplate::Arguments& arguments = {})
    {
        return table.Render(key, arguments);
    }

    bool AnyContains(const std::vector<std::string>& lines, std::string_view needle)
    {
        return std::ranges::any_of(lines, [needle](const std::string& line) { return line.contains(needle); });
    }
} // namespace

TEST_CASE("LanguageTable: English alone", "[LanguageTable]")
{
    const auto table = Table::Build({kEnglish, std::nullopt, std::nullopt}, kKeys);
    const auto& census = table.GetCensus();

    CHECK(census.rejected.empty());
    CHECK(census.unavailable.empty());
    CHECK(census.fellBack.empty());
    CHECK(census.keys == 3);
    CHECK(census.fromEnglish == 3);

    CHECK(table.GetHeader().name == "English");
    CHECK(table.GetHeader().code == "en");
    CHECK(table.GetHeader().status == LanguageTable::Status::Reviewed);
    CHECK(table.GetHeader().fonts == std::vector<std::string>{"en", "pl"});
    SECTION("quoted grammar values keep their spaces")
    {
        CHECK(table.Grammar().listSeparator == ", ");
        CHECK(table.Grammar().listPairSeparator == " and ");
        CHECK(table.Grammar().phraseTrailingPunctuation == ".!?;,");
    }
    CHECK(Rendered(table, kGreet, TextTemplate::Arguments{}.Set("who", TextTemplate::Person{"Hulda"}))
          == "Hello, Hulda.");
    CHECK(Rendered(table, kWait, TextTemplate::Arguments{}.Set("days", std::int64_t{1})) == "1 day");
}

TEST_CASE("LanguageTable: a shipped language", "[LanguageTable]")
{
    const auto table = Table::Build({kEnglish, kGerman, std::nullopt}, kKeys);
    const auto& census = table.GetCensus();

    CHECK(census.rejected.empty());
    CHECK(table.GetHeader().name == "German");
    CHECK(table.GetHeader().nativeName == "Deutsch");
    SECTION("the code is lower-cased")
    {
        CHECK(table.GetHeader().code == "de");
    }
    CHECK(table.GetHeader().status == LanguageTable::Status::Draft);

    SECTION("its own text, rendered with its own grammar")
    {
        CHECK(Rendered(table,
                       kGreet,
                       TextTemplate::Arguments{}.Set("who", TextTemplate::Person{"Hulda", TextTemplate::Sex::Female}))
              == "Hallo, Frau Hulda.");
        CHECK(Rendered(table, kWait, TextTemplate::Arguments{}.Set("days", std::int64_t{3})) == "3 Tage");
        CHECK(census.fromShipped == 2);
    }
    SECTION("a missing key falls back to English, and says so")
    {
        CHECK(Rendered(table, kFarewell) == "Goodbye.");
        CHECK(census.fromEnglish == 1);
        CHECK(AnyContains(census.fellBack, "[Test] farewell"));
    }
    SECTION("grammar falls back key by key")
    {
        CHECK(table.Grammar().listPairSeparator == " und ");
        CHECK(table.Grammar().listSeparator == ", ");
        CHECK_FALSE(table.Grammar().phraseLowercaseFirst);
        CHECK(table.Grammar().capitalizeSentenceStart);
    }
}

TEST_CASE("LanguageTable: a translation that is wrong falls back to English", "[LanguageTable]")
{
    constexpr std::string_view broken = R"ini(
[Language]
Name = German
NativeName = Deutsch
Code = de
Status = draft
PluralRule = one_other
Fonts = en

[Test]
greet = Hallo, {wer}.
wait = {days, plural, one {# Tag}}
farewell = Auf {Wiedersehen.
)ini";
    const auto table = Table::Build({kEnglish, broken, std::nullopt}, kKeys);
    const auto& census = table.GetCensus();

    CHECK(census.rejected.size() == 3);
    CHECK(AnyContains(census.rejected, "'wer' is not a placeholder"));
    CHECK(AnyContains(census.rejected, "no 'other' branch"));
    CHECK(census.fromEnglish == 3);
    CHECK(Rendered(table, kGreet, TextTemplate::Arguments{}.Set("who", TextTemplate::Person{"Hulda"}))
          == "Hello, Hulda.");
    CHECK(Rendered(table, kFarewell) == "Goodbye.");
}

TEST_CASE("LanguageTable: the overlay", "[LanguageTable]")
{
    SECTION("overrides the shipped file key by key")
    {
        constexpr std::string_view overlay = "[Test]\nwait = {days, plural, one {ein Tag} other {# Tage lang}}\n";
        const auto table = Table::Build({kEnglish, kGerman, overlay}, kKeys);
        CHECK(Rendered(table, kWait, TextTemplate::Arguments{}.Set("days", std::int64_t{2})) == "2 Tage lang");
        CHECK(table.GetCensus().fromOverlay == 1);
        CHECK(table.GetCensus().fromShipped == 1);
        CHECK(table.GetCensus().overlayUnmatched.empty());
    }
    SECTION("a rejected overlay line falls back to the shipped file, not straight to English")
    {
        constexpr std::string_view overlay = "[Test]\ngreet = {nobody}\n";
        const auto table = Table::Build({kEnglish, kGerman, overlay}, kKeys);
        CHECK(Rendered(table,
                       kGreet,
                       TextTemplate::Arguments{}.Set("who", TextTemplate::Person{"Amren", TextTemplate::Sex::Male}))
              == "Hallo, Amren.");
        CHECK(AnyContains(table.GetCensus().rejected, "overlay"));
    }
    SECTION("can change grammar, but not which language this is")
    {
        constexpr std::string_view overlay = "[Language]\nName = Klingon\n[Grammar]\nListSeparator = \" / \"\n";
        const auto table = Table::Build({kEnglish, kGerman, overlay}, kKeys);
        CHECK(table.GetHeader().name == "German");
        CHECK(table.Grammar().listSeparator == " / ");
        CHECK(table.GetCensus().overlayUnmatched == std::vector<std::string>{"[Language] Name"});
    }
    SECTION("a line that overrides nothing is reported")
    {
        constexpr std::string_view overlay = "[Test]\ngreeet = Hi.\n[Tset]\ngreet = Hi.\n";
        const auto table = Table::Build({kEnglish, std::nullopt, overlay}, kKeys);
        CHECK(table.GetCensus().overlayUnmatched == std::vector<std::string>{"[Test] greeet", "[Tset] greet"});
        CHECK(Rendered(table, kFarewell) == "Goodbye.");
    }
    SECTION("works over English itself")
    {
        constexpr std::string_view overlay = "[Test]\nfarewell = Farewell, then.\n";
        const auto table = Table::Build({kEnglish, std::nullopt, overlay}, kKeys);
        CHECK(Rendered(table, kFarewell) == "Farewell, then.");
    }
}

TEST_CASE("LanguageTable: what the census rejects", "[LanguageTable]")
{
    SECTION("a key no part of the plugin declares")
    {
        const auto table =
            Table::Build({kEnglish,
                          std::string_view{"[Language]\nName = X\nNativeName = X\nCode = x\nStatus = draft\n"
                                           "PluralRule = none\nFonts = en\n[Test]\nfarewel = Tschuss.\n"},
                          std::nullopt},
                         kKeys);
        CHECK(table.GetCensus().rejected
              == std::vector<std::string>{"shipped: [Test] farewel is not a key this version of the plugin knows"});
    }
    SECTION("a header field the language's file lacks, and a value that means nothing")
    {
        const auto table =
            Table::Build({kEnglish,
                          std::string_view{"[Language]\nName = German\nStatus = finished\nPluralRule = dual\n"},
                          std::nullopt},
                         kKeys);
        const auto& rejected = table.GetCensus().rejected;
        CHECK(AnyContains(rejected, "[Language] Code is missing"));
        CHECK(AnyContains(rejected, "neither draft nor reviewed"));
        CHECK(AnyContains(rejected, "not a plural rule"));
        CHECK(table.GetHeader().code == "en");
        CHECK(table.Grammar().pluralRule == TextTemplate::PluralRule::OneOther);
    }
    SECTION("a file that is not UTF-8 is not used at all")
    {
        const auto table =
            Table::Build({kEnglish, std::string_view{"[Test]\nfarewell = Tsch\xFC\x73.\n"}, std::nullopt}, kKeys);
        CHECK(AnyContains(table.GetCensus().rejected, "not valid UTF-8"));
        CHECK(Rendered(table, kFarewell) == "Goodbye.");
    }
    SECTION("a declared key not even English can supply")
    {
        constexpr TextKeys::Key kOrphan{"Test", "orphan", {}};
        constexpr std::array keys{kGreet, kOrphan};
        const auto table = Table::Build({kEnglish, std::nullopt, std::nullopt}, keys);
        CHECK(table.GetCensus().unavailable == std::vector<std::string>{"[Test] orphan"});
        CHECK(table.Find(kOrphan) == nullptr);
        CHECK(Rendered(table, kOrphan).empty());
    }
}

TEST_CASE("LanguageTable: a multi-line template", "[LanguageTable]")
{
    constexpr std::string_view english =
        "[Language]\nName = English\nNativeName = English\nCode = en\nStatus = reviewed\n"
        "PluralRule = one_other\nFonts = en\n[Test]\nfarewell = <<<END\nFirst line.\n\nSecond line.\nEND\n";
    const auto table = Table::Build({english, std::nullopt, std::nullopt}, std::array{kFarewell});
    CHECK(Rendered(table, kFarewell) == "First line.\n\nSecond line.");
}

TEST_CASE("LanguageTable: data-file overrides", "[LanguageTable]")
{
    constexpr std::string_view shipped = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\nStatus = draft\n"
                                         "PluralRule = one_other\nFonts = en\n"
                                         "[Data.Faction]\ncollege = das College von Winterfeste\n"
                                         "companions = die Gefaehrten\n"
                                         "[Data.Item]\nTG04GoldenglowDeed = eine Besitzurkunde\n";
    // "die Gef\xC3\xA4hrten": the overlay's spelling, with the umlaut.
    constexpr std::string_view overlay =
        "[Data.Faction]\ncompanions = die Gef\xC3\xA4hrten\n[Data.AttackerGroup]\nbandits = Banditen\n";

    SECTION("an override replaces the data file's text, by the row's id")
    {
        const auto table = Table::Build({kEnglish, shipped, std::nullopt}, kKeys);
        REQUIRE(table.DataText("Data.Faction", "college") != nullptr);
        CHECK(*table.DataText("Data.Faction", "college") == "das College von Winterfeste");
        CHECK(*table.DataText("Data.Item", "TG04GoldenglowDeed") == "eine Besitzurkunde");
        SECTION("and ids compare without regard to case, as the files' own keys do")
        {
            CHECK(table.DataText("data.faction", "COLLEGE") != nullptr);
        }
    }
    SECTION("the overlay wins over the shipped file")
    {
        const auto table = Table::Build({kEnglish, shipped, overlay}, kKeys);
        CHECK(*table.DataText("Data.Faction", "companions") == "die Gef\xC3\xA4hrten");
        CHECK(*table.DataText("Data.AttackerGroup", "bandits") == "Banditen");
        CHECK(*table.DataText("Data.Faction", "college") == "das College von Winterfeste");
    }
    SECTION("no override leaves the data file's own text standing")
    {
        const auto table = Table::Build({kEnglish, shipped, std::nullopt}, kKeys);
        CHECK(table.DataText("Data.Faction", "skaal") == nullptr);
        CHECK(table.DataText("Data.AttackerGroup", "bandits") == nullptr);
    }
    SECTION("an override for a row nobody loaded is reported, by the loader that knows the rows")
    {
        const auto table = Table::Build({kEnglish, shipped, overlay}, kKeys);
        const std::vector<std::string> loaded{"college", "skaal"};
        CHECK(table.UnmatchedData("Data.Faction", loaded) == std::vector<std::string>{"[Data.Faction] companions"});
        CHECK(table.UnmatchedData("Data.Item", std::vector<std::string>{"TG04GoldenglowDeed"}).empty());
    }
    SECTION("data sections are not unknown keys")
    {
        const auto table = Table::Build({kEnglish, shipped, overlay}, kKeys);
        CHECK(table.GetCensus().rejected.empty());
        CHECK(table.GetCensus().overlayUnmatched.empty());
    }
}

TEST_CASE("LanguageTable: prompt forms", "[LanguageTable]")
{
    constexpr std::string_view englishWithForm = "[Language]\nName = English\nNativeName = English\nCode = en\n"
                                                 "Status = reviewed\nPluralRule = one_other\nFonts = en\n"
                                                 "[Prompt.Form]\nscheme = a bare verb phrase\n";
    constexpr std::string_view shipped = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\nStatus = draft\n"
                                         "PluralRule = one_other\nFonts = en\n"
                                         "[Prompt.Form]\nscheme = a zu-clause\nfinish = a sentence\n";
    constexpr std::string_view overlay = "[Prompt.Form]\nScheme = an infinitive clause ending in zu\n";

    SECTION("a field's form comes from the language's own file")
    {
        const auto table = Table::Build({englishWithForm, shipped, std::nullopt}, kKeys);
        REQUIRE(table.PromptForm("scheme") != nullptr);
        CHECK(*table.PromptForm("scheme") == "a zu-clause");
        CHECK(*table.PromptForm("finish") == "a sentence");
        CHECK(table.PromptForm("ambition") == nullptr);
    }
    SECTION("the overlay wins, and field names compare without regard to case")
    {
        const auto table = Table::Build({englishWithForm, shipped, overlay}, kKeys);
        CHECK(*table.PromptForm("scheme") == "an infinitive clause ending in zu");
        CHECK(*table.PromptForm("finish") == "a sentence");
    }
    SECTION("never from English, whose form describes an English phrase")
    {
        constexpr std::string_view noForms = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\n"
                                             "Status = draft\nPluralRule = one_other\nFonts = en\n";
        CHECK(Table::Build({englishWithForm, noForms, std::nullopt}, kKeys).PromptForm("scheme") == nullptr);
        CHECK(Table::Build({englishWithForm, std::nullopt, std::nullopt}, kKeys).PromptForm("scheme") == nullptr);
    }
    SECTION("form sections are not unknown keys")
    {
        const auto table = Table::Build({englishWithForm, shipped, overlay}, kKeys);
        CHECK(table.GetCensus().rejected.empty());
        CHECK(table.GetCensus().overlayUnmatched.empty());
    }
}

TEST_CASE("LanguageTable: the dashboard's strings", "[LanguageTable]")
{
    constexpr std::string_view english = "[Language]\nName = English\nNativeName = English\nCode = en\n"
                                         "Status = reviewed\nPluralRule = one_other\nFonts = en\n"
                                         "[Dashboard]\n"
                                         "tab.director = Director\n"
                                         "ticks = {n, plural, one {# tick} other {# ticks}}\n"
                                         "slot = Slot {index}\n"
                                         "who = {names, list}\n";

    SECTION("English's keys are the section's, in its order")
    {
        const auto table = Table::Build({english, std::nullopt, std::nullopt}, kKeys);
        using Pair = std::pair<std::string, std::string>;
        CHECK(table.Strings()
              == std::vector<Pair>{{"tab.director", "Director"},
                                   {"ticks", "{n, plural, one {# tick} other {# ticks}}"},
                                   {"slot", "Slot {index}"},
                                   {"who", "{names, list}"}});
        CHECK(table.GetCensus().rejected.empty());
    }
    SECTION("a translation is held to the placeholders English's template uses")
    {
        constexpr std::string_view german = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\n"
                                            "Status = draft\nPluralRule = one_other\nFonts = en\n"
                                            "[Dashboard]\n"
                                            "tab.director = Regie\n"
                                            "ticks = {n, plural, one {# Tick} other {# Ticks}}\n"
                                            "slot = Platz {index, plural, other {#}}\n"
                                            "who = {names, list} und {others}\n";
        const auto table = Table::Build({english, german, std::nullopt}, kKeys);
        const auto strings = table.Strings();
        REQUIRE(strings.size() == 4);
        CHECK(strings[0].second == "Regie");
        CHECK(strings[1].second == "{n, plural, one {# Tick} other {# Ticks}}");
        // English says {index} plainly, so it is text, and text cannot be
        // pluralized; and English has no {others}.
        CHECK(strings[2].second == "Slot {index}");
        CHECK(strings[3].second == "{names, list}");
        CHECK(AnyContains(table.GetCensus().rejected, "[Dashboard] slot"));
        CHECK(AnyContains(table.GetCensus().rejected, "[Dashboard] who"));
    }
    SECTION("a key English's file does not have is not a key")
    {
        constexpr std::string_view german = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\n"
                                            "Status = draft\nPluralRule = one_other\nFonts = en\n"
                                            "[Dashboard]\ntab.directr = Regie\n";
        const auto table = Table::Build({english, german, "[Dashboard]\ntab.lettres = Briefe\n"}, kKeys);
        CHECK(AnyContains(table.GetCensus().rejected, "[Dashboard] tab.directr is not a key"));
        CHECK(table.GetCensus().overlayUnmatched == std::vector<std::string>{"[Dashboard] tab.lettres"});
        CHECK(table.Strings().size() == 4);
    }
    SECTION("the overlay rewords one string and leaves the rest")
    {
        const auto table = Table::Build({english, std::nullopt, "[Dashboard]\ntab.director = The Director\n"}, kKeys);
        CHECK(table.Strings()[0].second == "The Director");
        CHECK(table.Strings()[2].second == "Slot {index}");
    }
}
