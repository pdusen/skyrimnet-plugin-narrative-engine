#pragma once

#include <string>
#include <vector>

#include <PlotModel.h>
#include <PlotThread.h>

// PlotAdapt — what happens when a step fails and the plot does not.
//
// A scheme that ends at its first setback is not a scheme, and one that
// re-plans forever is not a story. Adaptation is the middle: the
// mastermind gets another way to the same end, a bounded number of
// times, and may decide at any point that it is not worth it.
//
// ---------------------------------------------------------------------
// Conceding is an explicit option, not an emergent one
//
// The model is offered "give up" in the response schema. Without that it
// invents alternatives indefinitely, because inventing one is always
// easier than declining to — and the plots that come out are the ones
// where somebody grinds through six approaches to steal a mead bottle.
// A refusal the model can express is a refusal it will use.
//
// ---------------------------------------------------------------------
// The scheme is not writable
//
// Adaptation rewrites the PATH, never the destination. The plot's
// scheme is fixed at birth and is the thing the whole plot is a record
// of wanting; a system that let it drift would produce plots whose
// ambition text no longer describes them, and no way to notice.
//
// This used to be a CHECK -- the revised plan had to end on the
// objective step, and one that ended elsewhere was rejected. It is now
// structural: the scheme is a sentence on the Plot, it is not in the
// plan, and a revision has no way to reach it. Stronger, and one fewer
// rule to get wrong.
//
// Like birth, this call is offered no menu of people or objects; steps
// come back as a type and a sentence.
namespace NarrativeEngine::PlotAdapt
{
    enum class Decision : std::uint8_t
    {
        Revise,
        Concede
    };

    struct Outcome
    {
        bool ok = false;
        std::string rejection;

        Decision decision = Decision::Concede;
        // Only when Concede. One sentence, already sanitized.
        std::string reason;
        // Only when Revise. The replacement TAIL -- everything from the
        // cursor onward.
        std::vector<PlotModel::Step> plan;
    };

    struct Limits
    {
        // The replacement tail, whole. One step is a legitimate
        // revision: "forget the groundwork, just do it".
        //
        // The ceiling tracks birth's. A revision that cannot be as long
        // as the plan it replaces is a strange rule, and neither bound
        // is stated to the model here either.
        std::size_t minSteps = 1;
        std::size_t maxSteps = 10;
        std::size_t maxReason = 300;
        std::size_t maxDescription = 200;
    };

    // Pure. Response text in, decision-or-rejection out.
    [[nodiscard]] Outcome Parse(const std::string& response, const Limits& limits);

    // --- The engine-bound half -------------------------------------------

    // Ask the model what happens next. BLOCKS on the plot worker, for
    // the same reason birth does.
    //
    // Returns the outcome; a failed call or a rejected response comes
    // back as a Concede, because the alternative is a plot that stalls
    // with a failed step and no way forward. Conceding on a bad response
    // is a worse story than a good revision and a better one than a plot
    // that never resolves.
    [[nodiscard]] Outcome Compose(const PlotThread::Token& pt,
                                  const PlotModel::Plot& plot,
                                  const PlotModel::Step& failed,
                                  int attempt,
                                  int maxAttempts);
} // namespace NarrativeEngine::PlotAdapt
