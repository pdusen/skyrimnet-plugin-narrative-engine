#include <GossipContent.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipClaims.h>
#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipSpies.h>
#include <GossipThread.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>

#include <FakeSkyrimNet.h>

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Tests for what a rumor says when it is passed on.
//
// The model writes the rumor once, in a handful of generation bands, and never
// again — cost scales with harvest sweeps, not with how far a rumor spreads. So
// everything below the seed is string work, and it is the string work that
// decides whether a memory reads like something a person would remember hearing
// or like a system notification.
//
// The framing is chosen from the tie between teller and listener, and the
// ordering of those choices carries the meaning. A kinsman who lives in another
// hold is news from away; the same kinsman down the road is family talk;
// somebody in the same house said it over supper; somebody in the same town
// just told you; and anyone else is a rumor going round. Getting the order
// wrong does not fail — it produces a memory where the player's sister in
// Riften "told me this" as though she lived next door.
//
// Every framing is a lead-in ending in a colon and never a "told me that"
// clause, because band text is one to six standalone sentences and a
// subordinating "that" can only govern the first of them.
//
// The kinship label comes off the relationship record rather than being
// invented, which is why the harness stands in for the engine's relationship
// lookup: the words "sister" and "brother" are data, and a test that hardcoded
// either would be testing itself.
//
// Above all that sits the walk, which is where the money goes. It evaluates one
// candidate at a time and stops at the first acceptance, so a pool of forty
// costs one model call in the usual case rather than forty. What it does with
// each answer is the part worth pinning: an answer about the OWNER keeps their
// memory claimed and frees the happening, so another witness can still tell it;
// an answer about the HAPPENING keeps both, so nobody tells it twice; and
// anything that means "nothing was learned" gives everything back, so a
// transient failure does not burn a memory for good.
//
// Both model calls in the walk run against the stand-in SkyrimNet beside the
// executable, which answers inline — so a walk that the shipping build spreads
// across seconds of callbacks finishes here before the call returns.

namespace
{
    namespace GossipContent = NarrativeEngine::GossipContent;
    namespace GossipClaims = NarrativeEngine::GossipClaims;
    namespace GossipDispatch = NarrativeEngine::GossipDispatch;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    namespace GossipThread = NarrativeEngine::GossipThread;
    namespace SkyrimNetAPI = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::GossipSpies;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::ResetGossipState;

    constexpr const char* kSettings = "[Gossip]\niGossipContentBands=3\n";

    constexpr std::uint32_t kTeller = 0x00D00001u;
    constexpr std::uint32_t kListener = 0x00D00002u;
    constexpr std::uint32_t kThirdParty = 0x00D00003u;

    constexpr std::uint32_t kHousehold = 0x00D10001u;
    constexpr std::uint32_t kOtherHousehold = 0x00D10002u;
    constexpr std::uint32_t kRiverwood = 0x00D20001u;
    constexpr std::uint32_t kFalkreath = 0x00D20002u;
    constexpr std::uint32_t kWhiterunHold = 0x00D30001u;
    constexpr std::uint32_t kFalkreathHold = 0x00D30002u;

    const std::string kBand = "A College mage was caught. The Arch-Mage covered it up.";

