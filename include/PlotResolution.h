#pragma once

#include <cstdint>

#include <PlotModel.h>

// PlotResolution — the progress race, as free functions over plain
// numbers.
//
// A step is not a coin flipped at a deadline. It is a race between
// accumulating progress and a running clock, and both sides of that race
// are fixed when the step is dispatched:
//
//   BUDGET    how many ticks the actor gets. From CIRCUMSTANCE: how far
//             they must travel, plus the step type's inherent scale.
//             Deliberately NOT a competence figure — a capable actor and
//             a hopeless one sent on the same errand to the same place
//             get the same budget; what differs is what they do with it.
//
//   THRESHOLD how much work the step represents. From the step type and
//             the target's importance.
//
// The two are computed by separate functions taking disjoint inputs, and
// that is load-bearing rather than tidy. If the budget were derived from
// the threshold, every step would carry identical odds and the race
// would be theatre. `SizeBudget` cannot see `targetImportance` and
// `SizeThreshold` cannot see `travelDistance`, so the independence is a
// property of the signatures rather than a promise in a comment.
//
// Nothing here reads the engine, Settings or plot state. The caller
// supplies distances, competences and the tuning values; a probe
// supplies whatever it likes.
//
// See docs/design/FACTION_PLOTS.md Part 6 and
// docs/implementation/PHASE_14_FACTION_PLOTS.md step 6.
namespace NarrativeEngine::PlotResolution
{
    // How many ticks a step of this type gets, given how far its actor
    // must travel.
    //
    // `travelDistanceNorm` is 0 (the actor is already there) to 1 (the
    // far side of the province). The caller normalises; this function
    // has no opinion about map units.
    //
    // `maxFractionPerTick` is the per-tick progress ceiling, and it is
    // here because it sets a FLOOR under any winnable budget: no tick may
    // add more than that share of the threshold, so a step needs at least
    // ceil(1 / maxFractionPerTick) ticks before success is even
    // arithmetically possible. A budget below that does not describe a
    // hard step, it describes an impossible one - and impossible steps
    // fail silently and look exactly like bad luck.
    //
    // Note this is a floor only. A step whose threshold is large relative
    // to the actor's rate takes far longer than the minimum, which is
    // where target importance now actually bites.
    //
    // This does NOT compromise the budget/threshold independence the
    // design turns on. The ceiling is a tuning constant, not a property
    // of the target: SizeBudget still cannot see targetImportance, and
    // SizeThreshold still cannot see travel.
    [[nodiscard]] int SizeBudget(PlotModel::StepType type, double travelDistanceNorm, double maxFractionPerTick);

    // The fewest ticks in which a threshold can be reached at all, given
    // the per-tick ceiling. Exposed so a caller or a probe can assert a
    // budget is winnable rather than rediscovering the arithmetic.
    [[nodiscard]] int MinimumViableBudget(double maxFractionPerTick);

    // How much progress a step of this type requires against a target of
    // this importance.
    //
    // `targetImportance` is 0 (a nobody, a trinket) to 1 (a jarl, an
    // artefact).
    [[nodiscard]] float SizeThreshold(PlotModel::StepType type, double targetImportance);

    // How well an actor suits a KIND of step.
    //
    // A thief sent to steal, a courtier sent to discredit. Derived from
    // the authored skills the population already caches, so it costs
    // nothing per tick and is stable for a given NPC.
    //
    // The mapping is deliberately coarse — three skills across eight
    // step types — because the point is that actors DIFFER, not that the
    // model is a faithful simulation of Skyrim's skill tree. Step 7's
    // harness is where the spread this produces gets judged.
    [[nodiscard]] double Suitability(PlotModel::StepType type, double speech, double sneak, double pickpocket);

    // What one tick of an actor's effort is worth.
    struct RollInputs
    {
        // 0..1. Level and relevant skills, normalised by the caller.
        double competence = 0.5;
        // 0..1. How well this actor suits this KIND of step — a thief
        // sent to steal, a courtier sent to discredit.
        double suitability = 0.5;

        // The step's own threshold, in the same absolute work units the
        // rates below are in.
        float threshold = 1.0f;

        // ABSOLUTE work per tick, not a fraction of the threshold.
        //
        // This distinction is the whole of the model and it was wrong in
        // the first implementation. When the per-tick roll is a fraction
        // OF the threshold, ticks-to-finish is 1 / mean(fraction) and the
        // threshold cancels out completely — a step worth 5 and a step
        // worth 500 both take 4.5 ticks, and `SizeThreshold`, target
        // importance and half the design do nothing at all. Measured, not
        // reasoned: the Step 9 harness found it.
        //
        // With absolute rates, ticks ≈ threshold / rate, so a harder
        // target genuinely takes longer.
        float rateMin = 1.5f;
        float rateMax = 5.0f;

        // The one thing that DOES stay relative: no single tick may clear
        // more than this share of the threshold, so a trivially small
        // step still cannot be finished in one go. This is what
        // MinimumViableBudget is derived from.
        float maxFractionPerTick = 0.45f;
    };

    // Progress for one unblocked tick. Always strictly positive and
    // always short of the threshold — see the clamps in RollInputs.
    [[nodiscard]] float RollProgress(const RollInputs& inputs, std::uint64_t& rng);

    struct MishapInputs
    {
        bool enabled = true;
        double chanceBase = 0.04;
        // Conspicuous steps are likelier to go wrong in a way somebody
        // notices; capable actors are less likely to be the ones it
        // happens to.
        bool conspicuous = false;
        double competence = 0.5;
    };

    // Did the actor get caught this tick?
    //
    // Typed failure is load-bearing — it decides whether the target ends
    // up with a memory and whether the plot leaks into gossip — and
    // progress alone cannot express it, because being caught is
    // orthogonal to how far along the work is. This is the second roll
    // that carries that distinction. Its SHAPE is an open design
    // question, which is why the caller can switch it off.
    [[nodiscard]] bool RollMishap(const MishapInputs& inputs, std::uint64_t& rng);

    // One tick's worth of everything that can happen to a live step.
    struct TickInputs
    {
        // A conspicuous step whose actor or target is loaded near the
        // player makes no progress this tick. `elapsed` advances anyway,
        // which is the whole mechanism: a step blocked long enough fails
        // by running out of time like any other, with no separate
        // "overtaken by events" path and no risk of a held step living
        // forever.
        bool blockedByPresence = false;
        RollInputs roll;
        MishapInputs mishap;
    };

    // Advance one live step by a tick, mutating it.
    //
    // Appends to the step's own roll history, and sets `state` and
    // `outcome` when the step reaches a terminal result. Returns what
    // happened, for the trace.
    PlotModel::TickRecord AdvanceStep(PlotModel::Step& step, const TickInputs& inputs, std::uint64_t& rng);
} // namespace NarrativeEngine::PlotResolution
