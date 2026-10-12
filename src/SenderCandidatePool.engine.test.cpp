#include <SenderCandidatePool.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakePo3Tweaks.h>
#include <FakeSkyrimNet.h>
#include <SenderEditorID.h>
#include <SkyrimNetAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Tests for the shared sender pool.
//
// Both composers draw their senders from here, so an NPC wrongly dropped is an
// NPC who never writes and never visits for the rest of the save — and nothing
// says so. That makes the viability walk the part worth pinning: it drops the
// dead, the disabled, the nameless, the unresolvable, and anyone another
// quest's scripted content already owns, and each of those is a separate reason
// a candidate can vanish.
//
// The memory tail carries the other risk, and it is the one that has already
// gone wrong. The watermark filter — "memories older than the last time this
// NPC was a sender may not motivate a fresh one" — used to derive absolute
// in-world time from `age_hours`, which is REAL elapsed time. On a save resumed
// after a long break that produced a negative absolute time and silently
// dropped every memory the sender had, so the NPC was rejected for having
// nothing to say. The row's own `game_time` is what the comparison uses now,
// and the case below is written against that specific failure.
//
// Everything reaches the module through SkyrimNet, which the harness backs with
// a real stand-in DLL, so the fixtures below are the JSON SkyrimNet actually
// returns rather than a convenient shape.

namespace
{
    namespace Pool = NarrativeEngine::SenderCandidatePool;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr std::uint32_t kYsolda = 0x0001A6A0u;
    constexpr std::uint32_t kCarlotta = 0x0001A6A1u;
    // The blacklist cases need one candidate whose EditorID and display name
    // differ, or the two match arms are indistinguishable. The vanilla courier
    // is that: WICourierNPC, shown in game as "Courier".
    constexpr std::uint32_t kCourierRef = 0x0001A6A2u;
    constexpr std::uint32_t kCourierBase = 0x00039F83u;

    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void SetJson(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // One engagement row, in SkyrimNet's own shape.
    std::string EngagementRow(std::uint32_t formId, const char* name, double importance, double lastEvent)
    {
        return R"({"formId":)" + std::to_string(formId) + R"(,"name":")" + name + R"(","totalMemoryImportance":)"
               + std::to_string(importance) + R"(,"lastEventTime":)" + std::to_string(lastEvent) + "}";
    }

    // One memory row. `gameTime` is the absolute in-world reading; the module
    // derives a memory's age from it against the current game clock.
    std::string MemoryRow(const char* content, double importance, double gameTime, const char* emotion = "calm")
    {
        return R"({"type":"EXPERIENCE","content":")" + std::string{content} + R"(","importance_score":)"
               + std::to_string(importance) + R"(,"game_time":)" + std::to_string(gameTime) + R"(,"emotion":")"
               + emotion + R"(","location":"Whiterun"})";
    }

    Pool::BuildOptions DefaultOptions()
    {
        Pool::BuildOptions opts;
        // Deterministic order, so a case can say which candidate came back
        // rather than only how many.
        opts.shuffleResult = false;
        opts.maxCandidates = 12;
        opts.maxMemoriesPerCandidate = 6;
        opts.requireMemories = true;
        return opts;
    }

    bool Contains(const std::vector<Pool::Candidate>& pool, std::uint32_t formId)
    {
        return std::any_of(pool.begin(), pool.end(), [&](const Pool::Candidate& c) { return c.formId == formId; });
    }

    using NarrativeEngine::Testing::FakePo3TweaksState;
    using NarrativeEngine::Testing::FakePo3TweaksStateFunc;
    using NarrativeEngine::Testing::kFakePo3TweaksStateExport;

    // Loads the stand-in po3_Tweaks.dll for one section and unloads it after.
    // The pool finds Tweaks by module name, so a copy left loaded would answer
    // every later case, and the "Tweaks is not installed" case could not exist.
    class LoadedPo3Tweaks
    {
    public:
        LoadedPo3Tweaks() : module_(::LoadLibraryW(L"po3_Tweaks.dll"))
        {
            REQUIRE(module_ != nullptr);
            auto* accessor = reinterpret_cast<FakePo3TweaksStateFunc>(
                reinterpret_cast<void*>(::GetProcAddress(module_, kFakePo3TweaksStateExport)));
            REQUIRE(accessor != nullptr);
            state_ = accessor();
            REQUIRE(state_ != nullptr);
            state_->Reset();
        }

        ~LoadedPo3Tweaks()
        {
            ::FreeLibrary(module_);
        }

        LoadedPo3Tweaks(const LoadedPo3Tweaks&) = delete;
        LoadedPo3Tweaks& operator=(const LoadedPo3Tweaks&) = delete;

        FakePo3TweaksState& State() const
        {
            return *state_;
        }

    private:
        HMODULE module_;
        FakePo3TweaksState* state_ = nullptr;
    };

    // Lookup stand-ins for the resolution-order cases, which need no DLL: the
    // resolver takes the export as a plain function pointer.
    int g_cacheLookups = 0;
    std::uint32_t g_lastCacheLookup = 0;

    const char* CacheHoldingTheCourier(std::uint32_t formID)
    {
        ++g_cacheLookups;
        g_lastCacheLookup = formID;
        return formID == kCourierBase ? "WICourierNPC" : nullptr;
    }

    const char* CacheAnsweringEmpty(std::uint32_t formID)
    {
        ++g_cacheLookups;
        g_lastCacheLookup = formID;
        return "";
    }
} // namespace