    // Puts somebody in the graph with the ties that decide how they are
    // spoken about, and gives them a name to be spoken about by.
    void Place(std::uint32_t npc,
               const char* name,
               std::uint32_t household,
               std::uint32_t settlement,
               std::uint32_t hold)
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        GossipGraph::Participant p;
        p.npc = npc;
        p.actorRef = npc + 0x00010000u;
        p.household = household;
        p.settlement = settlement;
        p.hold = hold;
        p.name = name;
        spies.participants[npc] = p;
        spies.npcNames[npc] = name;
    }

    // Memories and the happenings behind them. Claims are keyed on both, and
    // which of the two a refusal keeps is the thing most of the walk cases are
    // about.
    constexpr std::int64_t kFirstMemory = 5001;
    constexpr std::int64_t kSecondMemory = 5002;
    constexpr std::int64_t kOtherWitness = 5003;
    constexpr std::int64_t kFirstEvent = 9001;
    constexpr std::int64_t kSecondEvent = 9002;

    // Reaches the stand-in SkyrimNet. The DLL is already beside the executable,
    // so this only bumps a refcount and gives a handle to resolve through.
    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void Say(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // Claims are taken and read on the gossip worker, which is the only place
    // a token is made.
    template <class Fn> void OnGossipThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke([&](const GossipThread::Token& gt) { body(gt); });
    }

    GossipContent::Candidate CandidateFor(std::int64_t memoryId, std::int64_t eventId = kFirstEvent)
    {
        GossipContent::Candidate c;
        c.memoryId = memoryId;
        c.owner = kTeller;
        c.importance = 0.7f;
        c.text = "A College mage was caught in the Arcanaeum after hours.";
        c.locationName = "Winterhold";
        c.eventIds = {eventId};
        return c;
    }

    void Walk(std::vector<GossipContent::Candidate> pool, int maxSeeds = 4)
    {
        OnGossipThread([&](const GossipThread::Token& gt) {
            GossipContent::RequestRumors(gt, std::move(pool), maxSeeds, 100.0, {});
        });
    }

    bool IsClaimed(std::int64_t memoryId)
    {
        bool claimed = false;
        OnGossipThread(
            [&](const GossipThread::Token& gt) { claimed = NarrativeEngine::GossipClaims::IsClaimed(gt, memoryId); });
        return claimed;
    }

    bool AreEventsClaimed(const std::vector<std::int64_t>& eventIds)
    {
        bool claimed = false;
        OnGossipThread([&](const GossipThread::Token& gt) {
            claimed = NarrativeEngine::GossipClaims::AreEventsClaimed(gt, eventIds);
        });
        return claimed;
    }

    std::size_t SeededCount()
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.seeded.size();
    }

    NarrativeEngine::Testing::GossipSpyState::Seeded LastSeeded()
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        REQUIRE_FALSE(spies.seeded.empty());
        return spies.seeded.back();
    }

    std::string LastPromptName()
    {
        return std::string{FakeLLM().lastPromptName};
    }

    void NameLocation(std::uint32_t form, const char* name)
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        spies.locationNames[form] = name;
    }
} // namespace

TEST_CASE("GossipContent frames how a listener remembers hearing it", "[GossipContent][engine]")
{
    // Happy path, re-run per leaf: two strangers in the same hold and nothing
    // else in common, which is how most gossip travels. Each case adds the one
    // tie it is about.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    GossipSpies().Reset();
    NameLocation(kRiverwood, "Riverwood");
    NameLocation(kFalkreath, "Falkreath");
    Place(kTeller, "Sven", kHousehold, kRiverwood, kWhiterunHold);
    Place(kListener, "Camilla", kOtherHousehold, kFalkreath, kWhiterunHold);

    SECTION("when the two are barely connected")
    {
        Place(kTeller, "Sven", kHousehold, kRiverwood, kWhiterunHold);
        Place(kListener, "Camilla", kOtherHousehold, kFalkreath, kFalkreathHold);

        SECTION("should call it a rumor going round")
        {
            // The common case by a wide margin, and it is genuinely how most
            // gossip arrives — from nobody in particular.
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "I heard a rumor going round: " + kBand);
        }
    }

    SECTION("when the two live in the same town")
    {
        Place(kListener, "Camilla", kOtherHousehold, kRiverwood, kWhiterunHold);

        SECTION("should name who told them")
        {
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "Sven told me this: " + kBand);
        }
    }

    SECTION("when the two share a house")
    {
        Place(kListener, "Camilla", kHousehold, kRiverwood, kWhiterunHold);

        SECTION("should place it over supper")
        {
            // The household tie is checked before the settlement one, because
            // people who live together heard it somewhere more specific than
            // "in town".
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "Sven mentioned this over supper: " + kBand);
        }
    }

    SECTION("when the two are family in the same hold")
    {
        auto* teller = engine.AddNPC(kTeller);
        auto* listener = engine.AddNPC(kListener, /*female=*/true);
        engine.AddRelationship(teller, listener, "brother", "sister");
        Place(kListener, "Camilla", kOtherHousehold, kFalkreath, kWhiterunHold);

        SECTION("should use the word the record uses for them")
        {
            // Not a word this test chose. Kinship terms are gendered and come
            // off BGSAssociationType, so a mod that renames them renames them
            // here too.
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "My sister Sven told me this: " + kBand);
        }
    }

    SECTION("when the two are family in different holds")
    {
        auto* teller = engine.AddNPC(kTeller);
        auto* listener = engine.AddNPC(kListener, /*female=*/true);
        engine.AddRelationship(teller, listener, "brother", "sister");
        Place(kListener, "Camilla", kOtherHousehold, kFalkreath, kFalkreathHold);

        SECTION("should make it news from away")
        {
            // Which is the point of the distinction: a rumor that crossed a
            // hold border to reach someone arrived with a person, and saying
            // where from is what makes the distance readable.
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "My sister came from Riverwood with news: " + kBand);
        }
    }

    SECTION("when neither is in the graph")
    {
        GossipSpies().Reset();

        SECTION("should still say something")
        {
            // The memory is written whether or not the graph can describe the
            // pair, because a rumor that produced no memory did not happen as
            // far as anyone in the world is concerned.
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard == "I heard a rumor going round: " + kBand);
        }
    }

    SECTION("when any framing at all is chosen")
    {
        SECTION("should carry the rumor's own words through")
        {
            REQUIRE(GossipContent::ComposeHeard(kBand, kTeller, kListener).ends_with(kBand));
        }

        SECTION("should end its lead-in with a colon")
        {
            // Band text is up to six standalone sentences. A "told me that"
            // clause governs only the first of them and reads as a grammatical
            // error from the second on.
            const auto heard = GossipContent::ComposeHeard(kBand, kTeller, kListener);
            REQUIRE(heard.find(": ") != std::string::npos);
            REQUIRE(heard.find(" that ") == std::string::npos);
        }
    }
}

