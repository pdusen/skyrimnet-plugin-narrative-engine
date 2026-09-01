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
        //
        // The AGENDA: the larger thing this scheme ostensibly serves,
        // and deliberately not something this system can ever finish.
        // "Eliminate Talos worship in Skyrim." "Become Queen of Skyrim."
        //
        // The first version of this asked for "what this person wants
        // and why they will not ask openly", which is a paraphrase of
        // the objective -- so ambition and objective said the same
        // thing, nothing pulled the objective toward anything larger,
        // and every plot came out as a small theft with a motive
        // attached. The design always called it "the larger thing this
        // objective serves"; that is what it is now.
        std::string ambition;

        // The WHOLE scheme, in order, ending on the step that
        // accomplishes the objective.
        //
        // The objective used to be appended here as an extra final
        // step, and it read as padding: a plan that ended "deliver an
        // incriminating letter to Kodlak Whitemane" already discredited
        // Aela the Huntress, and tacking "Discredit Aela the Huntress"
        // on after it said nothing the previous step had not. The
        // objective is what the scheme is FOR, not a move in it.
        std::vector<PlotModel::Step> plan;

        // The destination, as a type and a target. Names the plot and
        // anchors adaptation; never appears in `plan`.
        //
        // Still chosen by the model as its own field rather than
        // inferred from whichever step happened to be last. Inferring
        // it produced plots titled "Cover Tracks Sybille Stentor":
        // `conceal` is a natural closing step, so it kept becoming the
        // objective, and covering your tracks is never what a scheme is
        // FOR. Choosing it explicitly also puts it in the right order --
        // pick the destination, then the route.
        PlotModel::Step objective;
    };

    struct Limits
    {
        // Bounds on the whole plan. Back to 2..6 now that the objective
        // is no longer appended on top of it -- a plot runs exactly this
        // many steps.
        //
        // Deliberately not stated in the prompt. Telling the model "2 to
        // 6" produced 4, 4, 4 -- the midpoint of any range offered is
        // where it lands. The bound belongs here, where it is enforced,
        // rather than in the prose, where it is a suggestion that
        // doubles as an anchor.
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
