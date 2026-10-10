#include <BeatRegistry.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <IBeat.h>
#include <Settings.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Mocked-engine tests for the beat registry.
//
// The registry is the gate every beat passes through on its way to being
// dispatched, and two of the three ways a beat leaves that gate are invisible
// to the beat itself: a disabled beat and a polarity mismatch never reach
// IsAvailable, so a beat that is wrongly filtered has no way to say so. That is
// the whole reason these cases exist — the symptom in game is a beat that never
// fires, with nothing in the log to distinguish it from a beat that keeps
// declining.
//
// The registry takes ownership of every beat and has no way to unregister, so
// its contents accumulate across the whole run. Rather than adding a Clear that
// only tests would call, every case registers beats under names unique to it,
// and the fake beats decline for every case but their own. AvailableMatching
// therefore sees the other cases' beats and filters them out on IsAvailable,
// which is exactly what it would do in the game.

namespace
{
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    namespace BeatRegistry = NarrativeEngine::BeatRegistry;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::IBeat;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;

    // Which case is currently running. Beats registered by an earlier case see
    // a different value and decline, which is what keeps AvailableMatching's
    // results attributable to the case under test.
    int g_activeCase = 0;

    class FakeBeat : public IBeat
    {
    public:
        FakeBeat(std::string name, BeatPolarity polarity, bool available)
            : name_(std::move(name)), polarity_(polarity), available_(available), owningCase_(g_activeCase)
        {}

        std::string Name() const override
        {
            return name_;
        }
        std::string Description() const override
        {
            return "A fake beat, for the registry's benefit.";
        }
        BeatPolarity Polarity() const override
        {
            return polarity_;
        }

        bool IsAvailable(const BeatContext&) const override
        {
            ++availabilityQueries;
            return available_ && owningCase_ == g_activeCase;
        }

        void OnStart(const BeatContext&, const nlohmann::json&) override {}
        TickResult Tick(const NarrativeEngine::PluginThread::Token&, TickMode, BeatState) override
        {
            return {};
        }
        void Abort(const NarrativeEngine::MainThread::Token&) override {}

        // Re-arm a registration that survived from an earlier leaf path, so
        // each leaf sees a beat that answers for it and a query count of zero.
        void Adopt(int owningCase, bool available)
        {
            owningCase_ = owningCase;
            available_ = available;
            availabilityQueries = 0;
        }

        // Counts how many times the registry asked. A disabled beat or a
        // polarity mismatch must be filtered before this moves.
        mutable int availabilityQueries = 0;

    private:
        std::string name_;
        BeatPolarity polarity_;
        bool available_;
        int owningCase_;
    };

    // Registers a beat and hands back whatever the registry now holds under
    // that name. Looked up rather than borrowed from the unique_ptr on purpose:
    // top-level setup re-runs for every leaf path, so from the second leaf on
    // the registration is a rejected duplicate and the object just built is
    // destroyed on the way out of Register.
    FakeBeat* RegisterBeat(const std::string& name, BeatPolarity polarity = BeatPolarity::Either, bool available = true)
    {
        BeatRegistry::Register(std::make_unique<FakeBeat>(name, polarity, available));
        auto* registered = static_cast<FakeBeat*>(BeatRegistry::Find(name));
        if (registered) {
            // The surviving registration was built by an earlier leaf and still
            // carries that leaf's case id, query count and enabled flag. Put
            // all three back to what a first registration would have produced —
            // the enabled flag through the same settings lookup Register uses,
            // so a beat the INI turns off still comes back off.
            registered->Adopt(g_activeCase, available);
            BeatRegistry::SetEnabled(name, NarrativeEngine::Settings::GetBeatEnabled(name, true));
        }
        return registered;
    }

    // Claims a case id so this case's beats are the only ones that can be
    // available. Declared as a local so it re-runs per leaf path.
    struct ActiveCase
    {
        explicit ActiveCase(int id)
        {
            g_activeCase = id;
        }

        ActiveCase(const ActiveCase&) = delete;
        ActiveCase& operator=(const ActiveCase&) = delete;
    };

    bool Contains(const std::vector<NarrativeEngine::IBeat*>& beats, const IBeat* wanted)
    {
        return std::find(beats.begin(), beats.end(), wanted) != beats.end();
    }

    bool NamesBeat(const std::vector<BeatRegistry::EntryView>& entries, std::string_view wanted)
    {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.name == wanted; });
    }
} // namespace