TEST_CASE("GossipContent names everyone a teller told", "[GossipContent][engine]")
{
    // One memory per teller per tick rather than one per telling: a carrier can
    // pass the same rumor on several times in an afternoon, and three
    // near-identical rows is both worse reading and more for the harvester to
    // wade back through.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    GossipSpies().Reset();
    Place(kTeller, "Sven", kHousehold, kRiverwood, kWhiterunHold);
    Place(kListener, "Camilla", kOtherHousehold, kRiverwood, kWhiterunHold);
    Place(kThirdParty, "Lucan", kOtherHousehold, kRiverwood, kWhiterunHold);

    SECTION("when they told one person")
    {
        SECTION("should name them")
        {
            REQUIRE(GossipContent::ComposeTold(kBand, {kListener}) == "I told Camilla this: " + kBand);
        }
    }

    SECTION("when they told two people")
    {
        SECTION("should join the names with an and")
        {
            REQUIRE(GossipContent::ComposeTold(kBand, {kListener, kThirdParty})
                    == "I told Camilla and Lucan this: " + kBand);
        }
    }

    SECTION("when they told three people")
    {
        SECTION("should comma the list and finish with an and")
        {
            // No Oxford comma: these are spoken-voice memories rather than
            // prose, and an NPC recalling their afternoon does not punctuate
            // like an editor.
            REQUIRE(GossipContent::ComposeTold(kBand, {kListener, kThirdParty, kTeller})
                    == "I told Camilla, Lucan and Sven this: " + kBand);
        }
    }

    SECTION("when one of them is not in the graph")
    {
        SECTION("should still name the rest")
        {
            // A participant can leave the graph between the telling and the
            // memory being written — they died, or the cell they were in went
            // away. Dropping the whole memory over one name would lose the
            // others too.
            REQUIRE(GossipContent::ComposeTold(kBand, {kListener, 0x00DEAD01u})
                    == "I told Camilla and someone this: " + kBand);
        }
    }
}