TEST_CASE("SenderCandidatePool::Build walks for viability", "[SenderCandidatePool][engine]")
{
    // Happy path, re-run per leaf: SkyrimNet up with its memory system ready,
    // one live named actor with one memory worth telling. Each case removes
    // one piece of that.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.", 0.8, 50.0) + "]");

    SECTION("when the candidate is viable")
    {
        const auto pool = Pool::Build(DefaultOptions());

        SECTION("should return them")
        {
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].formId == kYsolda);
            REQUIRE(pool[0].name == "Ysolda");
        }

        SECTION("should carry the engagement figures through")
        {
            // The composer ranks on these, so a pool that dropped them would
            // still look full and choose at random.
            REQUIRE(pool[0].engagementScore == 12.5);
            REQUIRE(pool[0].lastInteractedAt == 100.0);
        }

        SECTION("should attach their memory tail")
        {
            REQUIRE(pool[0].memories.is_array());
            REQUIRE(pool[0].memories.size() == 1);
            REQUIRE(pool[0].memories[0].value("content", "") == "She spoke of the mammoth tusk.");
        }
    }

    SECTION("when the form does not resolve")
    {
        SetJson(fake.engagementJson, "[" + EngagementRow(0x00DEAD01u, "Ghost", 5.0, 1.0) + "]");

        SECTION("should drop them")
        {
            // A plugin removed since the memories were written. The engagement
            // row survives in SkyrimNet's database; the NPC does not.
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when the actor is dead")
    {
        engine.forms.actorIsDead = true;

        SECTION("should drop them")
        {
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when the actor is disabled")
    {
        engine.forms.actorIsDisabled = true;

        SECTION("should drop them")
        {
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when the engagement row has no name")
    {
        SetJson(fake.engagementJson, R"([{"formId":)" + std::to_string(kYsolda) + R"(,"totalMemoryImportance":5.0}])");

        SECTION("should drop them")
        {
            // The name is how the LLM refers to the candidate at all. An
            // unnamed one would reach the prompt as an empty string.
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when another quest already owns the actor")
    {
        // Filled into a foreign running quest's alias, with a reserved hold.
        EngineMock::AliasState::Instance instance;
        instance.quest.sourceFile = "SomeOtherMod.esp";
        instance.quest.running = true;
        instance.reserves = true;
        engine.aliases.instances = {instance};
        engine.aliases.arrayPresent = true;
        engine.FillAliasInstances(nullptr);

        SECTION("should drop them")
        {
            // Competing with authored content means two systems driving one
            // NPC, and our own fill would be silently skipped anyway.
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when the caller's own filter rejects them")
    {
        auto opts = DefaultOptions();
        opts.extraViabilityFilter = [](RE::Actor*, std::string* reason) {
            if (reason)
                *reason = "not-unique";
            return false;
        };

        SECTION("should drop them")
        {
            // Runs after the universal walk, so the callback only has to know
            // the caller's own rules.
            REQUIRE(Pool::Build(opts).empty());
        }
    }

    SECTION("when the caller's own filter accepts them")
    {
        auto opts = DefaultOptions();
        opts.extraViabilityFilter = [](RE::Actor*, std::string*) { return true; };

        SECTION("should keep them")
        {
            REQUIRE(Pool::Build(opts).size() == 1);
        }
    }
}

TEST_CASE("SenderCandidatePool::ResolveSenderEditorID", "[SenderCandidatePool][SenderEditorID][engine]")
{
    // The order the blacklist's EditorID is looked for in. The engine's own
    // answer is preferred whenever it has one; powerofthree's Tweaks is asked
    // only when the engine discarded it, and only for the base form.
    g_cacheLookups = 0;
    g_lastCacheLookup = 0;

    SECTION("when the engine still has the EditorID")
    {
        const auto resolved = Pool::ResolveSenderEditorID("WICourierNPC", kCourierBase, CacheHoldingTheCourier);

        SECTION("should use it")
        {
            REQUIRE(resolved.value == "WICourierNPC");
            REQUIRE(std::string_view{resolved.source} == "native");
        }

        SECTION("should never ask the cache")
        {
            REQUIRE(g_cacheLookups == 0);
        }
    }

    SECTION("when the engine's EditorID is empty")
    {
        const auto resolved = Pool::ResolveSenderEditorID("", kCourierBase, CacheHoldingTheCourier);

        SECTION("should take the one the cache holds")
        {
            REQUIRE(resolved.value == "WICourierNPC");
            REQUIRE(std::string_view{resolved.source} == "po3-cache");
        }

        SECTION("should ask the cache for the base form it was given")
        {
            REQUIRE(g_cacheLookups == 1);
            REQUIRE(g_lastCacheLookup == kCourierBase);
        }
    }

    SECTION("when the engine's EditorID is null")
    {
        SECTION("should take the one the cache holds")
        {
            const auto resolved = Pool::ResolveSenderEditorID(nullptr, kCourierBase, CacheHoldingTheCourier);
            REQUIRE(resolved.value == "WICourierNPC");
            REQUIRE(std::string_view{resolved.source} == "po3-cache");
        }
    }

    SECTION("when the cache never stored the form")
    {
        SECTION("should leave the EditorID unresolved")
        {
            const auto resolved = Pool::ResolveSenderEditorID("", kYsolda, CacheHoldingTheCourier);
            REQUIRE(resolved.value.empty());
            REQUIRE(std::string_view{resolved.source} == "unresolved");
        }
    }

    SECTION("when the cache answers with an empty string")
    {
        SECTION("should leave the EditorID unresolved")
        {
            const auto resolved = Pool::ResolveSenderEditorID("", kCourierBase, CacheAnsweringEmpty);
            REQUIRE(resolved.value.empty());
            REQUIRE(std::string_view{resolved.source} == "unresolved");
        }
    }

    SECTION("when there is no cache to ask")
    {
        SECTION("should leave the EditorID unresolved")
        {
            const auto resolved = Pool::ResolveSenderEditorID("", kCourierBase, nullptr);
            REQUIRE(resolved.value.empty());
            REQUIRE(std::string_view{resolved.source} == "unresolved");
        }
    }
}

TEST_CASE("SenderCandidatePool::Build shapes the memory tail", "[SenderCandidatePool][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "]");
    // Two days of game time on the clock, so memories have somewhere to sit
    // relative to now. The module reads cumulative HOURS, not days.
    engine.calendar.hoursPassed = 48.0f;

    SECTION("when memories arrive out of order")
    {
        SetJson(fake.memoriesJson,
                "[" + MemoryRow("newest", 0.9, 40.0) + "," + MemoryRow("oldest", 0.9, 10.0) + ","
                    + MemoryRow("middle", 0.9, 25.0) + "]");

        SECTION("should present them oldest first")
        {
            // The tail is read by the model as a running narrative, so the
            // order is the story. Reversed, an NPC's motivation reads as its
            // own consequence.
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 3);
            REQUIRE(pool[0].memories[0].value("content", "") == "oldest");
            REQUIRE(pool[0].memories[2].value("content", "") == "newest");
        }
    }

    SECTION("when there are more memories than the cap")
    {
        std::string rows = "[";
        for (int i = 0; i < 10; ++i) {
            rows += MemoryRow(("memory" + std::to_string(i)).c_str(), 0.9, 10.0 + i);
            if (i < 9)
                rows += ",";
        }
        rows += "]";
        SetJson(fake.memoriesJson, rows);
        auto opts = DefaultOptions();
        opts.maxMemoriesPerCandidate = 3;

        SECTION("should keep the most recent rather than the first it read")
        {
            // Truncating the newest would leave the model reasoning from what
            // the NPC has already stopped caring about.
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 3);
            REQUIRE(pool[0].memories[2].value("content", "") == "memory9");
        }
    }

    SECTION("when a memory falls under the importance threshold")
    {
        SetJson(fake.memoriesJson,
                "[" + MemoryRow("trivial", 0.1, 20.0) + "," + MemoryRow("significant", 0.9, 30.0) + "]");
        auto opts = DefaultOptions();
        opts.memoryImportanceThreshold = 0.5;

        SECTION("should drop it")
        {
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 1);
            REQUIRE(pool[0].memories[0].value("content", "") == "significant");
        }
    }

    SECTION("when the caller excludes diary entries")
    {
        SetJson(fake.memoriesJson,
                "[" + MemoryRow("Diary Entry: I felt uneasy today.", 0.9, 20.0) + ","
                    + MemoryRow("She paid her debt.", 0.9, 30.0) + "]");
        auto opts = DefaultOptions();
        opts.excludeDiaryEntries = true;

        SECTION("should drop it")
        {
            // Diaries are first-person voice: useful at compose time for the
            // model to imitate, clutter at select time when the question is
            // only whether anything happened worth writing about.
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 1);
            REQUIRE(pool[0].memories[0].value("content", "") == "She paid her debt.");
        }
    }

    SECTION("when the caller keeps diary entries")
    {
        SetJson(fake.memoriesJson, "[" + MemoryRow("Diary Entry: I felt uneasy today.", 0.9, 20.0) + "]");
        auto opts = DefaultOptions();
        opts.excludeDiaryEntries = false;

        SECTION("should keep it")
        {
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 1);
        }
    }

    SECTION("when nothing survives the filters and memories are required")
    {
        SetJson(fake.memoriesJson, "[" + MemoryRow("trivial", 0.1, 20.0) + "]");
        auto opts = DefaultOptions();
        opts.memoryImportanceThreshold = 0.5;
        opts.requireMemories = true;

        SECTION("should drop the candidate entirely")
        {
            // A letter with no memory behind it has nothing to be about.
            REQUIRE(Pool::Build(opts).empty());
        }
    }

    SECTION("when nothing survives and memories are optional")
    {
        SetJson(fake.memoriesJson, "[]");
        auto opts = DefaultOptions();
        opts.requireMemories = false;

        SECTION("should keep the candidate")
        {
            // Visits work from live actor context rather than a memory-driven
            // brief, so an empty tail is not a disqualification for them.
            REQUIRE(Pool::Build(opts).size() == 1);
        }
    }
}

TEST_CASE("SenderCandidatePool::Build applies the sender watermark", "[SenderCandidatePool][engine]")
{
    // The filter this module got wrong once. A watermark says "this NPC has
    // already been a sender; memories older than that use may not motivate a
    // fresh beat with them". It used to derive absolute in-world time from
    // `age_hours`, which is REAL elapsed time — so a save resumed after a long
    // real-world break produced a negative absolute time and dropped every
    // memory the sender had, leaving them permanently unable to write.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "]");
    // Ten game days on the clock: 240 in-world hours.
    engine.calendar.hoursPassed = 240.0f;
    // One memory from hour 100, one from hour 200, in game seconds.
    SetJson(fake.memoriesJson,
            "[" + MemoryRow("before the last letter", 0.9, 100.0 * 3600.0) + ","
                + MemoryRow("since the last letter", 0.9, 200.0 * 3600.0) + "]");

    SECTION("when a watermark is set")
    {
        auto opts = DefaultOptions();
        opts.memoryWatermarkProvider = [](RE::FormID) { return std::optional<double>{150.0}; };

        SECTION("should drop only what predates it")
        {
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 1);
            REQUIRE(pool[0].memories[0].value("content", "") == "since the last letter");
        }
    }

    SECTION("when the watermark predates every memory")
    {
        auto opts = DefaultOptions();
        opts.memoryWatermarkProvider = [](RE::FormID) { return std::optional<double>{1.0}; };

        SECTION("should keep them all")
        {
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 2);
        }
    }

    SECTION("when the provider declines to give one")
    {
        auto opts = DefaultOptions();
        opts.memoryWatermarkProvider = [](RE::FormID) { return std::optional<double>{}; };

        SECTION("should filter nothing")
        {
            // This NPC has never been a sender, which is the common case and
            // must not be treated as a watermark of zero or of now.
            const auto pool = Pool::Build(opts);
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 2);
        }
    }

    SECTION("when there is no provider at all")
    {
        SECTION("should filter nothing")
        {
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(pool[0].memories.size() == 2);
        }
    }
}

