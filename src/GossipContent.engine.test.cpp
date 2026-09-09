#include <GossipContent.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipGraph.h>
#include <GossipSpies.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

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

namespace
{
    namespace GossipContent = NarrativeEngine::GossipContent;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::GossipSpies;

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
