#include <Localization.h>
#include <PlotDispatch.h>
#include <PlotMenus.h>
#include <PlotRejection.h>
#include <PlotTrial.h>
#include <SkyrimNetAPI.h>

#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>

// The language trial's three promises: a trial birth leaves the live plot
// state as it found it, a trial's tables are its own, read beside the
// session's without touching it, and so is the world its births name.

namespace
{
    namespace Localization = NarrativeEngine::Localization;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotMenus = NarrativeEngine::PlotMenus;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotTrial = NarrativeEngine::PlotTrial;
    using NarrativeEngine::PlotState;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    std::filesystem::path Languages()
    {
        return std::filesystem::path{NARRATIVEENGINE_STATICS_DIR} / "SKSE/Plugins/NarrativeEngine/Languages";
    }

    PlotCasting::Member Person(RE::FormID npc, const char* name)
    {
        PlotCasting::Member m;
        m.npc = npc;
        m.name = name;
        return m;
    }

    PlotCasting::Population Village()
    {
        PlotCasting::Population p;
        p.members.push_back(Person(0x00010001, "Olfrid"));
        p.members.push_back(Person(0x00010002, "Idolaf"));
        p.members.push_back(Person(0x00010003, "Hulda"));
        return p;
    }

    // A live state with something in every field a birth could touch.
    PlotState Live()
    {
        PlotState state;
        PlotModel::Plot plot;
        plot.id = 7;
        plot.mastermind = 0x00010009;
        state.plots.push_back(plot);
        state.occupancy[0x00010009] = PlotModel::Occupancy{};
        state.nextPlotId = 8;
        state.counters.plotsBorn = 3;
        state.simGameDay = 12.5;
        state.rngState = 0x1234567890ABCDEFull;
        return state;
    }

    template <std::size_t N> void CopyTo(char (&dest)[N], const char* text)
    {
        ::strncpy_s(dest, text, _TRUNCATE);
    }

    PlotTrial::Predicates Everyone()
    {
        const auto yes = [](RE::FormID) { return true; };
        return {yes, yes, {}, yes};
    }

    // Runs `job` on the plot worker, as the trial does, and waits for it.
    PlotTrial::Birth OnPlotWorker(const std::function<PlotTrial::Birth(const NarrativeEngine::PlotThread::Token&)>& job)
    {
        std::promise<PlotTrial::Birth> done;
        auto result = done.get_future();
        NarrativeEngine::PlotDispatch::Start();
        NarrativeEngine::PlotDispatch::EnqueueWork(
            [&](const NarrativeEngine::PlotThread::Token& pt) { done.set_value(job(pt)); });
        REQUIRE(result.wait_for(std::chrono::seconds{30}) == std::future_status::ready);
        NarrativeEngine::PlotDispatch::Stop();
        return result.get();
    }
} // namespace

TEST_CASE("A language trial's tables are its own", "[PlotTrial][Localization][engine]")
{
    const auto* session = &Localization::Get();
    const auto sessionCode = session->GetHeader().code;

    // Two at once, both alive while the other is read.
    const auto german = Localization::LoadShipped("german", Languages());
    const auto russian = Localization::LoadShipped("russian", Languages());
    REQUIRE(german.has_value());
    REQUIRE(russian.has_value());
    CHECK(german->GetHeader().code == "de");
    CHECK(russian->GetHeader().code == "ru");
    CHECK(german->Strings() != russian->Strings());
    CHECK(german->GetCensus().rejected.empty());
    CHECK(russian->GetCensus().rejected.empty());

    SECTION("and the session's is the same table as before, in the same language")
    {
        CHECK(&Localization::Get() == session);
        CHECK(Localization::Get().GetHeader().code == sessionCode);
    }
    SECTION("English loads alone, and a language with no file loads nothing")
    {
        const auto english = Localization::LoadShipped("english", Languages());
        REQUIRE(english.has_value());
        CHECK(english->GetHeader().code == "en");
        CHECK_FALSE(Localization::LoadShipped("klingon", Languages()).has_value());
    }
}

