#include <SkyrimNetAPI.h>

#include <AsyncDispatch.h>
#include <FakeSkyrimNet.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>

// Tests for the wrapper over SkyrimNet's soft-loaded public C API.
//
// Two things here are worth more than the rest.
//
// The first is the nullptr-on-empty convention. Four arguments across three
// endpoints — the event-type filter, the memory context query, and AddMemory's
// tags and related-actors JSON — must be handed to SkyrimNet as nullptr rather
// than as an empty C-string. An empty string routes SkyrimNet through its
// JSON-parse branch, which either crashes or silently drops the write, and both
// failures land far from the call that caused them. The fake records which of
// the two it was given, so the distinction is checkable at all.
//
// The second is the memory-system gate. SkyrimNet's own header says querying
// before the database is ready crashes inside SkyrimNet, and the database
// rebuilds asynchronously for some seconds after a load — so this is a state a
// real session passes through every single time, not an edge case.
//
// As with PrismaUI there is no seam: the upstream header does
// LoadLibraryA("SkyrimNet") and a GetProcAddress per export, inline. The
// harness therefore builds a real DLL of that name beside the test executable
// (testsupport/FakeSkyrimNet.cpp, the SkyrimNetFake target) and lets the
// wrapper resolve against it. Initialize is one-shot per process, so the case
// that needs an unresolved wrapper skips itself if something got there first;
// under ctest every TEST_CASE has its own process and none of them skip.

namespace
{
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr auto kTimeout = std::chrono::seconds{5};
    constexpr std::uint32_t kActorFormID = 0x0001A6A0u;

    template <class T> bool Arrived(std::future<T>& f)
    {
        return f.wait_for(kTimeout) == std::future_status::ready;
    }

    // The fake's answers live in fixed buffers it owns, so a test that wants a
    // different answer writes into one rather than handing over a pointer.
    template <std::size_t N> void CopyJson(char (&dest)[N], const char* text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && text[i] != '\0'; ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // Reaches the fake's state. The DLL is already beside the executable, so
    // LoadLibrary here only bumps a refcount and gives us a handle to resolve
    // the state accessor through.
    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    // A worker token, which the synchronous LLM call is gated on. Obtained the
    // way production does, through the dispatcher.
    template <class Fn> void OnPluginThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) { body(pt); });
    }
} // namespace

TEST_CASE("SkyrimNetAPI before it has resolved anything", "[SkyrimNetAPI][engine]")
{
    // The only case that needs unresolved function pointers, and Initialize
    // cannot be undone. Skipping rather than asserting keeps a shuffled
    // single-process run honest.
    if (SkyrimNet::IsAvailable()) {
        SKIP("SkyrimNet was already initialised in this process; run under ctest, which isolates each case");
    }

    SECTION("when SkyrimNet is not installed")
    {
        SECTION("should say it is unavailable")
        {
            REQUIRE_FALSE(SkyrimNet::IsAvailable());
        }

        SECTION("should report no version")
        {
            // -1 rather than 0: the dashboard's status pill renders this, and 0
            // is a version number a real SkyrimNet could report.
            REQUIRE(SkyrimNet::GetVersion() == -1);
        }

        SECTION("should report the memory system as not ready")
        {
            // Every memory endpoint is gated on this, so answering true with no
            // SkyrimNet would send each of them straight into a null pointer.
            REQUIRE_FALSE(SkyrimNet::IsMemorySystemReady());
        }

        SECTION("should return an empty array from every query")
        {
            // "[]" rather than "" because every caller parses the result as
            // JSON; an empty string throws where an empty array reads as "this
            // actor has nothing on file".
            REQUIRE(SkyrimNet::GetRecentEvents(kActorFormID, 10, "") == "[]");
            REQUIRE(SkyrimNet::GetMemoriesForActor(kActorFormID, 10, "") == "[]");
            REQUIRE(SkyrimNet::QueryMemoriesForActor(kActorFormID, MemoryQuery{}) == "[]");
            REQUIRE(SkyrimNet::GetRecentDialogue(kActorFormID, 10) == "[]");
            REQUIRE(SkyrimNet::GetActorEngagement(10, true, false, 86400.0, 604800.0) == "[]");
        }

        SECTION("should refuse to write a memory")
        {
            REQUIRE(SkyrimNet::AddMemory(kActorFormID, "text", 0.5f, "EXPERIENCE", "calm", "Whiterun") == -1);
        }

        SECTION("should resolve no UUID")
        {
            REQUIRE(SkyrimNet::FormIDToUUID(kActorFormID) == 0);
        }

        SECTION("should refuse to register a decorator")
        {
            REQUIRE_FALSE(SkyrimNet::RegisterDecorator("ne_test", "d", [](RE::Actor*) { return std::string{}; }));
            REQUIRE_FALSE(SkyrimNet::HasDecorator("ne_test"));
        }

        SECTION("should refuse to queue an LLM call")
        {
            REQUIRE_FALSE(SkyrimNet::SendCustomPromptToLLMForeign("p", "v", "{}", [](std::string, bool) {}));
        }

        SECTION("should fail a synchronous LLM call rather than block on it")
        {
            // The sharp one. The synchronous overload waits on a promise the
            // callback pokes, so a wrapper that queued nothing and waited
            // anyway would hang the plugin thread for the rest of the session.
            SkyrimNet::LLMResult result;
            OnPluginThread(
                [&](const PluginThread::Token& pt) { result = SkyrimNet::SendCustomPromptToLLM(pt, "p", "v", "{}"); });
            REQUIRE_FALSE(result.ok);
            REQUIRE_FALSE(result.response.empty());
        }
    }
}

