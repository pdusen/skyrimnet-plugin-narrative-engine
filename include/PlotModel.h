#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>

// PlotModel — the plot and step object model, plus the manifest that
// gives both their vocabulary.
//
// Pure data and pure functions. Nothing here reads the engine, holds a
// token, or knows a thread exists: it is the shape the simulation
// operates on, so that the arithmetic in later steps can be probed
// without a game. Every display string is derived here from cached
// names, which is what lets the log and the dashboard render a plot
// from a worker thread.
//
// See docs/design/FACTION_PLOTS.md Part 1 for the model and
// docs/implementation/PHASE_14_FACTION_PLOTS.md step 2 for this step.
namespace NarrativeEngine::PlotModel
{
    // THE MANIFEST.
    //
    // Fixed, small, and closed. Closed because every type needs a
    // mechanical resolution, and because this list is handed to the LLM
    // verbatim as the vocabulary it must plan in — a plan is a sequence
    // of these against pre-resolved targets, so validating one is a
    // membership test rather than a name-resolution problem.
    //
    // It is also the OBJECTIVE vocabulary. The last step of a plan is
    // the one that accomplishes the objective, so an objective is just a
    // step type at terminal difficulty against a high-value target.
    // Adding a type here widens what plots can be about, with no
    // separate objective taxonomy to keep in sync.
    enum class StepType : std::uint8_t
    {
        Locate,    // find where a thing or person is
        Acquire,   // obtain an object
        Deliver,   // move an object or message to someone
        Surveil,   // watch a person or place, report back
        Suborn,    // buy, recruit, or blackmail someone into cooperation
        Discredit, // damage a rival's standing or leverage
        Sabotage,  // impair a thing, a shipment, an arrangement
        Conceal,   // cover the tracks of a step already taken

        Count
    };

    inline constexpr std::size_t kStepTypeCount = static_cast<std::size_t>(StepType::Count);

    // Whether a step would unavoidably be noticed if the player were
    // standing there. Gates when the step may make progress — see the
    // design doc's Part 6.
    //
    // `Sometimes` is honest rather than lazy: lifting a ledger from an
    // empty study is not the same act as lifting it off a belt, and
    // whether the type or the individual step should decide is an open
    // design question. It is treated as conspicuous for now; the day
    // that becomes per-step, only this table and one predicate move.
    enum class Conspicuousness : std::uint8_t
    {
        No,
        Sometimes,
        Yes
    };

    struct StepTypeTraits
    {
        // Stable snake_case identifier. What the LLM answers with and
        // what the co-save and the trace record — never the enum's
        // numeric value, which is free to change.
        std::string_view id;
        // The display verb. Composed with a cached target name to make
        // step labels and plot titles; see Label() and Title().
        std::string_view verb;
        // Whether this type can currently be handed to the player. A
        // STAGING decision rather than a property of the type: the
        // `false` entries are the ones whose completion conditions are
        // harder to detect, not the ones that would make bad errands.
        bool playerDeliverable;
        Conspicuousness conspicuous;
    };

    // Ordered exactly as StepType. Indexed by cast, so a reordering of
    // the enum without a matching reordering here is caught by the
    // static_assert below rather than by silently mislabelling steps.
    inline constexpr std::array<StepTypeTraits, kStepTypeCount> kStepTypes{{
        {"locate", "Locate", true, Conspicuousness::No},
        {"acquire", "Acquire", true, Conspicuousness::Sometimes},
        {"deliver", "Deliver to", true, Conspicuousness::No},
        {"surveil", "Watch", true, Conspicuousness::Yes},
        {"suborn", "Suborn", false, Conspicuousness::Yes},
        {"discredit", "Discredit", false, Conspicuousness::Yes},
        {"sabotage", "Sabotage", false, Conspicuousness::Yes},
        {"conceal", "Cover Tracks", false, Conspicuousness::Sometimes},
    }};

    static_assert(kStepTypes.size() == kStepTypeCount, "kStepTypes must carry one row per StepType");

    [[nodiscard]] constexpr const StepTypeTraits& Traits(StepType t) noexcept
    {
        return kStepTypes[static_cast<std::size_t>(t)];
    }

    [[nodiscard]] constexpr std::string_view TypeId(StepType t) noexcept
    {
        return Traits(t).id;
    }

    [[nodiscard]] constexpr bool IsPlayerDeliverable(StepType t) noexcept
    {
        return Traits(t).playerDeliverable;
    }

    // Treated as conspicuous unless the manifest says plainly No. The
    // conservative reading of `Sometimes`: a step that might be seen is
    // held rather than resolved in front of the player, because the
    // failure mode of getting this wrong is a scheme resolving
    // implausibly under the player's nose.
    [[nodiscard]] constexpr bool IsConspicuous(StepType t) noexcept
    {
        return Traits(t).conspicuous != Conspicuousness::No;
    }

    // Parse a manifest id back to its type. Returns false for anything
    // not on the list — which is the whole point, and is what makes
    // validating an LLM's answer a membership test.
    [[nodiscard]] bool ParseStepType(std::string_view id, StepType& out) noexcept;

    // Where a step sits in its life.
    enum class StepState : std::uint8_t
    {
        Planned,        // in the plan, not yet dispatched
        AwaitingPlayer, // reserved for Phase D; never set in this phase
        InProgress,     // dispatched, accruing progress
        Succeeded,
        Failed
    };

    // How a step ended. Typed rather than boolean because "ran out of
    // time" and "was caught in the act" feed adaptation differently and
    // produce very different memories — and because only the second
    // decides that the target learns anything.
    enum class StepOutcome : std::uint8_t
    {
        None, // still running, or never dispatched
        Succeeded,
        FailedTimeout, // budget exhausted; how far they got is in `progress`
        FailedCaught   // a mishap ended it early
    };

