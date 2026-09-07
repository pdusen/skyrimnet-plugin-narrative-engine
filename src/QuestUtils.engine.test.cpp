#include <QuestUtils.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
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
// WHAT IS NOT COVERED, AND WHY
//
// The dispatch itself -- everything past the guard chain -- cannot run here.
// The final statement constructs two `RE::BSFixedString`s, and
// `BSFixedString::ctor8` is an inline member that resolves
// `RELOCATION_ID(67819, 69161)` through the address library at RUNTIME. That is
// not a link-time symbol, so the mocked-engine harness cannot substitute it:
// the linker never sees it, and `testsupport/CommonLibSSERuntimeStubs.cpp`
// aborts when production code reaches relocation.
//
// So these branches have no cases below: the successful dispatch, the VM
// refusing a call, the handle travelling from the policy to the dispatch, the
// script and method names arriving intact, the packed argument, and everything
// in VMDispatchQuestSetStage except its null-quest guard. Standing in for a
// relocated inline function needs a relocation-mock layer that does not exist
// yet -- see docs/DEVELOPMENT.md, "Known limits".

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
} // namespace

TEST_CASE("QuestUtils::VMDispatchOnQuest", "[QuestUtils][engine]")
{
    // Happy path, re-run for every leaf below: a VM that is up, a handle policy
    // that answers, and a dispatch the VM accepts. Those are EngineMock's
    // defaults, so what is hoisted here is the quest itself — each section then
    // overrides only the one thing it is about.
    EngineMock engine;
    RE::TESQuest* quest = FakeQuest();

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
}

TEST_CASE("QuestUtils::VMDispatchQuestSetStage", "[QuestUtils][engine]")
{
    EngineMock engine;

    SECTION("when the quest is null")
    {
        // The only branch of the SetStage wrapper that stops short of building
        // a BSFixedString, and so the only one reachable without a game.
        SECTION("should refuse to dispatch")
        {
            REQUIRE_FALSE(QuestUtils::VMDispatchQuestSetStage(nullptr, 40));
        }
    }
}
