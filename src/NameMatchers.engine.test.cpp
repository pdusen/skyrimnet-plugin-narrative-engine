#include <PlotCandidates.h>
#include <PlotCasting.h>
#include <PlotVerbs.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// The plot code's three name matchers -- whom a role's words name, whom a
// scheme names, and whom a step's sentence names -- reading names as the game
// writes them in other languages.
//
// The names are the game's own, from the Spriggit export's localized strings:
// Ulfric Stormcloak, Balgruuf the Greater and Hulda, in Russian, Japanese,
// Chinese, Polish and German. Each language's rules are a synthetic file of
// the shape the drafts will take. Non-ASCII text is written as escapes.

namespace
{
    namespace PlotCandidates = NarrativeEngine::PlotCandidates;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    using NarrativeEngine::Testing::Synthetic;

    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kUlfric = 0x0001414D;
    constexpr RE::FormID kBalgruuf = 0x00013BBD;
    constexpr RE::FormID kHulda = 0x00013BA3;
    constexpr RE::FormID kFaction = 0x0000C001;
    constexpr RE::FormID kHold = 0x0000B001;
    constexpr RE::FormID kTown = 0x0000A001;

    struct Names
    {
        const char* ulfric;
        const char* balgruuf;
        const char* hulda;
    };

    // Ульфрик Буревестник, Балгруф Старший, Хульда
    constexpr Names kRussian{"\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA "
                             "\xD0\x91\xD1\x83\xD1\x80\xD0\xB5\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD0\xB8\xD0\xBA",
                             "\xD0\x91\xD0\xB0\xD0\xBB\xD0\xB3\xD1\x80\xD1\x83\xD1\x84 "
                             "\xD0\xA1\xD1\x82\xD0\xB0\xD1\x80\xD1\x88\xD0\xB8\xD0\xB9",
                             "\xD0\xA5\xD1\x83\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xB0"};
    // ウルフリック・ストームクローク, 偉大なるバルグルーフ, フルダ
    constexpr Names kJapanese{"\xE3\x82\xA6\xE3\x83\xAB\xE3\x83\x95\xE3\x83\xAA\xE3\x83\x83\xE3\x82\xAF\xE3\x83\xBB"
                              "\xE3\x82\xB9\xE3\x83\x88\xE3\x83\xBC\xE3\x83\xA0\xE3\x82\xAF\xE3\x83\xAD\xE3\x83\xBC"
                              "\xE3\x82\xAF",
                              "\xE5\x81\x89\xE5\xA4\xA7\xE3\x81\xAA\xE3\x82\x8B\xE3\x83\x90\xE3\x83\xAB\xE3\x82\xB0"
                              "\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x95",
                              "\xE3\x83\x95\xE3\x83\xAB\xE3\x83\x80"};
    // 烏弗瑞克．風暴斗篷, 強者巴爾古夫, 胡爾達
    constexpr Names kChinese{"\xE7\x83\x8F\xE5\xBC\x97\xE7\x91\x9E\xE5\x85\x8B\xEF\xBC\x8E\xE9\xA2\xA8\xE6\x9A\xB4"
                             "\xE6\x96\x97\xE7\xAF\xB7",
                             "\xE5\xBC\xB7\xE8\x80\x85\xE5\xB7\xB4\xE7\x88\xBE\xE5\x8F\xA4\xE5\xA4\xAB",
                             "\xE8\x83\xA1\xE7\x88\xBE\xE9\x81\x94"};
    // Ulfric Gromowładny, Balgruuf Większy, Hulda
    constexpr Names kPolish{"Ulfric Gromow\xC5\x82"
                            "adny",
                            "Balgruuf Wi\xC4\x99kszy",
                            "Hulda"};
    // Ulfric Sturmmantel, Balgruuf der Ältere, Hulda
    constexpr Names kGerman{"Ulfric Sturmmantel", "Balgruuf der \xC3\x84ltere", "Hulda"};