TEST_CASE("SenderCandidatePool::FetchMemories", "[SenderCandidatePool][engine]")
{
    // For a caller that already knows its sender. It used to get the tail by
    // running a pool build filtered down to that one actor, and the build only
    // walks the first few engagement rows -- so a sender ranked below them
    // came back with no memories at all, and the visit composer wrote the
    // whole visit with nothing of the sender's own to go on.
    //
    // Happy path, re-run per leaf: SkyrimNet up, the sender holding two
    // memories worth telling, and nobody at all in the engagement list.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    engine.calendar.hoursPassed = 48.0f;
    SetJson(fake.memoriesJson, "[" + MemoryRow("newer", 0.9, 40.0) + "," + MemoryRow("older", 0.9, 10.0) + "]");

    SECTION("when the sender ranks nowhere in the engagement list")
    {
        const auto memories = Pool::FetchMemories(kYsolda, DefaultOptions());

        SECTION("should still return their memories")
        {
            REQUIRE(memories.size() == 2);
        }

        SECTION("should ask SkyrimNet about that sender")
        {
            REQUIRE(fake.lastFormID == kYsolda);
        }

        SECTION("should not walk the engagement list at all")
        {
            REQUIRE(fake.engagementCalls == 0);
        }

        SECTION("should present them oldest first, as a build does")
        {
            REQUIRE(memories[0].value("content", "") == "older");
            REQUIRE(memories[1].value("content", "") == "newer");
        }
    }

    SECTION("when a memory falls under the importance threshold")
    {
        SetJson(fake.memoriesJson,
                "[" + MemoryRow("worth telling", 0.9, 40.0) + "," + MemoryRow("idle chatter", 0.1, 30.0) + "]");
        auto opts = DefaultOptions();
        opts.memoryImportanceThreshold = 0.5;

        SECTION("should drop it by the same rule a build uses")
        {
            const auto memories = Pool::FetchMemories(kYsolda, opts);
            REQUIRE(memories.size() == 1);
            REQUIRE(memories[0].value("content", "") == "worth telling");
        }
    }

    SECTION("when there are more memories than the cap")
    {
        auto opts = DefaultOptions();
        opts.maxMemoriesPerCandidate = 1;

        SECTION("should keep the most recent")
        {
            const auto memories = Pool::FetchMemories(kYsolda, opts);
            REQUIRE(memories.size() == 1);
            REQUIRE(memories[0].value("content", "") == "newer");
        }
    }

    SECTION("when the sender has a watermark")
    {
        // Hours, against a clock of 48: only the memory at hour 40 is newer.
        SetJson(fake.memoriesJson,
                "[" + MemoryRow("since", 0.9, 40.0 * 3600.0) + "," + MemoryRow("before", 0.9, 10.0 * 3600.0) + "]");
        auto opts = DefaultOptions();
        opts.memoryWatermarkProvider = [](RE::FormID) { return std::optional<double>{20.0}; };

        SECTION("should drop what predates it")
        {
            const auto memories = Pool::FetchMemories(kYsolda, opts);
            REQUIRE(memories.size() == 1);
            REQUIRE(memories[0].value("content", "") == "since");
        }
    }

    SECTION("when SkyrimNet holds nothing for the sender")
    {
        SetJson(fake.memoriesJson, "[]");

        SECTION("should return an empty list rather than nothing")
        {
            const auto memories = Pool::FetchMemories(kYsolda, DefaultOptions());
            REQUIRE(memories.is_array());
            REQUIRE(memories.empty());
        }
    }

    SECTION("when the memory database is still rebuilding")
    {
        fake.memorySystemReady = false;

        SECTION("should return an empty list without asking")
        {
            const auto memories = Pool::FetchMemories(kYsolda, DefaultOptions());
            REQUIRE(memories.empty());
            REQUIRE(fake.memoriesForActorCalls == 0);
        }
    }
}