TEST_CASE("GossipContent picks a telling for how far it has come", "[GossipContent][engine]")
{
    // Bands are how a rumor degrades: the first carriers tell it in full, and
    // each remove loses detail. Which band applies is decided here.
    EngineMock engine;

    SECTION("when three bands are configured")
    {
        const ConfiguredSettings settings{"[Gossip]\niGossipContentBands=3\n"};

        SECTION("should give the first three generations the freshest telling")
        {
            REQUIRE(GossipContent::BandForGeneration(0) == 0);
            REQUIRE(GossipContent::BandForGeneration(2) == 0);
        }

        SECTION("should step down every three generations")
        {
            REQUIRE(GossipContent::BandForGeneration(3) == 1);
            REQUIRE(GossipContent::BandForGeneration(5) == 1);
            REQUIRE(GossipContent::BandForGeneration(6) == 2);
        }

        SECTION("should stop stepping at the last band")
        {
            // A rumor twenty removes out has no twentieth telling to read, and
            // indexing past the end is what would happen instead.
            REQUIRE(GossipContent::BandForGeneration(60) == 2);
        }
    }

    SECTION("when only one band is configured")
    {
        const ConfiguredSettings settings{"[Gossip]\niGossipContentBands=1\n"};

        SECTION("should give every generation the same telling")
        {
            REQUIRE(GossipContent::BandForGeneration(0) == 0);
            REQUIRE(GossipContent::BandForGeneration(99) == 0);
        }
    }

    SECTION("when the band count is set to nothing")
    {
        const ConfiguredSettings settings{"[Gossip]\niGossipContentBands=0\n"};

        SECTION("should still answer with a band that exists")
        {
            // The index is used to subscript the band list, so a zero count
            // has to floor at one rather than produce an index of minus one.
            REQUIRE(GossipContent::BandForGeneration(7) == 0);
        }
    }
}

