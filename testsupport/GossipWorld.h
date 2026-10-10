#pragma once

#include "EngineMock.h"

#include <cstdint>

// A small province for the gossip graph to be built over.
//
// The graph is reconstructed from the load order — every location, its keywords
// and parent, every unique NPC's residence row, and every faction membership —
// so anything that needs a live graph needs a world for one to be built from.
// Assembling that by hand in each test file would be several dozen lines
// repeated four times, and the repetition is what makes such a fixture drift.
//
// The shape is the smallest one with something to say: two holds, each with a
// town, each town with an inn, and people living in them. That gives every tier
// the graph classifies (household, settlement, hold) at least two members, so a
// rumor has somewhere to travel at each of them, and gives two holds so that
// crossing between them is a thing that can be observed.
namespace NarrativeEngine::Testing
{
    struct GossipWorld
    {
        // Whiterun Hold: Whiterun, and the Bannered Mare inside it.
        std::uint32_t whiterunHold = 0;
        std::uint32_t whiterun = 0;
        std::uint32_t banneredMare = 0;

        // Falkreath Hold: Falkreath, and the Dead Man's Drink inside it.
        std::uint32_t falkreathHold = 0;
        std::uint32_t falkreath = 0;
        std::uint32_t deadMansDrink = 0;

        // Two people in the inn at Whiterun, one elsewhere in the same town,
        // and one away in Falkreath.
        std::uint32_t hulda = 0;
        std::uint32_t saadia = 0;
        std::uint32_t ysolda = 0;
        std::uint32_t valga = 0;

        // A faction two of them share, small enough to be admitted as a
        // gossip channel.
        std::uint32_t companions = 0;
    };

    // Builds the world above into `engine` and hands back the FormIDs. Does not
    // build the graph — a caller that wants one calls GossipGraph::Initialize
    // afterwards, and a caller testing what happens without one does not.
    GossipWorld BuildGossipWorld(EngineMock& engine);
} // namespace NarrativeEngine::Testing