TEST_CASE("SenderCandidatePool::Build caps and orders the pool", "[SenderCandidatePool][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    (void)engine.AddActor(kCarlotta);
    SetJson(fake.engagementJson,
            "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "," + EngagementRow(kCarlotta, "Carlotta", 9.0, 90.0)
                + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("something happened", 0.9, 50.0) + "]");

    SECTION("when several candidates are viable")
    {
        SECTION("should return all of them")
        {
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 2);
            REQUIRE(Contains(pool, kYsolda));
            REQUIRE(Contains(pool, kCarlotta));
        }
    }

    SECTION("when the cap is lower than the viable count")
    {
        auto opts = DefaultOptions();
        opts.maxCandidates = 1;

        SECTION("should return no more than the cap")
        {
            REQUIRE(Pool::Build(opts).size() == 1);
        }
    }
}

TEST_CASE("SenderCandidatePool without SkyrimNet", "[SenderCandidatePool][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    SetJson(fake.engagementJson, "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("something happened", 0.9, 50.0) + "]");

    SECTION("when the memory database is still rebuilding")
    {
        fake.memorySystemReady = false;

        SECTION("should return an empty pool rather than a wrong one")
        {
            // The database rebuilds asynchronously for seconds after every
            // load. Building a pool from what it answers during that window
            // would rank on partial data and look like a working choice.
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when the engagement answer is unparseable")
    {
        SetJson(fake.engagementJson, "not json at all");

        SECTION("should return an empty pool")
        {
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when nobody has engaged with the player yet")
    {
        SetJson(fake.engagementJson, "[]");

        SECTION("should return an empty pool")
        {
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }
}

TEST_CASE("SenderCandidatePool::CountViable", "[SenderCandidatePool][engine]")
{
    // The cheap availability check: the same walk, without the per-candidate
    // memory fetch, stopping as soon as enough have been seen. It runs on every
    // beat-availability poll, so doing the full build here would put an LLM
    // round trip's worth of work on a tick that usually declines.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    (void)engine.AddActor(kYsolda);
    (void)engine.AddActor(kCarlotta);
    SetJson(fake.engagementJson,
            "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "," + EngagementRow(kCarlotta, "Carlotta", 9.0, 90.0)
                + "]");

    SECTION("when enough candidates are viable")
    {
        SECTION("should count them")
        {
            REQUIRE(Pool::CountViable(nullptr, 2) >= 2);
        }

        SECTION("should fetch no memories")
        {
            // The whole reason this exists rather than calling Build and
            // measuring its size.
            fake.memoriesForActorCalls = 0;
            (void)Pool::CountViable(nullptr, 2);
            REQUIRE(fake.memoriesForActorCalls == 0);
        }
    }

    SECTION("when a filter rejects everyone")
    {
        SECTION("should count none")
        {
            REQUIRE(Pool::CountViable([](RE::Actor*, std::string*) { return false; }, 2) == 0);
        }
    }

    SECTION("when the actors are all dead")
    {
        engine.forms.actorIsDead = true;

        SECTION("should count none")
        {
            REQUIRE(Pool::CountViable(nullptr, 2) == 0);
        }
    }
}

TEST_CASE("SenderCandidatePool::Build honours the sender blacklist", "[SenderCandidatePool][engine]")
{
    // The blacklist is the one gate a player sets by hand, which makes it the
    // one that has to hold without them being able to see it hold. It lives in
    // the universal walk rather than a caller's extra filter so that both
    // composers and the availability count are covered by the same test.
    //
    // An entry is tested against the base form's EditorID and against the
    // display name, because Skyrim SE discards most EditorIDs at runtime: with
    // an EditorID-recovery mod installed the first arm carries everything, and
    // without one it never fires at all and the second carries everything.
    // Both arms therefore matter on some install, so both are covered here.
    EngineMock engine;
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    auto* courier = engine.AddActor(kCourierRef);
    auto* courierBase = engine.AddNPC(kCourierBase, "Courier");
    engine.SetEditorIDOf(courierBase, "WICourierNPC");
    engine.SetActorBase(courier, courierBase);
    // Ysolda is left without a base form on purpose — an actor whose
    // GetActorBase() is null must still be testable against the name arm
    // rather than skipping the gate.
    (void)engine.AddActor(kYsolda);

    SetJson(fake.engagementJson,
            "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "," + EngagementRow(kCourierRef, "Courier", 9.0, 90.0)
                + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.", 0.8, 50.0) + "]");

    SECTION("when the blacklist is empty")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};

        SECTION("should return everyone viable")
        {
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 2);
            REQUIRE(Contains(pool, kYsolda));
            REQUIRE(Contains(pool, kCourierRef));
        }
    }

    SECTION("when an entry names a candidate's display name")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=Ysolda\n"};

        SECTION("should drop that candidate and keep the rest")
        {
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(Contains(pool, kCourierRef));
        }
    }

    SECTION("when an entry names a candidate's base-form EditorID")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=WICourierNPC\n"};

        SECTION("should drop that candidate and keep the rest")
        {
            // Nothing the courier's engagement row says is "WICourierNPC" —
            // the row calls them "Courier" — so a pass here is the EditorID
            // arm and cannot be the name arm firing by coincidence.
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(Contains(pool, kYsolda));
        }
    }

    SECTION("when an entry differs in case and carries surrounding whitespace")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=  ySOLda  \n"};

        SECTION("should still drop that candidate")
        {
            // Hand-written by a player in a text editor. Neither the casing
            // they used nor the spaces they left around the comma are a
            // statement about who they meant.
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(Contains(pool, kCourierRef));
        }
    }

    SECTION("when the list names several NPCs")
    {
        const ConfiguredSettings settings{
            "[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=Ysolda; WICourierNPC\n"};

        SECTION("should drop every one of them")
        {
            // Both arms at once, which is the shape a real list has.
            REQUIRE(Pool::Build(DefaultOptions()).empty());
        }
    }

    SECTION("when an entry matches nobody")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=Nazeem\n"};

        SECTION("should keep everyone")
        {
            // A blacklist naming an NPC the player has not met is the ordinary
            // case, not an error, and it must not cost anyone else their slot.
            REQUIRE(Pool::Build(DefaultOptions()).size() == 2);
        }
    }

    SECTION("when a blacklisted candidate would be counted for availability")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=Ysolda\n"};

        SECTION("should not count them")
        {
            // The gate has to hold on this path too. A beat that counts a
            // candidate it may not use reports itself available, spends a
            // compose LLM call, and then has nobody to send.
            REQUIRE(Pool::CountViable(nullptr, 2) == 1);
        }
    }
}

