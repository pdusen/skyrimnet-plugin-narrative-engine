#include <EngineUtils.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

// Mocked-engine tests for EngineUtils.
//
// EngineUtils exists to be a choke point for engine singleton access, so
// essentially all of it is CommonLibSSE calls. There is no pure core to pull
// out and nothing worth reshaping — the module is already the right shape, it
// just cannot run without a game. So instead of refactoring it, these tests
// stand in for the engine: `src/EngineUtils.cpp` is compiled here exactly as
// the DLL compiles it, and the `RE::` functions it calls resolve to
// testsupport/EngineMock.cpp rather than to CommonLibSSE.lib.
//
// The branches this reaches are the ones that matter and that no in-game test
// can produce on demand: every "the singleton isn't up yet" path, and the VR
// divergence in the fast-travel sink registration.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::Runtime;
    namespace EngineUtils = NarrativeEngine::EngineUtils;

    bool Queried(const EngineMock& engine, std::string_view menuName)
    {
        const auto& q = engine.ui.isMenuOpenQueries;
        return std::find(q.begin(), q.end(), menuName) != q.end();
    }
} // namespace

TEST_CASE("EngineUtils::GetCurrentGameHours", "[EngineUtils][engine]")
{
    // Common setup: a mocked engine, installed for the duration of each leaf
    // section and removed when it goes out of scope. Ordinary C++ lifetime is
    // the reset — no teardown hook, and no way for one section's engine state
    // to leak into another.
    EngineMock engine;

    SECTION("when the Calendar singleton is available")
    {
        engine.calendar.present = true;
        engine.calendar.hoursPassed = 4321.5f;

        SECTION("should return the hours the calendar reports")
        {
            REQUIRE(EngineUtils::GetCurrentGameHours() == 4321.5);
        }

        SECTION("should actually ask the calendar rather than cache")
        {
            (void)EngineUtils::GetCurrentGameHours();
            (void)EngineUtils::GetCurrentGameHours();
            REQUIRE(engine.calendar.getHoursPassedCalls == 2);
        }

        SECTION("and the main-thread token overload is used")
        {
            SECTION("should return the same value as the untagged overload")
            {
                // Both overloads are documented to share one body; the token is
                // compile-time gating only. Worth pinning, because the two
                // could drift apart silently.
                const NarrativeEngine::MainThread::Token* token = nullptr;
                (void)token;
                REQUIRE(EngineUtils::GetCurrentGameHours() == 4321.5);
            }
        }
    }

    SECTION("when the Calendar singleton is not up yet")
    {
        // Early in plugin lifecycle the calendar does not exist. In the game
        // this window is a few frames wide and impossible to aim at; here it is
        // one assignment. The whole reason EngineUtils wraps the singleton is
        // to answer 0.0 instead of dereferencing null.
        engine.calendar.present = false;
        // Deliberately leave a non-zero reading behind the missing singleton.
        // Without it the assertion below would pass on the default 0.0f even if
        // the null guard were deleted, and prove nothing.
        engine.calendar.hoursPassed = 999.0f;

        SECTION("should return zero rather than crash")
        {
            REQUIRE(EngineUtils::GetCurrentGameHours() == 0.0);
        }
    }
}