    [[nodiscard]] std::string_view StepStateId(StepState s) noexcept;
    [[nodiscard]] std::string_view StepOutcomeId(StepOutcome o) noexcept;

    // One unit of work.
    //
    // Names are cached at dispatch rather than resolved on demand: the
    // trace and the dashboard both render steps from the plot worker,
    // where touching a TESForm is not available. This is the
    // GossipGraph::Participant precedent.
    struct Step
    {
        StepType type = StepType::Locate;

        // Pre-resolved. Never a free string from an LLM — the model
        // picks from a menu by index and this is what that index
        // resolved to. May be 0 for a type that needs no target.
        RE::FormID target = 0;
        std::string targetName;

        // Who is doing it. 0 until the step is dispatched and cast.
        RE::FormID actor = 0;
        std::string actorName;

        StepState state = StepState::Planned;
        StepOutcome outcome = StepOutcome::None;

        // THE RACE. Both sides are fixed at dispatch and derived from
        // independent inputs: the budget from how far the actor must
        // travel plus the type's inherent scale, the threshold from the
        // type and the target's importance. If one were computed from
        // the other every step would carry identical odds.
        int budget = 0;  // ticks allowed before the step times out
        int elapsed = 0; // ticks spent, INCLUDING ones that made no progress
        float threshold = 0.0f;
        float progress = 0.0f;

        // Ticks on which the presence gate held this step. Diagnostic
        // only, but it is the difference between "this actor is hopeless"
        // and "the player stood next to them for a week".
        int heldTicks = 0;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == StepState::Succeeded || state == StepState::Failed;
        }

        // 0..1, clamped. The dashboard's progress ring and the trace both
        // want this and neither should be dividing by a threshold that
        // could be zero.
        [[nodiscard]] float ProgressFraction() const noexcept;
    };

    enum class PlotStatus : std::uint8_t
    {
        Active,
        Succeeded,
        Failed
    };

    [[nodiscard]] std::string_view PlotStatusId(PlotStatus s) noexcept;

    // Why a plot ended. Kept because "why did that one fail" is the
    // question the dashboard exists to answer after the fact.
    enum class PlotOutcome : std::uint8_t
    {
        None,
        ObjectiveAchieved,
        Conceded,         // the mastermind judged it unreachable
        AdaptationCapHit, // it would not concede; iPlotMaxAdaptations did
        MastermindLost    // dead, or no longer resolvable
    };

    [[nodiscard]] std::string_view PlotOutcomeId(PlotOutcome o) noexcept;

    // One scheme, one mastermind.
    struct Plot
    {
        std::uint32_t id = 0;

        RE::FormID mastermind = 0;
        std::string mastermindName;

        // The larger thing the objective serves, in one sentence. Flavour
        // rather than mechanism, but it is what keeps adaptation coherent
        // — and from step 11 onward it is LLM-authored, so it is
        // sanitized at the point of extraction.
        std::string ambition;

        // THE OBJECTIVE, held separately from the plan.
        //
        // The plan's last step accomplishes it, so this duplicates that
        // step's type and target — deliberately. Adaptation rewrites the
        // remaining plan but may never change the destination, and
        // holding the objective outside the rewritable structure is what
        // makes that an invariant rather than a convention. It also keeps
        // the plot's title stable across a re-plan.
        StepType objectiveType = StepType::Acquire;
        RE::FormID objectiveTarget = 0;
        std::string objectiveTargetName;

        // The ladder of steps beneath the objective. Rewritten from
        // `cursor` onward on adaptation; everything before it has already
        // moved to `history`.
        std::vector<Step> plan;
        std::size_t cursor = 0;

        // Resolved steps in resolution order, failures included. The
        // dashboard's chain is history ++ live ++ remaining plan, so a
        // failed step has to stay at the position it occupied rather
        // than being dropped when the plot moves on. Capped by
        // iPlotStepHistoryCap.
        std::vector<Step> history;

        int adaptations = 0;

        PlotStatus status = PlotStatus::Active;
        PlotOutcome outcome = PlotOutcome::None;

        double bornOnGameDay = 0.0;
        double endedOnGameDay = 0.0;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return status != PlotStatus::Active;
        }

        // The live step, or nullptr when the plot is between steps or
        // finished.
        [[nodiscard]] const Step* LiveStep() const noexcept;
        [[nodiscard]] Step* LiveStep() noexcept;
    };

    // ---- Display derivation -------------------------------------------
    //
    // Mechanical, from the manifest's verb plus a cached name. NOT
    // authored: the dashboard ships in step 8, before any LLM call
    // exists in this subsystem, and a chain that cannot label its own
    // nodes until step 11 is not the instrument step 9 needs. It also
    // keeps every rendered string off the sanitizer's critical path,
    // because none of it came from a model.
    //
    // No articles are inserted. "Acquire Amulet of Kings" reads a little
    // stiff, but the alternative is a grammar problem with no correct
    // answer — "Silence the Maven Black-Briar" is worse than stiff, and
    // there is no reliable way to tell a proper name from a common noun
    // in a Skyrim display name.

    // Verb plus target, or the bare verb when there is no target name.
    [[nodiscard]] std::string Label(StepType type, std::string_view targetName);

    [[nodiscard]] inline std::string Label(const Step& step)
    {
        return Label(step.type, step.targetName);
    }

    // The plot's title, as the dashboard card's heading. Same
    // construction at plot scale: "Acquire Amulet of Kings".
    [[nodiscard]] std::string Title(const Plot& plot);
} // namespace NarrativeEngine::PlotModel
