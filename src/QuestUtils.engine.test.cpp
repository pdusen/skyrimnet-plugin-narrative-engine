#include <QuestUtils.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

// Mocked-engine tests for the Papyrus VM dispatch wrappers.
//
// QuestUtils is nothing but engine access: it reaches the Papyrus virtual
// machine, asks its handle policy for a handle onto a TESQuest, and queues a
// member-function call. There is no pure core to extract and nothing worth
// reshaping — so the engine is mocked and the production source compiles here
// exactly as it does in the DLL.
//
// Most of the module's logic lives in the `VMDispatchOnQuest` template in the
// header, which is instantiated into this translation unit. Its guard chain
// (quest, VM, handle policy) is the behaviour under test, and every one of
// those failure branches is unreachable in a running game.
//
// The dispatch itself reaches `RE::BSFixedString`, whose constructor is inline
// CommonLibSSE code resolving RELOCATION_ID(67819, 69161) through the address
// library at runtime -- past anything the linker can substitute. It is reachable
// here because testsupport/RelocationMocks.cpp registers a stand-in against that
// id, so the string builds against memory the harness owns.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace QuestUtils = NarrativeEngine::QuestUtils;

    // A stand-in quest. Nothing under test dereferences it — it is passed to
    // the handle policy and otherwise only compared — so storage of the right
    // size and alignment is all a TESQuest has to be here.
    RE::TESQuest* FakeQuest()
    {
        alignas(RE::TESQuest) static std::byte storage[sizeof(RE::TESQuest)]{};
        return reinterpret_cast<RE::TESQuest*>(storage);
    }

    // Names with no meaning to the engine, chosen so an assertion that they
    // arrived intact cannot pass by coincidence against a hard-coded "Quest"
    // or "SetStage" somewhere in the dispatch path.
    constexpr std::string_view kScriptName = "_ne_VisitQuest";
    constexpr std::string_view kMethodName = "RunSenderSilentSceneEvent";
    constexpr std::int32_t kArgument = 7;

    // Distinctive enough that finding it on the dispatch proves it travelled
    // from the handle policy rather than being a zero that happened to match.
    constexpr std::uint64_t kHandle = 0x00ABCDEF12345678ull;
} // namespace

