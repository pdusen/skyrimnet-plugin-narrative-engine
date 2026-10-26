#include <Localization.h>
#include <PlotDispatch.h>
#include <PlotRejection.h>
#include <PlotTrial.h>
#include <SkyrimNetAPI.h>

#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>

// The language trial's two promises: a trial birth leaves the live plot state
// as it found it, and a trial's tables are its own, read beside the session's
// without touching it.

namespace
{
    namespace Localization = NarrativeEngine::Localization;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
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
        return PlotTrial::TrialBirth(pt, population, live, rng, *german, Everyone());
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