    // The three, the mastermind's subordinates in one town, so any role a
    // known person may fill can be any of them, and any step can go to them.
    PlotCasting::Population People(const Names& names)
    {
        PlotCasting::Population p;
        PlotCasting::Member boss;
        boss.npc = kBoss;
        boss.name = "Olfrid";
        boss.hold = kHold;
        boss.settlement = kTown;
        boss.factions.push_back({kFaction, 1.0, true});
        for (const auto npc : {kUlfric, kBalgruuf, kHulda}) {
            boss.ties.push_back({npc, true, 0});
        }
        p.members.push_back(boss);
        for (const auto& [npc, name] :
             {std::pair{kUlfric, names.ulfric}, std::pair{kBalgruuf, names.balgruuf}, std::pair{kHulda, names.hulda}}) {
            PlotCasting::Member m;
            m.npc = npc;
            m.name = name;
            m.hold = kHold;
            m.settlement = kTown;
            m.factions.push_back({kFaction, 0.5, true});
            p.members.push_back(m);
        }
        return p;
    }

    // Who a "watch" step's mark resolves to when the scheme names it so, or
    // 0 for nobody.
    RE::FormID Mark(const PlotCasting::Population& population,
                    const NarrativeEngine::LanguageTable::Table& table,
                    const char* text)
    {
        static const NarrativeEngine::PlotMenus::World world;
        const std::vector<NarrativeEngine::PlotModel::SlotValue> none;
        const std::vector<const NarrativeEngine::PlotModel::Step*> earlier;
        PlotCandidates::Request request{population, kBoss, world, none, earlier, {}, {}};
        request.table = &table;
        const auto out = PlotCandidates::ResolveRoles(
            *NarrativeEngine::PlotVerbs::Find("watch"), {{"mark", 0, std::string{text}}}, request);
        return out.problems.empty() && !out.values.empty() ? out.values.front().form : 0;
    }

    // Whether a step whose sentence is `text` may go to `npc`: whether the
    // sentence does not name them.
    bool May(const PlotCasting::Population& population,
             const NarrativeEngine::LanguageTable::Table& table,
             RE::FormID npc,
             const char* text)
    {
        const PlotCasting::OccupancyTable occupancy;
        return PlotCasting::QualifyAgent(population, kBoss, npc, text, 0, occupancy, 10.0, {}, {}, table).Eligible();
    }
} // namespace

TEST_CASE("Name matchers: Russian", "[NameMatchers][engine]")
{
    // The genitive, dative, instrumental and prepositional endings.
    const auto table = Synthetic("ru",
                                 "Name.Suffixes = \xD0\xB0, \xD1\x83, \xD0\xBE\xD0\xBC, \xD0\xBE\xD0\xB9, "
                                 "\xD0\xB5, \xD1\x8B");
    const auto people = People(kRussian);

    SECTION("a role naming the person resolves to them, inflected too")
    {
        CHECK(Mark(people, table, kRussian.hulda) == kHulda);
        // "Ульфрика": Ulfric, genitive.
        CHECK(Mark(people, table, "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA\xD0\xB0") == kUlfric);
        // "Балгруфом": Balgruuf, instrumental.
        CHECK(Mark(people, table, "\xD0\x91\xD0\xB0\xD0\xBB\xD0\xB3\xD1\x80\xD1\x83\xD1\x84\xD0\xBE\xD0\xBC")
              == kBalgruuf);
    }

    SECTION("a name inside a longer word, or in lowercase, does not")
    {
        // "Хульдаборг", "хульда".
        CHECK(Mark(people, table, "\xD0\xA5\xD1\x83\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x80\xD0\xB3")
              == 0);
        CHECK(Mark(people, table, "\xD1\x85\xD1\x83\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xB0") == 0);
    }

    SECTION("a scheme naming the person in the genitive names them")
    {
        // "Разорить Ульфрика"
        const auto named = PlotCandidates::NamedIn(people,
                                                   "\xD0\xA0\xD0\xB0\xD0\xB7\xD0\xBE\xD1\x80\xD0\xB8\xD1\x82\xD1\x8C "
                                                   "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA\xD0\xB0",
                                                   table);
        CHECK(named.contains(kUlfric));
        CHECK_FALSE(named.contains(kHulda));
    }

    SECTION("a step naming the person, in the instrumental, is not given to them")
    {
        // "Следить за Ульфриком"
        const char* step = "\xD0\xA1\xD0\xBB\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD1\x8C \xD0\xB7\xD0\xB0 "
                           "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xBC";
        CHECK_FALSE(May(people, table, kUlfric, step));
        CHECK(May(people, table, kBalgruuf, step));
    }
}

