#include <GossipClaims.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipSpies.h>
#include <GossipThread.h>
#include <ThreadRole.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Tests for the ledger of memories already turned into rumours.
//
// One claim per memory, and while it stands that memory can never be a rumour
// again. Everything below follows from that, and every failure is silent in a
// specific way.
//
// The event claims are the part worth understanding. SkyrimNet writes ONE
// MEMORY PER ACTOR PRESENT at a happening, each with its own id but overlapping
// related-event ids — so per-memory dedup alone lets the same happening become
// as many rumours as there were witnesses. Claiming the events is what stops
// that, and it is invisible until a tavern is repeating one brawl five times.
//
// A repeated claim must not push the expiry out, or a memory the harvest keeps
// re-examining is held for ever. And a failed generation must give the events
// back while a wrong-owner verdict must not: the first means nobody is telling
// this story, the second means this NPC is not, and someone else who was there
// still might.
//
// The ledger lives on GossipState, which the harness owns as a real struct, so
// these cases read the ledger directly rather than through a stand-in.

namespace
{
    namespace GossipClaims = NarrativeEngine::GossipClaims;
    namespace GossipThread = NarrativeEngine::GossipThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::LiveGossipState;
    using NarrativeEngine::Testing::ResetGossipState;

    constexpr std::int64_t kMemory = 4242;
    constexpr std::int64_t kOtherMemory = 4243;
    constexpr std::int64_t kEventA = 900;
    constexpr std::int64_t kEventB = 901;

    // Claims run on the gossip worker, which is the only place a token is
    // made.
    template <class Fn> void OnGossipThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke([&](const GossipThread::Token& gt) { body(gt); });
    }

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    std::size_t ClaimCount()
    {
        return GossipClaims::Count(LiveGossipState());
    }
    std::size_t EventClaimCount()
    {
        return GossipClaims::EventCount(LiveGossipState());
    }
} // namespace

TEST_CASE("GossipClaims::Claim", "[GossipClaims][engine]")
{
    // Happy path, re-run per leaf: an empty ledger and a ten-day expiry, with
    // the simulation clock at day 100.
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();

    SECTION("when a memory is claimed")
    {
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {}, 100.0); });

        SECTION("should hold it")
        {
            OnGossipThread([](const GossipThread::Token& gt) { REQUIRE(GossipClaims::IsClaimed(gt, kMemory)); });
        }

        SECTION("should hold only that memory")
        {
            OnGossipThread(
                [](const GossipThread::Token& gt) { REQUIRE_FALSE(GossipClaims::IsClaimed(gt, kOtherMemory)); });
            REQUIRE(ClaimCount() == 1);
        }

        SECTION("should set the expiry a full window out")
        {
            const auto& claims = LiveGossipState().claims;
            REQUIRE(claims.at(kMemory) == 110.0);
        }
    }

    SECTION("when the same memory is claimed twice")
    {
        OnGossipThread([](const GossipThread::Token& gt) {
            GossipClaims::Claim(gt, kMemory, {}, 100.0);
            GossipClaims::Claim(gt, kMemory, {}, 105.0);
        });

        SECTION("should keep the original expiry")
        {
            // Emplace rather than assign. A harvest that keeps re-examining
            // the same memory would otherwise push its expiry forward every
            // time and hold it for ever, and the memory would never come back.
            REQUIRE(LiveGossipState().claims.at(kMemory) == 110.0);
        }

        SECTION("should still hold one claim")
        {
            REQUIRE(ClaimCount() == 1);
        }
    }

    SECTION("when the memory carries related events")
    {
        OnGossipThread(
            [](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {kEventA, kEventB}, 100.0); });

        SECTION("should claim each event too")
        {
            // The point of the whole event ledger. SkyrimNet writes one memory
            // per actor present at a happening, so without this the same brawl
            // becomes as many rumours as there were witnesses.
            OnGossipThread(
                [](const GossipThread::Token& gt) { REQUIRE(GossipClaims::AreEventsClaimed(gt, {kEventA})); });
            REQUIRE(EventClaimCount() == 2);
        }

        SECTION("should lock out another witness's account of the same event")
        {
            // A different memory id, overlapping event ids: exactly what a
            // second witness's memory looks like.
            OnGossipThread(
                [](const GossipThread::Token& gt) { REQUIRE(GossipClaims::AreEventsClaimed(gt, {kEventB, 999})); });
        }

        SECTION("should give the events the same expiry as the memory")
        {
            REQUIRE(LiveGossipState().eventClaims.at(kEventA).expiresOnGameDay == 110.0);
        }
    }

    SECTION("when the expiry setting is nonsense")
    {
        const ConfiguredSettings tiny{"[Gossip]\nfGossipClaimExpiryDays=0.0\n"};
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {}, 100.0); });

        SECTION("should still hold the claim for at least a day")
        {
            // Floored rather than trusted. A zero expiry would hand the memory
            // straight back on the next sweep, and the same story would go
            // round again on the following tick.
            REQUIRE(LiveGossipState().claims.at(kMemory) >= 101.0);
        }
    }
}