TEST_CASE("SkyrimNetAPI::Initialize", "[SkyrimNetAPI][engine]")
{
    // Deliberately does not reset the fake's version record: FindFunctions runs
    // once per process, and that record is the only evidence it happened.
    auto& fake = FakeState();

    SECTION("when SkyrimNet is installed")
    {
        const bool ok = SkyrimNet::Initialize();

        SECTION("should report success")
        {
            REQUIRE(ok);
        }

        SECTION("should then say it is available")
        {
            REQUIRE(SkyrimNet::IsAvailable());
        }

        SECTION("should report the version SkyrimNet gave")
        {
            REQUIRE(SkyrimNet::GetVersion() == fake.version);
        }
    }

    SECTION("when Initialize is called again")
    {
        REQUIRE(SkyrimNet::Initialize());
        const int afterFirst = fake.versionQueries;

        SECTION("should reuse the cached availability")
        {
            // kDataLoaded can arrive more than once in a session; re-resolving
            // every export each time would be pointless work at a moment the
            // game is already busy.
            REQUIRE(SkyrimNet::Initialize());
            REQUIRE(fake.versionQueries == afterFirst);
        }
    }
}

TEST_CASE("SkyrimNetAPI passes empty arguments as null", "[SkyrimNetAPI][engine]")
{
    // The highest-value case in the file. SkyrimNet documents nullptr as "no
    // filter" / "no tags"; an empty C-string instead routes through its
    // JSON-parse branch, which crashes or silently drops the write. Nothing at
    // the call site distinguishes the two, and nothing in the log does either.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when the event filter is empty")
    {
        SECTION("should hand SkyrimNet nothing rather than an empty string")
        {
            (void)SkyrimNet::GetRecentEvents(kActorFormID, 10, "");
            REQUIRE(fake.recentEventsCalls == 1);
            REQUIRE(std::string{fake.lastEventFilter}.empty());
        }
    }

    SECTION("when the event filter is set")
    {
        SECTION("should pass it through")
        {
            (void)SkyrimNet::GetRecentEvents(kActorFormID, 10, "dialogue");
            REQUIRE(std::string{fake.lastEventFilter} == "dialogue");
        }
    }

    SECTION("when the memory context query is empty")
    {
        SECTION("should hand SkyrimNet nothing rather than an empty string")
        {
            // Empty means "rank by recency"; SkyrimNet takes that path only on
            // null.
            (void)SkyrimNet::GetMemoriesForActor(kActorFormID, 10, "");
            REQUIRE(fake.memoriesForActorCalls == 1);
            REQUIRE(std::string{fake.lastContextQuery}.empty());
        }
    }

    SECTION("when the memory context query is set")
    {
        SECTION("should pass it through")
        {
            (void)SkyrimNet::GetMemoriesForActor(kActorFormID, 10, "the debt");
            REQUIRE(std::string{fake.lastContextQuery} == "the debt");
        }
    }

    SECTION("when a memory carries no tags or related actors")
    {
        SECTION("should hand SkyrimNet null for both")
        {
            // Checked as null rather than as empty text, because that is the
            // whole distinction: an empty string here is a silently dropped
            // write, and the memory simply never appears.
            (void)SkyrimNet::AddMemory(kActorFormID, "text", 0.5f, "EXPERIENCE", "calm", "Whiterun");
            REQUIRE(fake.addMemoryCalls == 1);
            REQUIRE(fake.lastTagsWereNull);
            REQUIRE(fake.lastRelatedActorsWereNull);
        }
    }

    SECTION("when a memory carries tags and related actors")
    {
        SECTION("should pass both through as given")
        {
            (void)SkyrimNet::AddMemory(
                kActorFormID, "text", 0.5f, "EXPERIENCE", "calm", "Whiterun", R"(["debt"])", "[20]");
            REQUIRE_FALSE(fake.lastTagsWereNull);
            REQUIRE_FALSE(fake.lastRelatedActorsWereNull);
            REQUIRE(std::string{fake.lastTagsJson} == R"(["debt"])");
            REQUIRE(std::string{fake.lastRelatedActorsJson} == "[20]");
        }
    }
}