TEST_CASE("GossipContent walks a pool one candidate at a time", "[GossipContent][engine]")
{
    // Happy path, re-run per leaf: one candidate nobody has claimed, and a
    // model that approves it and writes its bands.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    GossipSpies().Reset();
    ResetGossipState();
    REQUIRE(SkyrimNetAPI::Initialize());
    auto& llm = FakeLLM();
    llm.sendPromptAccepts = true;
    llm.sendPromptSucceeds = true;
    llm.sendPromptCalls = 0;
    Say(llm.promptResponse, R"({"verdict":"seed","bands":["first","second","third"]})");
    Place(kTeller, "Sven", kHousehold, kRiverwood, kWhiterunHold);

    SECTION("when the model approves a candidate")
    {
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should seed a rumor from it")
        {
            REQUIRE(SeededCount() == 1);
        }

        SECTION("should seed it with the bands the model wrote")
        {
            REQUIRE(LastSeeded().bands == std::vector<std::string>{"first", "second", "third"});
        }

        SECTION("should credit the memory it came from")
        {
            // The simulation records this so the harvester never offers the
            // same memory again, and so a rumor can be traced back.
            REQUIRE(LastSeeded().sourceMemoryId == kFirstMemory);
            REQUIRE(LastSeeded().originNpc == kTeller);
        }

        SECTION("should leave the memory claimed")
        {
            REQUIRE(IsClaimed(kFirstMemory));
        }
    }

    SECTION("when the model returns fewer bands than there are")
    {
        Say(llm.promptResponse, R"({"verdict":"seed","bands":["only one"]})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should repeat the last one to fill them")
        {
            // Two usable tellings out of three is still a usable rumor, and
            // refusing the seed over it would spend a director call and an
            // expensive composer call for nothing.
            REQUIRE(LastSeeded().bands.size() == 3);
            REQUIRE(LastSeeded().bands.back() == "only one");
        }
    }

    SECTION("when the owner would not repeat it")
    {
        Say(llm.promptResponse, R"({"verdict":"private"})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should seed nothing")
        {
            REQUIRE(SeededCount() == 0);
        }

        SECTION("should never ask the composer")
        {
            // The whole point of splitting the two calls: a refusal costs the
            // cheap model and nothing else, and the refusals outnumber the
            // acceptances.
            REQUIRE(LastPromptName() == "narrative_engine_gossip_eval");
        }

        SECTION("should keep the memory but free the happening")
        {
            // A judgement about this person, not about the event. Somebody
            // else who saw the same thing may still be willing to tell it.
            REQUIRE(IsClaimed(kFirstMemory));
            REQUIRE_FALSE(AreEventsClaimed({kFirstEvent}));
        }
    }

    SECTION("when the story is not worth telling")
    {
        Say(llm.promptResponse, R"({"verdict":"not_worthy"})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should keep the memory but free the happening")
        {
            REQUIRE(IsClaimed(kFirstMemory));
            REQUIRE_FALSE(AreEventsClaimed({kFirstEvent}));
        }
    }

    SECTION("when the story is already going round")
    {
        Say(llm.promptResponse, R"({"verdict":"duplicate","duplicate_of":"the mage business"})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should keep the happening claimed too")
        {
            // A judgement about the event rather than the person. Releasing it
            // would let a second witness start a second rumor about the one
            // thing that happened.
            REQUIRE(IsClaimed(kFirstMemory));
            REQUIRE(AreEventsClaimed({kFirstEvent}));
        }
    }

    SECTION("when the answer means nothing at all")
    {
        Say(llm.promptResponse, R"({"verdict":"perhaps"})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should give the memory back")
        {
            // An unrecognised verdict is a fault in the prompt or the model,
            // not a judgement about the memory. Keeping the claim would burn a
            // memory for the rest of the save over a bad afternoon.
            REQUIRE_FALSE(IsClaimed(kFirstMemory));
            REQUIRE_FALSE(AreEventsClaimed({kFirstEvent}));
        }
    }

    SECTION("when the model cannot be reached")
    {
        llm.sendPromptSucceeds = false;
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should give the memory back")
        {
            REQUIRE_FALSE(IsClaimed(kFirstMemory));
        }
    }

    SECTION("when the composer answers with no bands")
    {
        Say(llm.promptResponse, R"({"verdict":"seed","bands":[]})");
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should seed nothing and give the memory back")
        {
            // Composition failing is not the memory's fault, so it returns to
            // the pool — but the sweep's seed budget is spent either way, or a
            // tick's cost would be bounded by nothing the settings say.
            REQUIRE(SeededCount() == 0);
            REQUIRE_FALSE(IsClaimed(kFirstMemory));
        }
    }

    SECTION("when the simulation refuses the seed")
    {
        GossipSpies().nextRumorID = 0;
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should give the memory back")
        {
            // The live-rumor cap being full is a temporary state, and a memory
            // burned on one is a memory nobody ever hears about.
            REQUIRE_FALSE(IsClaimed(kFirstMemory));
        }
    }

    SECTION("when a candidate was claimed since the pool was built")
    {
        OnGossipThread(
            [](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kFirstMemory, {kFirstEvent}, 100.0); });
        Walk({CandidateFor(kFirstMemory), CandidateFor(kSecondMemory, kSecondEvent)});

        SECTION("should skip it and carry on to the next")
        {
            REQUIRE(SeededCount() == 1);
            REQUIRE(LastSeeded().sourceMemoryId == kSecondMemory);
        }
    }

    SECTION("when another account of the same happening is already claimed")
    {
        OnGossipThread(
            [](const GossipThread::Token& gt) { GossipClaims::Claim(gt, kOtherWitness, {kFirstEvent}, 100.0); });
        Walk({CandidateFor(kFirstMemory)});

        SECTION("should skip it")
        {
            // Two people saw one thing. Only one rumor should come of it.
            REQUIRE(SeededCount() == 0);
        }
    }

    SECTION("when the pool holds more than the sweep may seed")
    {
        Walk({CandidateFor(kFirstMemory), CandidateFor(kSecondMemory, kSecondEvent)}, /*maxSeeds=*/1);

        SECTION("should stop at its budget")
        {
            // The pool is deliberately shuffled before it gets here, so
            // stopping early is a uniform choice among the acceptable rather
            // than a bias towards whatever sorted first.
            REQUIRE(SeededCount() == 1);
        }
    }

    SECTION("when the world is replaced mid-walk")
    {
        SECTION("should stop and give the memory back")
        {
            // A tick that keeps going past a load spends model calls on a
            // world that no longer exists, and worse, can seed a rumor into
            // it.
            auto cancel = std::make_shared<GossipDispatch::CancellationToken>();
            cancel->Cancel();
            OnGossipThread([&](const GossipThread::Token& gt) {
                GossipContent::RequestRumors(gt, {CandidateFor(kFirstMemory)}, 1, 100.0, cancel);
            });
            REQUIRE(SeededCount() == 0);
            REQUIRE_FALSE(IsClaimed(kFirstMemory));
        }
    }
}