TEST_CASE("BeatRegistry::Register", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{1};

    SECTION("when a beat is registered")
    {
        auto* beat = RegisterBeat("registry_case1_first");

        SECTION("should be findable by name")
        {
            REQUIRE(BeatRegistry::Find("registry_case1_first") == beat);
        }

        SECTION("should appear in the snapshot")
        {
            REQUIRE(NamesBeat(BeatRegistry::All(), "registry_case1_first"));
        }

        SECTION("should have never been dispatched")
        {
            REQUIRE(BeatRegistry::LastDispatchedRealTime("registry_case1_first") == 0.0);
        }
    }

    SECTION("when the same name is registered twice")
    {
        RegisterBeat("registry_case1_dup", BeatPolarity::Raise);
        RegisterBeat("registry_case1_dup", BeatPolarity::Lower);

        SECTION("should keep the first registration")
        {
            // Names key the cosave records, the beat-select prompt's answer and
            // the dashboard rows. A second entry under one name would make all
            // three ambiguous, and the duplicate is far more likely to be a
            // double Register call than a deliberate replacement. The polarity
            // is what tells the two apart.
            REQUIRE(BeatRegistry::Find("registry_case1_dup")->Polarity() == BeatPolarity::Raise);
        }

        SECTION("should hold only one entry under the name")
        {
            const auto all = BeatRegistry::All();
            const auto count =
                std::count_if(all.begin(), all.end(), [](const auto& e) { return e.name == "registry_case1_dup"; });
            REQUIRE(count == 1);
        }
    }

    SECTION("when the beat is null")
    {
        SECTION("should refuse it")
        {
            const auto before = BeatRegistry::All().size();
            BeatRegistry::Register(nullptr);
            REQUIRE(BeatRegistry::All().size() == before);
        }
    }

    SECTION("when the beat has no name")
    {
        SECTION("should refuse it")
        {
            // An unnamed beat could never be found, toggled, or given a cosave
            // record, so admitting it would put a permanently unreachable row
            // in the dashboard.
            const auto before = BeatRegistry::All().size();
            BeatRegistry::Register(std::make_unique<FakeBeat>("", BeatPolarity::Either, true));
            REQUIRE(BeatRegistry::All().size() == before);
        }
    }
}

TEST_CASE("BeatRegistry seeds enabled state from settings", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{2};

    SECTION("when the beat has a settings key and is turned off there")
    {
        const ConfiguredSettings settings{"[Beats]\nbEnableAmbush=0\n"};
        RegisterBeat("ambush");

        SECTION("should register it disabled")
        {
            // The seed and the dashboard toggle write through the same table,
            // so a beat the player turned off stays off across a reload rather
            // than coming back enabled on every boot.
            REQUIRE_FALSE(BeatRegistry::IsEnabled("ambush"));
        }
    }

    SECTION("when the beat has no settings key")
    {
        RegisterBeat("registry_case2_unkeyed");

        SECTION("should default to enabled")
        {
            // Failing open. A beat with no key cannot be persisted either way,
            // and defaulting to disabled would silently drop any beat whose
            // settings row someone forgot to add.
            REQUIRE(BeatRegistry::IsEnabled("registry_case2_unkeyed"));
        }
    }

    SECTION("when the beat is unknown")
    {
        SECTION("should report disabled rather than guess")
        {
            REQUIRE_FALSE(BeatRegistry::IsEnabled("registry_case2_never_registered"));
        }
    }
}

TEST_CASE("BeatRegistry::SetEnabled", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{3};
    RegisterBeat("registry_case3_toggle");

    SECTION("when a beat is turned off")
    {
        BeatRegistry::SetEnabled("registry_case3_toggle", false);

        SECTION("should report it disabled")
        {
            REQUIRE_FALSE(BeatRegistry::IsEnabled("registry_case3_toggle"));
        }

        SECTION("should show it disabled in the snapshot")
        {
            const auto all = BeatRegistry::All();
            const auto it =
                std::find_if(all.begin(), all.end(), [](const auto& e) { return e.name == "registry_case3_toggle"; });
            REQUIRE(it != all.end());
            REQUIRE_FALSE(it->enabled);
        }

        SECTION("should turn back on again")
        {
            BeatRegistry::SetEnabled("registry_case3_toggle", true);
            REQUIRE(BeatRegistry::IsEnabled("registry_case3_toggle"));
        }
    }

    SECTION("when the name is unknown")
    {
        SECTION("should leave the registry alone")
        {
            BeatRegistry::SetEnabled("registry_case3_never_registered", false);
            REQUIRE(BeatRegistry::IsEnabled("registry_case3_toggle"));
        }
    }
}