TEST_CASE("A language trial's world is worded in its own table", "[PlotTrial][PlotMenus][engine]")
{
    using NarrativeEngine::PlotRecipes::Recipe;

    // A session's world as the load leaves it: English names, each beside
    // the key a language file rewords it by.
    PlotMenus::World session;
    session.settlements.push_back({0x00020001, "Whiterun"});
    session.factions.push_back({0x00030001, "the College of Winterhold", "college"});
    session.factions.push_back({0x00030002, "the Moonlit Order", "moonlit_order"});
    session.items.push_back({0x00040001, "a deed of ownership", {}, "TG04GoldenglowDeed"});
    session.items.push_back({0x00040002, "a silver locket", {}, "ModdedLocket"});
    // Recipes phrased in English at load, from the game's bare names, and one
    // with no name kept, as a test fabricates them.
    Recipe sword;
    sword.form = 0x00050001;
    sword.displayName = "an Iron Sword";
    sword.name = "Iron Sword";
    session.recipes.push_back(sword);
    Recipe arrows;
    arrows.form = 0x00050002;
    arrows.displayName = "a sheaf of Elven Arrows";
    arrows.name = "Elven Arrow";
    arrows.ammo = true;
    session.recipes.push_back(arrows);
    Recipe unnamed;
    unnamed.form = 0x00050003;
    unnamed.displayName = "a Steel Sword";
    session.recipes.push_back(unnamed);

    // Rows for one item and one faction, and none for the other two:
    // "купчая", a bill of sale, and "Коллегия", the College. A made thing
    // takes no article, and arrows come by the quiver ("колчан").
    constexpr const char* kDeed = "\xD0\xBA\xD1\x83\xD0\xBF\xD1\x87\xD0\xB0\xD1\x8F";
    constexpr const char* kCollege = "\xD0\x9A\xD0\xBE\xD0\xBB\xD0\xBB\xD0\xB5\xD0\xB3\xD0\xB8\xD1\x8F";
    constexpr const char* kQuiver = "\xD0\xBA\xD0\xBE\xD0\xBB\xD1\x87\xD0\xB0\xD0\xBD";
    const auto russian = NarrativeEngine::Testing::Synthetic(
        "ru",
        "",
        "",
        std::string{"[Data.Item]\nTG04GoldenglowDeed = "} + kDeed + "\n[Data.Faction]\ncollege = " + kCollege
            + "\n[Plot.Item]\nmade = {name}\nammo = " + kQuiver + " {name}\n");

    const auto world = PlotMenus::Translated(session, russian);

    SECTION("an item or faction the table has a row for takes the table's name")
    {
        REQUIRE(world.items.size() == 2);
        CHECK(world.items[0].displayName == kDeed);
        CHECK(world.items[0].form == 0x00040001);
        CHECK(world.items[0].editorId == "TG04GoldenglowDeed");
        REQUIRE(world.factions.size() == 2);
        CHECK(world.factions[0].name == kCollege);
        CHECK(world.factions[0].faction == 0x00030001);
    }
    SECTION("one it has no row for keeps the name it had")
    {
        CHECK(world.items[1].displayName == "a silver locket");
        CHECK(world.factions[1].name == "the Moonlit Order");
    }
    SECTION("settlements are left as the game spells them")
    {
        REQUIRE(world.settlements.size() == 1);
        CHECK(world.settlements[0].name == "Whiterun");
    }
    SECTION("a made thing keeps the game's name, phrased as the table phrases it")
    {
        REQUIRE(world.recipes.size() == 3);
        CHECK(world.recipes[0].displayName == "Iron Sword");
        CHECK(world.recipes[1].displayName == std::string{kQuiver} + " Elven Arrow");
        CHECK(world.recipes[2].displayName == "a Steel Sword");
        CHECK(session.recipes[0].displayName == "an Iron Sword");
    }
    SECTION("the world it was given, the session's, is untouched")
    {
        CHECK(session.items[0].displayName == "a deed of ownership");
        CHECK(session.factions[0].name == "the College of Winterhold");
    }
}

TEST_CASE("A trial birth leaves the live plot state as it found it", "[PlotTrial][engine]")
{
    EngineMock engine;
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    // Not JSON: the cast call is refused, retried, and refused again, after
    // a shortlist has been drawn and shuffled and a birth composed.
    const auto population = Village();
    const auto live = Live();
    const auto before = Live();
    const auto german = Localization::LoadShipped("german", Languages());
    REQUIRE(german.has_value());

    std::uint64_t rng = 42;
    const auto birth = OnPlotWorker([&](const NarrativeEngine::PlotThread::Token& pt) {
        return PlotTrial::TrialBirth(pt, population, live, rng, PlotMenus::CurrentWorld(), *german, Everyone());
    });

    SECTION("it composed, and was refused, as a real birth would be")
    {
        CHECK(birth.skipped.empty());
        CHECK_FALSE(birth.accepted);
        REQUIRE(birth.refusals.size() == 2);
        CHECK(birth.refusals[0].stage == "cast");
        CHECK(birth.refusals[0].code == NarrativeEngine::PlotRejection::Code::NotJson);
    }
    SECTION("it drew from its own stream")
    {
        CHECK(rng != 42);
    }
    SECTION("an answer cut off on its way back is asked again without spending the correction")
    {
        FakeLLM().Reset();
        CopyTo(FakeLLM().promptResponse, R"({"reasoning": "Olfrid has the motive", "choice": "0x000)");
        std::uint64_t again = 42;
        const auto cut = OnPlotWorker([&](const NarrativeEngine::PlotThread::Token& pt) {
            return PlotTrial::TrialBirth(pt, population, live, again, PlotMenus::CurrentWorld(), *german, Everyone());
        });

        // The cut-off, the same question asked again, and the correction.
        REQUIRE(cut.refusals.size() == 3);
        for (const auto& refusal : cut.refusals) {
            CHECK(refusal.stage == "cast");
            CHECK(refusal.code == NarrativeEngine::PlotRejection::Code::NotJson);
        }
    }
    SECTION("no plot, occupancy row, id, count or random draw of the live state moved")
    {
        CHECK(live.plots.size() == before.plots.size());
        CHECK(live.plots.front().id == before.plots.front().id);
        CHECK(live.occupancy.size() == before.occupancy.size());
        CHECK(live.nextPlotId == before.nextPlotId);
        CHECK(live.counters.plotsBorn == before.counters.plotsBorn);
        CHECK(live.rngState == before.rngState);
        CHECK(live.simGameDay == before.simGameDay);
    }
}