TEST_CASE("GossipClaims::AreEventsClaimed", "[GossipClaims][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();

    SECTION("when nothing has been claimed")
    {
        SECTION("should say no")
        {
            OnGossipThread(
                [](const GossipThread::Token& gt) { REQUIRE_FALSE(GossipClaims::AreEventsClaimed(gt, {kEventA})); });
        }
    }

    SECTION("when the memory has no related events")
    {
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {kEventA}, 100.0); });

        SECTION("should say no rather than treat an empty list as a match")
        {
            // A memory with no related events is common, and answering true
            // for one would exclude every such memory from ever being gossip.
            OnGossipThread(
                [](const GossipThread::Token& gt) { REQUIRE_FALSE(GossipClaims::AreEventsClaimed(gt, {})); });
        }
    }
}

TEST_CASE("GossipClaims::Release", "[GossipClaims][engine]")
{
    // A rumour is claimed BEFORE its content is generated, so a failed
    // generation has to hand everything back or a transient error permanently
    // burns a memory that never produced anything.
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();
    OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {kEventA, kEventB}, 100.0); });

    SECTION("when generation fails")
    {
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Release(gt, kMemory); });

        SECTION("should hand the memory back")
        {
            OnGossipThread([](const GossipThread::Token& gt) { REQUIRE_FALSE(GossipClaims::IsClaimed(gt, kMemory)); });
        }

        SECTION("should hand the events back too")
        {
            // Otherwise a failed generation keeps every other witness's account
            // of the same happening locked out for the whole expiry window,
            // for nothing.
            OnGossipThread([](const GossipThread::Token& gt) {
                REQUIRE_FALSE(GossipClaims::AreEventsClaimed(gt, {kEventA, kEventB}));
            });
            REQUIRE(EventClaimCount() == 0);
        }
    }

    SECTION("when only this owner is ruled out")
    {
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::ReleaseEvents(gt, kMemory); });

        SECTION("should keep holding the memory")
        {
            // This NPC will not be telling this story, so asking them about it
            // again is wasted work.
            OnGossipThread([](const GossipThread::Token& gt) { REQUIRE(GossipClaims::IsClaimed(gt, kMemory)); });
        }

        SECTION("should leave the happening open to another witness")
        {
            // The distinction that makes two release paths worth having: the
            // happening itself is still fair game, and someone else who was
            // there may have an account worth repeating.
            OnGossipThread([](const GossipThread::Token& gt) {
                REQUIRE_FALSE(GossipClaims::AreEventsClaimed(gt, {kEventA, kEventB}));
            });
        }
    }

    SECTION("when a memory nobody claimed is released")
    {
        SECTION("should leave the ledger alone")
        {
            OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Release(gt, kOtherMemory); });
            OnGossipThread([](const GossipThread::Token& gt) { REQUIRE(GossipClaims::IsClaimed(gt, kMemory)); });
            REQUIRE(EventClaimCount() == 2);
        }
    }

    SECTION("when one memory's events are released")
    {
        // A second memory holds an event of its own. Releasing the first
        // memory's events must not reach it.
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kOtherMemory, {777}, 100.0); });
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Release(gt, kMemory); });

        SECTION("should leave another memory's event claims standing")
        {
            OnGossipThread([](const GossipThread::Token& gt) { REQUIRE(GossipClaims::AreEventsClaimed(gt, {777})); });
        }
    }
}

TEST_CASE("GossipClaims::Sweep", "[GossipClaims][engine]")
{
    // Run from the simulation poll on sampled game time rather than on rumour
    // activity, so a quiet stretch with no live rumours still expires claims.
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();
    OnGossipThread([](const GossipThread::Token& gt) {
        GossipClaims::Claim(gt, kMemory, {kEventA}, 100.0);
        GossipClaims::Claim(gt, kOtherMemory, {kEventB}, 200.0);
    });

    SECTION("when nothing has expired yet")
    {
        std::size_t dropped = 0;
        OnGossipThread([&](const GossipThread::Token& gt) { dropped = GossipClaims::Sweep(gt, 105.0); });

        SECTION("should drop nothing")
        {
            REQUIRE(dropped == 0);
            REQUIRE(ClaimCount() == 2);
        }
    }

    SECTION("when one claim has aged out")
    {
        std::size_t dropped = 0;
        OnGossipThread([&](const GossipThread::Token& gt) { dropped = GossipClaims::Sweep(gt, 150.0); });

        SECTION("should drop only that one")
        {
            REQUIRE(dropped == 1);
            OnGossipThread([](const GossipThread::Token& gt) {
                REQUIRE_FALSE(GossipClaims::IsClaimed(gt, kMemory));
                REQUIRE(GossipClaims::IsClaimed(gt, kOtherMemory));
            });
        }

        SECTION("should take its event claims with it")
        {
            // Event claims age on their own stored expiry rather than with
            // their memory, so a stray one can never outlive the sweep that
            // should have taken it.
            OnGossipThread([](const GossipThread::Token& gt) {
                REQUIRE_FALSE(GossipClaims::AreEventsClaimed(gt, {kEventA}));
                REQUIRE(GossipClaims::AreEventsClaimed(gt, {kEventB}));
            });
        }
    }

    SECTION("when the clock reaches an expiry exactly")
    {
        SECTION("should drop it")
        {
            // The comparison is `<=`. Off by one here leaves a claim standing
            // for a whole extra sweep interval, every time.
            std::size_t dropped = 0;
            OnGossipThread([&](const GossipThread::Token& gt) { dropped = GossipClaims::Sweep(gt, 110.0); });
            REQUIRE(dropped == 1);
        }
    }
}