TEST_CASE("SkyrimNetAPI waits for the memory system", "[SkyrimNetAPI][engine]")
{
    // SkyrimNet's own header says querying before the database is ready crashes
    // inside SkyrimNet, and the database rebuilds asynchronously for seconds
    // after every load — so this is a state a real session passes through every
    // time, not an edge case.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    fake.memorySystemReady = false;

    SECTION("when the database is still rebuilding")
    {
        SECTION("should say so")
        {
            REQUIRE_FALSE(SkyrimNet::IsMemorySystemReady());
        }

        SECTION("should ask SkyrimNet nothing about events")
        {
            // The counter is what makes this real. Asserting only on the "[]"
            // would pass with the gate deleted, because the fake also answers
            // "[]" by default.
            REQUIRE(SkyrimNet::GetRecentEvents(kActorFormID, 10, "dialogue") == "[]");
            REQUIRE(fake.recentEventsCalls == 0);
        }

        SECTION("should ask SkyrimNet nothing about memories")
        {
            REQUIRE(SkyrimNet::GetMemoriesForActor(kActorFormID, 10, "q") == "[]");
            REQUIRE(SkyrimNet::QueryMemoriesForActor(kActorFormID, MemoryQuery{}) == "[]");
            REQUIRE(fake.memoriesForActorCalls == 0);
            REQUIRE(fake.queryMemoriesCalls == 0);
        }

        SECTION("should ask SkyrimNet nothing about dialogue or engagement")
        {
            REQUIRE(SkyrimNet::GetRecentDialogue(kActorFormID, 10) == "[]");
            REQUIRE(SkyrimNet::GetActorEngagement(10, true, false, 86400.0, 604800.0) == "[]");
            REQUIRE(fake.recentDialogueCalls == 0);
            REQUIRE(fake.engagementCalls == 0);
        }

        SECTION("should refuse to write a memory")
        {
            // Writing into a database that is still rebuilding is the one call
            // here that would lose data rather than merely return nothing.
            REQUIRE(SkyrimNet::AddMemory(kActorFormID, "text", 0.5f, "EXPERIENCE", "calm", "Whiterun") == -1);
            REQUIRE(fake.addMemoryCalls == 0);
        }
    }

    SECTION("when the database is ready")
    {
        fake.memorySystemReady = true;

        SECTION("should let the queries through")
        {
            (void)SkyrimNet::GetRecentEvents(kActorFormID, 10, "dialogue");
            (void)SkyrimNet::GetMemoriesForActor(kActorFormID, 10, "q");
            REQUIRE(fake.recentEventsCalls == 1);
            REQUIRE(fake.memoriesForActorCalls == 1);
        }
    }
}