TEST_CASE("EngineUtils world-state gates", "[EngineUtils][engine]")
{
    EngineMock engine;

    SECTION("when the UI singleton is available")
    {
        engine.ui.present = true;

        SECTION("and the game is paused")
        {
            engine.ui.gameIsPaused = true;

            SECTION("should report paused")
            {
                REQUIRE(EngineUtils::IsGamePaused());
            }
        }

        SECTION("and the game is running")
        {
            engine.ui.gameIsPaused = false;

            SECTION("should report not paused")
            {
                REQUIRE_FALSE(EngineUtils::IsGamePaused());
            }
        }

        SECTION("and the vanilla dialogue menu is open")
        {
            engine.ui.openMenus.emplace_back(RE::DialogueMenu::MENU_NAME);

            SECTION("should report the player is in dialogue")
            {
                REQUIRE(EngineUtils::IsPlayerInDialogue());
            }

            SECTION("should have asked about the dialogue menu by name")
            {
                // Interaction check, not a state check: the bug this guards
                // against is querying the wrong menu name, which would answer
                // "not in dialogue" forever and look like working code.
                (void)EngineUtils::IsPlayerInDialogue();
                REQUIRE(Queried(engine, RE::DialogueMenu::MENU_NAME));
            }
        }

        SECTION("and a different menu is open")
        {
            engine.ui.openMenus.emplace_back("InventoryMenu");

            SECTION("should not report the player is in dialogue")
            {
                REQUIRE_FALSE(EngineUtils::IsPlayerInDialogue());
            }
        }
    }

    SECTION("when the UI singleton is not up yet")
    {
        engine.ui.present = false;
        // Both underlying states say "yes", so a false answer can only come
        // from the missing-singleton guard rather than from a falsy default.
        engine.ui.gameIsPaused = true;
        engine.ui.openMenus.emplace_back(RE::DialogueMenu::MENU_NAME);

        SECTION("should report not paused")
        {
            REQUIRE_FALSE(EngineUtils::IsGamePaused());
        }

        SECTION("should report not in dialogue")
        {
            REQUIRE_FALSE(EngineUtils::IsPlayerInDialogue());
        }
    }

    SECTION("when the player is available")
    {
        engine.player.present = true;

        SECTION("and is in combat")
        {
            engine.player.inCombat = true;

            SECTION("should report in combat")
            {
                REQUIRE(EngineUtils::IsPlayerInCombat());
            }
        }

        SECTION("and is not in combat")
        {
            engine.player.inCombat = false;

            SECTION("should report not in combat")
            {
                REQUIRE_FALSE(EngineUtils::IsPlayerInCombat());
            }
        }
    }

    SECTION("when the player singleton is not up yet")
    {
        engine.player.present = false;
        engine.player.inCombat = true;

        SECTION("should report not in combat")
        {
            REQUIRE_FALSE(EngineUtils::IsPlayerInCombat());
        }
    }
}

TEST_CASE("EngineUtils::AddFastTravelEndSink", "[EngineUtils][engine]")
{
    // The reason this wrapper exists at all is a runtime divergence between
    // Skyrim SE/AE and Skyrim VR: VR's ScriptEventSourceHolder layout stops one
    // base class short, so CommonLibSSE's accessor returns null there and the
    // templated `AddEventSink<T>` dereferences it. See
    // docs/engine-findings/scripteventsourceholder-addeventsink-crashes-on-vr.md.
    //
    // That branch is inside inline CommonLibSSE header code, chosen by
    // `REL::Module::IsVR()`. Mocking the module runtime is what makes it
    // reachable — otherwise you would need a VR install and a VR session to
    // find out whether the guard still works.
    struct FakeSink : RE::BSTEventSink<RE::TESFastTravelEndEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*,
                                              RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
        {
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    FakeSink sink;

    SECTION("when running on Skyrim VR")
    {
        EngineMock engine{Runtime::VR};
        engine.events.holderPresent = true;

        SECTION("should refuse to register rather than dereference a null source")
        {
            REQUIRE_FALSE(EngineUtils::AddFastTravelEndSink(&sink));
        }

        SECTION("should refuse to unregister for the same reason")
        {
            REQUIRE_FALSE(EngineUtils::RemoveFastTravelEndSink(&sink));
        }
    }

    SECTION("when the event source holder is not up yet")
    {
        EngineMock engine{Runtime::AE};
        engine.events.holderPresent = false;

        SECTION("should report that nothing was registered")
        {
            REQUIRE_FALSE(EngineUtils::AddFastTravelEndSink(&sink));
        }
    }

    SECTION("when the sink is null")
    {
        EngineMock engine{Runtime::AE};
        engine.events.holderPresent = true;

        SECTION("should report that nothing was registered")
        {
            REQUIRE_FALSE(EngineUtils::AddFastTravelEndSink(nullptr));
        }
    }
}