TEST_CASE("Name matchers: Japanese and Chinese, written without spaces", "[NameMatchers][engine]")
{
    const auto japanese = Synthetic("ja", "", "WordSeparated = false");
    const auto chinese = Synthetic("zh", "", "WordSeparated = false");

    SECTION("a Japanese role resolves by a name's own part, inside the sentence")
    {
        const auto people = People(kJapanese);
        // "ウルフリック", "フルダの店".
        CHECK(Mark(people, japanese, "\xE3\x82\xA6\xE3\x83\xAB\xE3\x83\x95\xE3\x83\xAA\xE3\x83\x83\xE3\x82\xAF")
              == kUlfric);
        CHECK(Mark(people, japanese, "\xE3\x83\x95\xE3\x83\xAB\xE3\x83\x80\xE3\x81\xAE\xE5\xBA\x97") == kHulda);
        CHECK(Mark(people, japanese, kJapanese.balgruuf) == kBalgruuf);
        // "ウルフリックを見張る": watch Ulfric.
        const char* step = "\xE3\x82\xA6\xE3\x83\xAB\xE3\x83\x95\xE3\x83\xAA\xE3\x83\x83\xE3\x82\xAF\xE3\x82\x92"
                           "\xE8\xA6\x8B\xE5\xBC\xB5\xE3\x82\x8B";
        CHECK_FALSE(May(people, japanese, kUlfric, step));
        CHECK(May(people, japanese, kHulda, step));
        CHECK(PlotCandidates::NamedIn(people, step, japanese).contains(kUlfric));
    }

    SECTION("a Chinese role resolves the same way")
    {
        const auto people = People(kChinese);
        // "烏弗瑞克", "胡爾達的店".
        CHECK(Mark(people, chinese, "\xE7\x83\x8F\xE5\xBC\x97\xE7\x91\x9E\xE5\x85\x8B") == kUlfric);
        CHECK(Mark(people, chinese, "\xE8\x83\xA1\xE7\x88\xBE\xE9\x81\x94\xE7\x9A\x84\xE5\xBA\x97") == kHulda);
        CHECK(Mark(people, chinese, kChinese.balgruuf) == kBalgruuf);
        // "監視烏弗瑞克": watch Ulfric.
        const char* step = "\xE7\x9B\xA3\xE8\xA6\x96\xE7\x83\x8F\xE5\xBC\x97\xE7\x91\x9E\xE5\x85\x8B";
        CHECK_FALSE(May(people, chinese, kUlfric, step));
        CHECK(May(people, chinese, kHulda, step));
    }
}

TEST_CASE("Name matchers: Polish and German", "[NameMatchers][engine]")
{
    SECTION("Polish: a role in the genitive resolves")
    {
        const auto table = Synthetic("pl", "Name.Suffixes = a, owi, em, ie, u");
        const auto people = People(kPolish);
        CHECK(Mark(people, table, "Ulfrica") == kUlfric);
        CHECK(Mark(people, table, "Balgruufem") == kBalgruuf);
        CHECK(Mark(people,
                   table,
                   "Gromow\xC5\x82"
                   "adny")
              == kUlfric);
        CHECK_FALSE(May(people, table, kUlfric, "\xC5\x9Aledzi\xC4\x87 Ulfrica"));
    }

    SECTION("German: a genitive without an apostrophe is the person, a longer word is not")
    {
        const auto table = Synthetic("de", "Name.Suffixes = s");
        const auto people = People(kGerman);
        CHECK(Mark(people, table, "Ulfrics") == kUlfric);
        CHECK(Mark(people, table, "Sturmmantels Pl\xC3\xA4ne") == kUlfric);
        CHECK(Mark(people,
                   table,
                   "Huldastra\xC3\x9F"
                   "e")
              == 0);
        CHECK_FALSE(May(people, table, kUlfric, "Ulfrics Pl\xC3\xA4ne beobachten"));
        CHECK(May(people, table, kUlfric, "Ulfricssons Pl\xC3\xA4ne beobachten"));
        CHECK(PlotCandidates::NamedIn(people, "Ulfrics Thron stehlen", table).contains(kUlfric));
    }
}
