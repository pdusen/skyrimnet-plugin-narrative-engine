#pragma once

#include <string>
#include <vector>

#include <PlotMenus.h>
#include <PlotModel.h>
#include <PlotThread.h>

// PlotBirth — turning one LLM response into a plot, or into nothing.
//
// The response is untrusted input from a system that is wrong sometimes,
// so every way it can be wrong has to end somewhere defined. There are
// only two outcomes here: a plot in which every noun is a resolved form,
// or a rejection with a reason. There is deliberately no third case
// where a partly-good response yields a partly-built plot — a plan
// missing its middle is worse than no plan, because it looks runnable.
//
// ---------------------------------------------------------------------
// Validation is a MEMBERSHIP TEST
//
// The model never writes a name. It writes an index into a menu the
// plugin built, and the check is whether that index is on the menu. That
// is why this file has no fuzzy matching, no nearest-name lookup and no
// "did they mean" logic: those are what a system that accepts names
// needs, and each of them is a way to accept something wrong.
//
// See docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md for what accepting
// names cost the project this one learned from.
namespace NarrativeEngine::PlotBirth
{
    // Why a response was thrown away. One per rejection, and specific
    // enough to act on: "step 2 named target 7 of 4" says which step,
    // which index, and how far out of range it was.
    struct Outcome
    {
        bool ok = false;
        std::string rejection;

        // Only meaningful when ok.
        std::string ambition;
        // In order. The LAST entry is the objective; everything before
        // it is preparation. That is the model's own convention from
        // the prompt, and it is checked rather than assumed.
        std::vector<PlotModel::Step> plan;
    };

    struct Limits
    {
        std::size_t minSteps = 2;
        std::size_t maxSteps = 6;
        // A hard ceiling on the free-form field, applied AFTER
        // sanitizing. An ambition is one sentence; anything past this is
        // the model ignoring the instruction, and letting it through
        // would put an essay in the co-save and on the dashboard.
        std::size_t maxAmbition = 400;
    };

    // Pure. Response text in, plot-or-rejection out.
    //
    // Takes the menus it was built from, because the indices mean
    // nothing without them — and takes them BY VALUE of the same object
    // the prompt was rendered from, so a menu rebuilt between the two
    // cannot silently renumber the answer.
    [[nodiscard]] Outcome Parse(const std::string& response, const PlotMenus::Menus& menus, const Limits& limits);

    // --- The engine-bound half -------------------------------------------

    // Ask the model for a plot for `mastermind`, and fill `plot` in.
    //
    // BLOCKS on the plot worker, which is the point: nothing else runs
    // there, so there is nobody to yield to and no reason to hand the
    // rest of the work to a callback. Same argument GossipContent makes
    // for its own calls.
    //
    // Returns false when the call failed or the response was rejected,
    // having logged why. The caller frees the slot.
    [[nodiscard]] bool Compose(const PlotThread::Token& pt,
                               const PlotCasting::Member& mastermind,
                               const PlotMenus::Menus& menus,
                               PlotModel::Plot& plot);
} // namespace NarrativeEngine::PlotBirth
