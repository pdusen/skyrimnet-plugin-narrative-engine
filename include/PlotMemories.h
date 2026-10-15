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
// disagree about what the step was. Written at these moments:
//
//   dispatch     the agent, on being sent; the mastermind, on sending them
//                -- or, when the mastermind is their own agent, one memory
//                of deciding to do it themselves
//   resolution   the agent, on how it went; the mastermind who sent them, on
//                hearing of a success; and everybody the step reached, by
//                their slot's awareness, each from their slot's own template
//   answered     the mastermind, on a failed step, once the plot has decided
//                what to do about it: one memory of the failure, ending in
//                going on another way when it does
//   the end      the mastermind, on bringing the scheme off, on its failing
//                at the finish, or on giving it up
//
// Only the living keep anything: Write skips a keeper who is not alive. And
// nobody keeps two memories of one plot from one tick: the plot worker
// gathers a plot's memories as it advances it and Combines them per keeper.
//
// See docs/implementation/PHASE_17_MILESTONE_2_RIGID_STEPS.md and
// PHASE_17_MILESTONE_3_PLOT_MEMORIES.md.
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
        // Which template it came from, for the trace and the suite; for a
        // combined memory, each of its statements' in order, joined by '+'.
        std::string source;
    };

    // Pure. What a step's dispatch writes.
    [[nodiscard]] std::vector<Memory> OnDispatch(const PlotModel::Plot& plot, const PlotModel::Step& step);

    // Pure. What a step's resolution writes, from its outcome. Nothing for a
    // step that has not ended. The mastermind's memory of a failure is not
    // among it: see OnFailureAnswered.
    [[nodiscard]] std::vector<Memory> OnResolve(const PlotModel::Plot& plot, const PlotModel::Step& step);

    // Pure. The mastermind's one memory of a failed or timed-out step, once
    // the plot has answered it: the report of it, or their own failure when
    // they did the step themselves, ending in kWentOnClause when the plot
    // went on another way. Nothing for a step that did not fail.
    [[nodiscard]] std::vector<Memory> OnFailureAnswered(const PlotModel::Plot& plot,
                                                        const PlotModel::Step& step,
                                                        bool wentOn);

    // Pure. What bringing the scheme off writes.
    [[nodiscard]] std::vector<Memory> OnSucceeded(const PlotModel::Plot& plot);

    // Pure. What the scheme failing at its finish writes.
    [[nodiscard]] std::vector<Memory> OnFinishFailed(const PlotModel::Plot& plot);

    // Pure. What giving a plot up writes.
    [[nodiscard]] std::vector<Memory> OnConceded(const PlotModel::Plot& plot);

    // Pure. One memory per keeper, in the order keepers first appear: their
    // statements joined into a paragraph in the order they were made, the
    // most important statement's importance and type, everybody any of them
    // relates, and the first location any of them has. A keeper with one
    // statement keeps it as it was.
    [[nodiscard]] std::vector<Memory> Combine(const std::vector<Memory>& memories);

    // Write each memory through SkyrimNet to its keeper's actor reference.
    // Returns how many SkyrimNet accepted. A keeper with no reference, or a
    // memory system not yet ready, costs that memory and nothing else.
    std::size_t Write(const std::vector<Memory>& memories);
} // namespace NarrativeEngine::PlotMemories
