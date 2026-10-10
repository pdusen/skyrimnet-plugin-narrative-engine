#include <Decorators.h>

#include <DecisionLog.h>
#include <FakeSkyrimNet.h>
#include <PhaseTracker.h>
#include <SkyrimNetAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

// Tests for the two Inja decorators that put the Director's narrative state
// into every NPC's prompt.
//
// These are the only path by which the Director's state reaches the LLM at all,
// and every failure they can have is silent. A decorator that does not register
// leaves its placeholder unrendered in every bio for the rest of the session; a
// decorator that returns the wrong shape of string feeds a mood-mapping table a
// value it has no row for. Neither shows up in a log, and neither is visible in
// game except as NPCs who never seem to notice what is going on around them.
//
// The callbacks themselves live in an anonymous namespace, so the only way to
// reach them is the way SkyrimNet does: through what was registered. The
// harness's stand-in SkyrimNet.dll calls each callback once as it is registered
// (with a null actor, which is what these two are documented to ignore), and
// records what came back — so the registered function is checked rather than a
// copy of it.

namespace
{
    namespace Decorators = NarrativeEngine::Decorators;
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    // What the named decorator answered when the fake called it during
    // registration. Looked up by name rather than by position, because the
    // order Register installs them in is not part of the contract.
    std::string ResultOf(const FakeSkyrimNetState& fake, std::string_view name)
    {
        for (int i = 0; i < fake.recordedDecorators; ++i) {
            if (std::string_view{fake.decoratorNames[i]} == name)
                return fake.decoratorResults[i];
        }
        return "<never registered>";
    }

    bool Registered(const FakeSkyrimNetState& fake, std::string_view name)
    {
        for (int i = 0; i < fake.recordedDecorators; ++i) {
            if (std::string_view{fake.decoratorNames[i]} == name)
                return true;
        }
        return false;
    }

    DecisionLog::DecisionRecord RecordWithTension(std::uint32_t tension)
    {
        DecisionLog::DecisionRecord r;
        r.tensionScore = tension;
        return r;
    }
} // namespace

TEST_CASE("Decorators::Register", "[Decorators][engine]")
{
    // Happy path, re-run per leaf: SkyrimNet installed and accepting
    // registrations, with no name already taken.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when SkyrimNet accepts both")
    {
        Decorators::Register();

        SECTION("should register exactly two decorators")
        {
            REQUIRE(fake.registerDecoratorCalls == 2);
        }

        SECTION("should register them under the names the prompt expects")
        {
            // The names are the placeholders the shipped prompt submodule
            // renders, so they are a contract with a file outside this repo's
            // C++ entirely. Renaming one here leaves the template referring to
            // a decorator that no longer exists, and Inja renders nothing.
            REQUIRE(Registered(fake, "ne_narrative_tension"));
            REQUIRE(Registered(fake, "ne_narrative_phase"));
        }

        SECTION("should describe what the value means")
        {
            // The description is what the LLM reads to know how to interpret
            // the value, so an empty one is a decorator the model cannot use.
            REQUIRE(std::string{fake.lastDecoratorDescription}.find("Freytag") != std::string::npos);
        }
    }

    SECTION("when a name is already taken")
    {
        // A built-in or another plugin holds it. Registration is attempted
        // anyway — SkyrimNet is the one that decides — so the count must not
        // change.
        fake.hasDecoratorAnswer = true;

        SECTION("should still attempt both registrations")
        {
            Decorators::Register();
            REQUIRE(fake.registerDecoratorCalls == 2);
        }
    }

    SECTION("when SkyrimNet refuses a registration")
    {
        fake.registerDecoratorSucceeds = false;

        SECTION("should attempt both rather than stop at the first")
        {
            // A refusal on the tension decorator must not cost the phase one
            // too: they are independent, and half a narrative state in the
            // prompt is worse than none because the template renders around it.
            Decorators::Register();
            REQUIRE(fake.registerDecoratorCalls == 2);
        }
    }
}

TEST_CASE("Decorators report the narrative state", "[Decorators][engine]")
{
    // The registered callbacks are reached through SkyrimNet, which calls each
    // one as it is registered and keeps what it returned. That is the only way
    // in: the callbacks are anonymous-namespace statics.
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());

    SECTION("when a decision has been recorded")
    {
        DecisionLog::Clear();
        DecisionLog::Append(RecordWithTension(73));
        PhaseTracker::Reset(PhaseTracker::Phase::Climax);
        fake.Reset();
        Decorators::Register();

        SECTION("should render the phase by name")
        {
            // The prompt template maps this string to a mood paragraph, so it
            // has to be one of the five names PhaseTracker emits rather than a
            // number or an enum's integer value.
            REQUIRE(ResultOf(fake, "ne_narrative_phase") == "Climax");
        }

        SECTION("should render the tension as a decimal string")
        {
            // A decimal string in 0..100, because the mood-mapping table in the
            // template compares it numerically. Anything else -- a float, a
            // qualitative word -- falls through every row of that table.
            REQUIRE(ResultOf(fake, "ne_narrative_tension") == "73");
        }
    }

    SECTION("when nothing has been evaluated yet")
    {
        DecisionLog::Clear();
        PhaseTracker::Reset();
        fake.Reset();

        SECTION("should still render a phase")
        {
            // Exposition is a real answer before the first evaluation, not a
            // fallback: the story has not started, which is what Exposition
            // means.
            Decorators::Register();
            REQUIRE(ResultOf(fake, "ne_narrative_phase") == "Exposition");
        }

        SECTION("should render a calm tension rather than nothing")
        {
            // The log is empty, so there is no score to report -- but an empty
            // string would leave the template's mood table with nothing to
            // compare, so the nullopt is collapsed to 0 here instead.
            Decorators::Register();
            REQUIRE(ResultOf(fake, "ne_narrative_tension") == "0");
        }
    }
}