TEST_CASE("SkyrimNetAPI forwards its queries faithfully", "[SkyrimNetAPI][engine]")
{
    // Happy path, re-run per leaf: SkyrimNet installed, database ready.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when events are requested")
    {
        SECTION("should return what SkyrimNet said")
        {
            CopyJson(fake.eventsJson, R"([{"type":"dialogue"}])");
            REQUIRE(SkyrimNet::GetRecentEvents(kActorFormID, 25, "dialogue") == R"([{"type":"dialogue"}])");
        }

        SECTION("should pass the form id and count through")
        {
            (void)SkyrimNet::GetRecentEvents(kActorFormID, 25, "dialogue");
            REQUIRE(fake.lastFormID == kActorFormID);
            REQUIRE(fake.lastMaxCount == 25);
        }
    }

    SECTION("when engagement is requested")
    {
        SECTION("should pass every window and flag through")
        {
            // Five arguments of three types in a row, which is exactly the
            // shape a transposition hides in.
            (void)SkyrimNet::GetActorEngagement(7, true, false, 86400.0, 604800.0);
            REQUIRE(fake.engagementCalls == 1);
            REQUIRE(fake.lastMaxCount == 7);
            REQUIRE(fake.lastExcludePlayer);
            REQUIRE_FALSE(fake.lastPlayerEventsOnly);
            REQUIRE(fake.lastShortWindow == 86400.0);
            REQUIRE(fake.lastMediumWindow == 604800.0);
        }
    }

    SECTION("when a filtered memory query is made")
    {
        SECTION("should serialize the filter into the query it sends")
        {
            // The filtering happens in SkyrimNet's SQL, before truncation —
            // which is the whole reason this endpoint exists. A filter that
            // failed to serialize would come back as an unfiltered top-N and
            // look like a working query.
            MemoryQuery query;
            query.maxCount = 12;
            query.includeTags = {"gossip"};
            (void)SkyrimNet::QueryMemoriesForActor(kActorFormID, query);
            REQUIRE(fake.queryMemoriesCalls == 1);
            const std::string sent{fake.lastQueryJson};
            REQUIRE(sent.find("gossip") != std::string::npos);
            REQUIRE(sent.find("12") != std::string::npos);
        }
    }

    SECTION("when a UUID is resolved")
    {
        SECTION("should return what SkyrimNet said")
        {
            REQUIRE(SkyrimNet::FormIDToUUID(kActorFormID) == fake.uuidAnswer);
            REQUIRE(fake.lastFormID == kActorFormID);
        }
    }

    SECTION("when a memory is written")
    {
        SECTION("should pass every field through")
        {
            const int id =
                SkyrimNet::AddMemory(kActorFormID, "Ysolda paid her debt", 0.75f, "EXPERIENCE", "relief", "Whiterun");
            REQUIRE(id == fake.addMemoryAnswer);
            REQUIRE(std::string{fake.lastMemoryText} == "Ysolda paid her debt");
            REQUIRE(fake.lastImportance == 0.75f);
            REQUIRE(std::string{fake.lastMemoryType} == "EXPERIENCE");
            REQUIRE(std::string{fake.lastEmotion} == "relief");
            REQUIRE(std::string{fake.lastLocation} == "Whiterun");
        }
    }

    SECTION("when a decorator is registered")
    {
        SECTION("should pass the name and description through")
        {
            REQUIRE(SkyrimNet::RegisterDecorator(
                "ne_narrative_tension", "the tension score", [](RE::Actor*) { return std::string{"42"}; }));
            REQUIRE(fake.registerDecoratorCalls == 1);
            REQUIRE(std::string{fake.lastDecoratorName} == "ne_narrative_tension");
            REQUIRE(std::string{fake.lastDecoratorDescription} == "the tension score");
        }

        SECTION("should report a refusal")
        {
            // SkyrimNet refuses a name a built-in or another plugin already
            // holds, and the decorator then silently never renders.
            fake.registerDecoratorSucceeds = false;
            REQUIRE_FALSE(
                SkyrimNet::RegisterDecorator("ne_narrative_tension", "d", [](RE::Actor*) { return std::string{}; }));
        }
    }

    SECTION("when a decorator name is checked")
    {
        SECTION("should report what SkyrimNet said")
        {
            fake.hasDecoratorAnswer = true;
            REQUIRE(SkyrimNet::HasDecorator("ne_narrative_phase"));
            REQUIRE(std::string{fake.lastDecoratorName} == "ne_narrative_phase");
        }
    }
}