TEST_CASE("BeatRegistry::MarkDispatched", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{4};
    RegisterBeat("registry_case4_dispatch");

    SECTION("when a beat is dispatched")
    {
        BeatRegistry::MarkDispatched("registry_case4_dispatch");

        SECTION("should stamp a real epoch time")
        {
            // Past 2001 in epoch seconds. A session-relative counter or a
            // steady-clock reading would both fail this, and the dashboard
            // renders the value as a wall-clock moment.
            REQUIRE(BeatRegistry::LastDispatchedRealTime("registry_case4_dispatch") > 1'000'000'000.0);
        }

        SECTION("should show the stamp in the snapshot")
        {
            const auto all = BeatRegistry::All();
            const auto it =
                std::find_if(all.begin(), all.end(), [](const auto& e) { return e.name == "registry_case4_dispatch"; });
            REQUIRE(it != all.end());
            REQUIRE(it->lastDispatchedRealTime > 1'000'000'000.0);
        }
    }

    SECTION("when the name is unknown")
    {
        SECTION("should report never dispatched")
        {
            BeatRegistry::MarkDispatched("registry_case4_never_registered");
            REQUIRE(BeatRegistry::LastDispatchedRealTime("registry_case4_never_registered") == 0.0);
        }
    }
}

TEST_CASE("BeatRegistry::Find", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{5};

    SECTION("when the name is unknown")
    {
        SECTION("should return nothing")
        {
            RegisterBeat("registry_case5_present");
            REQUIRE(BeatRegistry::Find("registry_case5_absent") == nullptr);
        }
    }
}

TEST_CASE("BeatRegistry::AvailableMatching", "[BeatRegistry][engine]")
{
    // Happy path, re-run per leaf: an enabled, available, Either-polarity beat
    // and a Raise request. Each case changes exactly one of those.
    EngineMock engine;
    const ActiveCase active{6};
    const BeatContext ctx;
    auto* beat = RegisterBeat("registry_case6_either", BeatPolarity::Either, true);

    SECTION("when everything permits the beat")
    {
        SECTION("should offer it")
        {
            REQUIRE(Contains(BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise), beat));
        }

        SECTION("should offer it for the other direction too")
        {
            // Either matches both, which is what lets a beat that can push
            // tension whichever way is wanted stay in the running all cycle.
            REQUIRE(Contains(BeatRegistry::AvailableMatching(ctx, BeatPolarity::Lower), beat));
        }
    }

    SECTION("when the beat's polarity matches the request")
    {
        auto* raiser = RegisterBeat("registry_case6_raise", BeatPolarity::Raise, true);

        SECTION("should offer it")
        {
            REQUIRE(Contains(BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise), raiser));
        }
    }

    SECTION("when the beat's polarity is the wrong way round")
    {
        auto* raiser = RegisterBeat("registry_case6_raise_only", BeatPolarity::Raise, true);
        const auto offered = BeatRegistry::AvailableMatching(ctx, BeatPolarity::Lower);

        SECTION("should not offer it")
        {
            REQUIRE_FALSE(Contains(offered, raiser));
        }

        SECTION("should not even ask whether it is available")
        {
            // One of the two silent exits. IsAvailable is where a beat logs its
            // own reason for declining, so a beat filtered before it is reached
            // leaves nothing in the log at all — which is why the filter order
            // is worth pinning rather than assuming.
            REQUIRE(raiser->availabilityQueries == 0);
        }
    }

    SECTION("when the beat is disabled")
    {
        BeatRegistry::SetEnabled("registry_case6_either", false);
        const auto offered = BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise);

        SECTION("should not offer it")
        {
            REQUIRE_FALSE(Contains(offered, beat));
        }

        SECTION("should not even ask whether it is available")
        {
            // The other silent exit, and the one a player can cause from the
            // dashboard.
            REQUIRE(beat->availabilityQueries == 0);
        }
    }

    SECTION("when the beat declines")
    {
        auto* unavailable = RegisterBeat("registry_case6_declines", BeatPolarity::Either, false);
        const auto offered = BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise);

        SECTION("should not offer it")
        {
            REQUIRE_FALSE(Contains(offered, unavailable));
        }

        SECTION("should have asked it")
        {
            // Unlike the two filters above, this one is the beat's own decision
            // and it logs its own reason — so the registry deliberately says
            // nothing here.
            REQUIRE(unavailable->availabilityQueries == 1);
        }
    }

    SECTION("when several beats qualify")
    {
        auto* second = RegisterBeat("registry_case6_second", BeatPolarity::Either, true);

        SECTION("should offer all of them")
        {
            const auto offered = BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise);
            REQUIRE(Contains(offered, beat));
            REQUIRE(Contains(offered, second));
        }
    }

    SECTION("when debug logging is on")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=1\n"};

        SECTION("should still filter the same way")
        {
            // The debug arm exists only to account for the two silent exits, so
            // it must not change which beats come back.
            BeatRegistry::SetEnabled("registry_case6_either", false);
            REQUIRE_FALSE(Contains(BeatRegistry::AvailableMatching(ctx, BeatPolarity::Raise), beat));
        }
    }
}

TEST_CASE("BeatRegistry::Initialize", "[BeatRegistry][engine]")
{
    EngineMock engine;
    const ActiveCase active{7};

    SECTION("when called before anything is registered")
    {
        SECTION("should leave the registry alone")
        {
            // Nothing more than a log line, but it runs at kDataLoaded ahead of
            // every Register call, so it must not clear what a previous load
            // put there.
            RegisterBeat("registry_case7_survivor");
            BeatRegistry::Initialize();
            REQUIRE(BeatRegistry::Find("registry_case7_survivor") != nullptr);
        }
    }
}
