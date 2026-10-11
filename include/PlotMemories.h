#pragma once

#include <PlotModel.h>
#include <PlotVerbs.h>

#include <string>
#include <string_view>
#include <vector>

// PlotMemories — what each person in a plot remembers, and when.
//
// Every memory is one of a verb's own first-person templates filled from
// the step, so no model writes any of it and the people in a step can never
// disagree about what the step was. Written at three moments:
//
//   dispatch     the agent, on being sent; the mastermind, on sending them
//                -- or, when the mastermind is their own agent, one memory
//                of deciding to do it themselves
//   resolution   the agent, on how it went; on a failure or a timeout, the
//                mastermind, on hearing about it
//   giving up    the mastermind, once, when a plot ends unfinished with
//                them alive
//
// Nobody else in a step keeps anything yet: the template set has no memory
// for a mark, a recipient or a witness (the milestone doc's open question 4).
//
// See docs/implementation/PHASE_17_MILESTONE_2_RIGID_STEPS.md.
namespace NarrativeEngine::PlotMemories
{
    // The tag every plot memory carries, so a reader of SkyrimNet's store
    // can tell plot output from everything else -- the same arrangement as
    // GossipHarvest::kOwnOutputTag.
    inline constexpr std::string_view kTag = "narrative_engine_plot";

    struct Memory
    {
        // The NPC who keeps it, as a base form, like the rest of the plot
        // code. Write crosses to their reference.
        RE::FormID npc = 0;
        std::string text;
        float importance = 0.5f;
        // From SkyrimNet's documented set.
        std::string type;
        // The other people in the step, never the keeper.
        std::vector<RE::FormID> related;
        // The step's place, when its verb has one; otherwise empty, because
        // the people in it move on schedules and a guess would be wrong.
        std::string location;
        // Which template it came from, for the trace and the suite.
        std::string_view source;
    };

    // Pure. What a step's dispatch writes.
    [[nodiscard]] std::vector<Memory> OnDispatch(const PlotModel::Plot& plot, const PlotModel::Step& step);

    // Pure. What a step's resolution writes, from its outcome. Nothing for a
    // step that has not ended.
    [[nodiscard]] std::vector<Memory> OnResolve(const PlotModel::Plot& plot, const PlotModel::Step& step);

    // Pure. What giving a plot up writes.
    [[nodiscard]] std::vector<Memory> OnConceded(const PlotModel::Plot& plot);

    // Write each memory through SkyrimNet to its keeper's actor reference.
    // Returns how many SkyrimNet accepted. A keeper with no reference, or a
    // memory system not yet ready, costs that memory and nothing else.
    std::size_t Write(const std::vector<Memory>& memories);
} // namespace NarrativeEngine::PlotMemories