TEST_CASE("QuestUtils::VMDispatchOnQuest", "[QuestUtils][engine]")
{
    // Happy path, re-run for every leaf below: a VM that is up, a handle policy
    // that answers, and a dispatch the VM accepts. Those are EngineMock's
    // defaults, so what is hoisted here is the quest itself — each section then
    // overrides only the one thing it is about.
    EngineMock engine;
    RE::TESQuest* quest = FakeQuest();
    engine.papyrus.handle = kHandle;

    SECTION("when the quest is null")
    {
        SECTION("should refuse to dispatch")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchOnQuest(nullptr, kScriptName, kMethodName, kArgument));
        }

        SECTION("should not reach the virtual machine at all")
        {
            // The null check comes first for a reason. A null quest that got as
            // far as the handle policy would come back with a handle onto
            // nothing, and the call would be queued against it — failing later,
            // inside the VM, with nothing pointing back here.
            (void)QuestUtils::VMDispatchOnQuest(nullptr, kScriptName, kMethodName, kArgument);
            REQUIRE(engine.papyrus.handleRequests.empty());
            REQUIRE(engine.papyrus.dispatches.empty());
        }
    }

    SECTION("when the virtual machine is not up yet")
    {
        engine.papyrus.vmPresent = false;

        SECTION("should refuse to dispatch")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument));
        }

        SECTION("should not ask for a handle")
        {
            (void)QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument);
            REQUIRE(engine.papyrus.handleRequests.empty());
        }
    }

    SECTION("when the handle policy is unavailable")
    {
        engine.papyrus.handlePolicyPresent = false;

        SECTION("should refuse to dispatch")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument));
        }

        SECTION("should not queue a call")
        {
            (void)QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument);
            REQUIRE(engine.papyrus.dispatches.empty());
        }
    }

    SECTION("when everything the dispatch needs is available")
    {
        const bool queued = QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument);

        SECTION("should report the call was queued")
        {
            REQUIRE(queued);
        }

        SECTION("should take the handle from the quest it was given")
        {
            REQUIRE(engine.papyrus.handleRequests.size() == 1);
            REQUIRE(engine.papyrus.handleRequests[0].formType == static_cast<std::uint32_t>(RE::TESQuest::FORMTYPE));
            REQUIRE(engine.papyrus.handleRequests[0].form == static_cast<const RE::TESForm*>(quest));
        }

        SECTION("should dispatch against that handle")
        {
            REQUIRE(engine.papyrus.dispatches.size() == 1);
            REQUIRE(engine.papyrus.dispatches[0].handle == kHandle);
        }

        SECTION("should name the script and method it was given")
        {
            // The failure this catches is a dispatch that queues happily
            // against the wrong Papyrus function: the VM accepts it, the
            // return value is true, and nothing happens in game.
            REQUIRE(engine.papyrus.dispatches.size() == 1);
            REQUIRE(engine.papyrus.dispatches[0].className == kScriptName);
            REQUIRE(engine.papyrus.dispatches[0].methodName == kMethodName);
        }

        SECTION("should pass the packed arguments along")
        {
            REQUIRE(engine.papyrus.dispatches.size() == 1);
            REQUIRE(engine.papyrus.dispatches[0].hadArguments);
            REQUIRE(engine.papyrus.packedInts.size() == 1);
            REQUIRE(engine.papyrus.packedInts[0] == kArgument);
        }
    }

    SECTION("when the virtual machine refuses the call")
    {
        // Everything else is in order, so a false result here can only be the
        // VM's own answer being reported rather than swallowed.
        engine.papyrus.dispatchSucceeds = false;

        SECTION("should report the call was not queued")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchOnQuest(quest, kScriptName, kMethodName, kArgument));
        }
    }
}

TEST_CASE("QuestUtils::VMDispatchQuestSetStage", "[QuestUtils][engine]")
{
    EngineMock engine;
    RE::TESQuest* quest = FakeQuest();
    constexpr std::uint32_t kStage = 40;

    SECTION("when the quest is available")
    {
        const bool queued = QuestUtils::VMDispatchQuestSetStage(quest, kStage);

        SECTION("should report the call was queued")
        {
            REQUIRE(queued);
        }

        SECTION("should dispatch Quest.SetStage")
        {
            REQUIRE(engine.papyrus.dispatches.size() == 1);
            REQUIRE(engine.papyrus.dispatches[0].className == "Quest");
            REQUIRE(engine.papyrus.dispatches[0].methodName == "SetStage");
        }

        SECTION("should pack the stage number as the argument")
        {
            REQUIRE(engine.papyrus.packedInts.size() == 1);
            REQUIRE(engine.papyrus.packedInts[0] == static_cast<std::int32_t>(kStage));
        }
    }

    SECTION("when the stage number exceeds the signed range")
    {
        // The wrapper takes a std::uint32_t and narrows it to the signed int
        // Papyrus wants, so a stage above INT32_MAX arrives negative. Real
        // quest stages are two digits and this never bites, but the conversion
        // is real: pinning it means widening the Papyrus argument later is a
        // deliberate change rather than a silent one.
        (void)QuestUtils::VMDispatchQuestSetStage(quest, 0x80000000u);

        SECTION("should pack the wrapped value")
        {
            REQUIRE(engine.papyrus.packedInts.size() == 1);
            REQUIRE(engine.papyrus.packedInts[0] == std::numeric_limits<std::int32_t>::min());
        }
    }

    SECTION("when the quest is null")
    {
        SECTION("should refuse to dispatch")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchQuestSetStage(nullptr, kStage));
        }
    }
}