TEST_CASE("GossipClaims survives a save and load", "[GossipClaims][engine]")
{
    // The ledger is saved from the same instant as the rumours it produced.
    // Split them and a reload restores a rumour whose source memory is no
    // longer claimed, leaving that memory free to seed a second rumour about a
    // happening already going round.
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();
    OnGossipThread([](const GossipThread::Token& gt) {
        GossipClaims::Claim(gt, kMemory, {kEventA, kEventB}, 100.0);
        GossipClaims::Claim(gt, kOtherMemory, {}, 200.0);
    });

    SECTION("when the ledger is written and read back")
    {
        GossipClaims::OnSave(FakeInterface(), LiveGossipState());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        ResetGossipState();
        GossipClaims::OnLoad(FakeInterface(), 2, 0);

        SECTION("should restore every memory claim")
        {
            REQUIRE(NarrativeEngine::Testing::StagedGossipState().claims.size() == 2);
        }

        SECTION("should restore each claim's own expiry")
        {
            // Two claims taken at different moments. A loader that wrote one
            // expiry for all of them would look right until the first sweep.
            const auto& claims = NarrativeEngine::Testing::StagedGossipState().claims;
            REQUIRE(claims.at(kMemory) == 110.0);
            REQUIRE(claims.at(kOtherMemory) == 210.0);
        }

        SECTION("should restore the event claims under their own memory")
        {
            // Events carry neither an expiry nor an owner on disk: both are
            // implied by the claim they sit under, which is what makes a
            // dangling event claim unrepresentable rather than merely unlikely.
            const auto& events = NarrativeEngine::Testing::StagedGossipState().eventClaims;
            REQUIRE(events.size() == 2);
            REQUIRE(events.at(kEventA).claimedByMemoryId == kMemory);
            REQUIRE(events.at(kEventA).expiresOnGameDay == 110.0);
        }
    }

    SECTION("when the record stamps its type and version")
    {
        GossipClaims::OnSave(FakeInterface(), LiveGossipState());

        SECTION("should use the frozen record type")
        {
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == GossipClaims::kRecordTypeId);
            REQUIRE(engine.cosave.opened[0].version == 2);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            GossipClaims::OnSave(FakeInterface(), LiveGossipState());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            GossipClaims::OnSave(nullptr, LiveGossipState());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("GossipClaims::OnLoad rejects a payload it cannot trust", "[GossipClaims][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[Gossip]\nfGossipClaimExpiryDays=10.0\n"};
    ResetGossipState();
    OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kMemory, {kEventA}, 100.0); });
    GossipClaims::OnSave(FakeInterface(), LiveGossipState());
    const auto goodPayload = engine.cosave.written;
    ResetGossipState();

    SECTION("when the version is one no build ever wrote")
    {
        engine.cosave.readable = goodPayload;
        engine.cosave.readCursor = 0;

        SECTION("should restore nothing")
        {
            // Restoring a ledger from the wrong offsets is worse than none: it
            // would hold memories nobody ever claimed and free ones that are
            // already rumours.
            GossipClaims::OnLoad(FakeInterface(), 9, 0);
            REQUIRE(NarrativeEngine::Testing::StagedGossipState().claims.empty());
        }
    }

    SECTION("when the record ends before its count")
    {
        SECTION("should restore nothing")
        {
            GossipClaims::OnLoad(FakeInterface(), 2, 0);
            REQUIRE(NarrativeEngine::Testing::StagedGossipState().claims.empty());
        }
    }

    SECTION("when the record is truncated mid-entry")
    {
        engine.cosave.readable = goodPayload;
        engine.cosave.readable.resize(goodPayload.size() - 4);
        engine.cosave.readCursor = 0;

        SECTION("should restore nothing")
        {
            GossipClaims::OnLoad(FakeInterface(), 2, 0);
            REQUIRE(NarrativeEngine::Testing::StagedGossipState().claims.empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should leave the staged state alone")
        {
            GossipClaims::OnLoad(nullptr, 2, 0);
            REQUIRE(NarrativeEngine::Testing::StagedGossipState().claims.empty());
        }
    }
}