TEST_CASE("SkyrimNetAPI bridges the LLM callback onto the plugin thread", "[SkyrimNetAPI][engine]")
{
    // The type system already makes it impossible to invoke the caller's
    // callback from SkyrimNet's own thread — the callback takes a
    // PluginThread::Token, and the only legal place to make one is inside an
    // AsyncDispatch job. What is checkable at runtime is that the bridge is
    // actually there and that the response survives it: SkyrimNet's response
    // pointer is valid only for the duration of its call, so a wrapper that
    // captured the pointer rather than a copy would hand the callback freed
    // memory some time later.
    //
    // The fake delivers its callback inline, from the caller's own thread,
    // which is the sharpest version of the check: a wrapper that skipped the
    // bridge would run the callback here and be caught.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    AsyncDispatch::Start();

    SECTION("when the call is queued")
    {
        // The promises are shared and captured by value on purpose. The whole
        // point of this wrapper is that the callback runs later, on another
        // thread — so it outlives this section, and capturing its locals by
        // reference would hand the worker a dangling pointer once a leaf that
        // does not wait for the callback finishes.
        auto delivered = std::make_shared<std::promise<std::string>>();
        auto arrived = delivered->get_future();
        auto where = std::make_shared<std::promise<std::thread::id>>();
        auto ranOn = where->get_future();

        const bool queued = SkyrimNet::SendCustomPromptToLLM(
            "beat_select",
            "default",
            R"({"phase":"Climax"})",
            [delivered, where](const PluginThread::Token&, std::string response, bool success) {
                where->set_value(std::this_thread::get_id());
                delivered->set_value(success ? response : std::string{"failed"});
            });

        SECTION("should accept it")
        {
            REQUIRE(queued);
        }

        SECTION("should pass the prompt, variant and context through")
        {
            REQUIRE(std::string{fake.lastPromptName} == "beat_select");
            REQUIRE(std::string{fake.lastVariant} == "default");
            REQUIRE(std::string{fake.lastContextJson} == R"({"phase":"Climax"})");
        }

        SECTION("should deliver the response")
        {
            REQUIRE(Arrived(arrived));
            REQUIRE(arrived.get() == std::string{fake.promptResponse});
        }

        SECTION("should run the callback somewhere other than SkyrimNet's thread")
        {
            REQUIRE(Arrived(ranOn));
            REQUIRE(ranOn.get() != std::this_thread::get_id());
        }
    }

    SECTION("when SkyrimNet refuses the call")
    {
        fake.sendPromptAccepts = false;

        SECTION("should say so")
        {
            REQUIRE_FALSE(SkyrimNet::SendCustomPromptToLLM(
                "beat_select", "default", "{}", [](const PluginThread::Token&, std::string, bool) {}));
        }
    }

    AsyncDispatch::Stop();
}

TEST_CASE("SkyrimNetAPI's foreign LLM callback stays foreign", "[SkyrimNetAPI][engine]")
{
    // The variant the synchronous overload is built on. It must NOT bridge
    // through AsyncDispatch: the plugin thread waiting on the promise is the
    // same thread that would have to dequeue the job that sets it, so a bridge
    // here is a deadlock rather than a slowdown.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when the call is queued")
    {
        SECTION("should run the callback on the delivering thread")
        {
            // The fake delivers inline, so "the delivering thread" is this one.
            // A wrapper that routed through AsyncDispatch would answer with the
            // worker's id — and, with no worker running, never answer at all.
            std::thread::id ranOn;
            const bool queued = SkyrimNet::SendCustomPromptToLLMForeign(
                "beat_select", "default", "{}", [&](std::string, bool) { ranOn = std::this_thread::get_id(); });
            REQUIRE(queued);
            REQUIRE(ranOn == std::this_thread::get_id());
        }
    }
}

TEST_CASE("SkyrimNetAPI's synchronous LLM call", "[SkyrimNetAPI][engine]")
{
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when SkyrimNet answers")
    {
        SECTION("should return the response linearly")
        {
            SkyrimNet::LLMResult result;
            OnPluginThread([&](const PluginThread::Token& pt) {
                result = SkyrimNet::SendCustomPromptToLLM(pt, "beat_select", "default", "{}");
            });
            REQUIRE(result.ok);
            REQUIRE(result.response == std::string{fake.promptResponse});
        }
    }

    SECTION("when SkyrimNet reports a failure")
    {
        fake.sendPromptSucceeds = false;

        SECTION("should report it rather than the response")
        {
            SkyrimNet::LLMResult result;
            OnPluginThread([&](const PluginThread::Token& pt) {
                result = SkyrimNet::SendCustomPromptToLLM(pt, "beat_select", "default", "{}");
            });
            REQUIRE_FALSE(result.ok);
        }
    }

    SECTION("when SkyrimNet refuses the call")
    {
        fake.sendPromptAccepts = false;

        SECTION("should fail immediately rather than wait for a callback")
        {
            // Nothing will ever poke the promise, so a wrapper that waited
            // anyway would hold the plugin thread for the rest of the session.
            // Bounded here so that failure reports rather than wedges the run.
            auto done = std::async(std::launch::async, [] {
                SkyrimNet::LLMResult result;
                OnPluginThread([&](const PluginThread::Token& pt) {
                    result = SkyrimNet::SendCustomPromptToLLM(pt, "beat_select", "default", "{}");
                });
                return result;
            });
            REQUIRE(Arrived(done));
            const auto result = done.get();
            REQUIRE_FALSE(result.ok);
            REQUIRE_FALSE(result.response.empty());
        }
    }
}