TEST_CASE("SenderCandidatePool::Build reads EditorIDs powerofthree's Tweaks retained",
          "[SenderCandidatePool][SenderEditorID][engine]")
{
    // The install the EditorID arm is documented for: powerofthree's Tweaks is
    // present, and the engine's own getter is still empty for every NPC,
    // because Tweaks keeps NPC EditorIDs in its own cache rather than giving
    // them back to the engine. The courier's base form is deliberately left
    // without an engine-side EditorID here, so a match can only come from the
    // cache, through the same GetModuleHandle lookup the game takes.
    EngineMock engine;
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    auto* courier = engine.AddActor(kCourierRef);
    auto* courierBase = engine.AddNPC(kCourierBase, "Courier");
    engine.SetActorBase(courier, courierBase);
    (void)engine.AddActor(kYsolda);

    SetJson(fake.engagementJson,
            "[" + EngagementRow(kYsolda, "Ysolda", 12.5, 100.0) + "," + EngagementRow(kCourierRef, "Courier", 9.0, 90.0)
                + "]");
    SetJson(fake.memoriesJson, "[" + MemoryRow("She spoke of the mammoth tusk.", 0.8, 50.0) + "]");

    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n[Beats]\nsBlacklistedSenders=WICourierNPC\n"};

    SECTION("when Tweaks holds the courier's EditorID")
    {
        const LoadedPo3Tweaks tweaks;
        tweaks.State().Add(kCourierBase, "WICourierNPC");

        SECTION("should drop the courier and keep the rest")
        {
            // The row calls the courier "Courier", so this cannot be the name
            // arm firing; before the cache was read, the courier stayed in.
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(Contains(pool, kYsolda));
        }

        SECTION("should ask Tweaks for the base form, not the placed reference")
        {
            // The cache is keyed by the record that carries the EditorID. The
            // reference has none, so asking with its FormID finds nothing.
            (void)Pool::Build(DefaultOptions());
            REQUIRE(tweaks.State().AskedFor(kCourierBase));
            REQUIRE_FALSE(tweaks.State().AskedFor(kCourierRef));
        }

        SECTION("should not count the courier for availability either")
        {
            REQUIRE(Pool::CountViable(nullptr, 2) == 1);
        }
    }

    SECTION("when Tweaks never stored the courier's EditorID")
    {
        const LoadedPo3Tweaks tweaks;

        SECTION("should keep the courier")
        {
            REQUIRE(Pool::Build(DefaultOptions()).size() == 2);
        }
    }

    SECTION("when Tweaks is not loaded")
    {
        SECTION("should keep the courier, as there is nowhere left to find the EditorID")
        {
            // Without Tweaks the documented remedy is the display name; the
            // EditorID entry degrades to matching nobody rather than failing.
            REQUIRE(::GetModuleHandleW(L"po3_Tweaks.dll") == nullptr);
            REQUIRE(Pool::Build(DefaultOptions()).size() == 2);
        }
    }

    SECTION("when the engine still has the courier's EditorID")
    {
        const LoadedPo3Tweaks tweaks;
        engine.SetEditorIDOf(courierBase, "WICourierNPC");

        SECTION("should match it without asking Tweaks")
        {
            const auto pool = Pool::Build(DefaultOptions());
            REQUIRE(pool.size() == 1);
            REQUIRE(Contains(pool, kYsolda));
            // Only the courier is pinned here: whether another candidate's
            // base needed the cache depends on fixtures this case does not own.
            REQUIRE_FALSE(tweaks.State().AskedFor(kCourierBase));
        }
    }
}
