#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <PlotState.h>

#include <RE/Skyrim.h>

// PlotCasting — who masterminds a plot, and who carries out its steps.
//
// Every function here takes the population, the occupancy table, the
// clock and a liveness predicate as PARAMETERS. None of them reaches for
// GossipGraph, Settings or the engine. That is what lets a probe hand
// them a fabricated population and assert the rules directly — an
// occupied NPC is never returned, a cooling-down one is returned again
// one tick after their stamp expires, the ladder falls through in order
// — none of which is observable from inside a running game without
// waiting weeks of in-world time and reading a log.
//
// The population itself is built from GossipGraph by PlotPopulation,
// which is the engine-bound half.
//
// See docs/design/FACTION_PLOTS.md Part 5 and
// docs/implementation/PHASE_14_FACTION_PLOTS.md step 5.
namespace NarrativeEngine::PlotCasting
{
    struct FactionRank
    {
        RE::FormID faction = 0;
        // The NPC's rank in that faction. Vanilla ranks start at 0 and
        // rise; a negative value means "member, rank unspecified", which
        // a great many vanilla records use.
        int rank = 0;
    };

    // A distance-blind tie to another participant, from GossipGraph's
    // PersonalEdge. `sharedFaction` distinguishes a colleague from a
    // relative, which is what separates the first two rungs of the
    // agent ladder.
    struct Tie
    {
        RE::FormID other = 0;
        bool sharedFaction = false;
    };

    // What an NPC brings to a step, cached at population-build time.
    //
    // Read once from the TESNPC rather than per tick: these are authored
    // values that do not change, and reading them on the plot worker
    // would be an engine read from the wrong place as well as a waste.
    // All normalised to 0..1 so the resolution arithmetic never has to
    // know Skyrim's scales.
    struct SkillProfile
    {
        double competence = 0.5; // level
        double speech = 0.5;
        double sneak = 0.5;
        double pickpocket = 0.5;
    };

    struct Member
    {
        RE::FormID npc = 0;
        std::string name;
        RE::FormID hold = 0;
        SkillProfile skills;
        // Memberships of factions the prominence filter admitted. An
        // independent NPC has none, and stays eligible on that basis
        // rather than being excluded by it.
        std::vector<FactionRank> factions;
        std::vector<Tie> ties;
    };

    struct Population
    {
        std::vector<Member> members;

        [[nodiscard]] const Member* Find(RE::FormID npc) const;
    };

    // Why a candidate was passed over. Recorded for every rejected
    // candidate, because casting is where a simulation like this
    // silently degrades into "the same six NPCs do everything" and the
    // rejects are the only place that is visible.
    enum class Reject : std::uint8_t
    {
        None,
        Occupied,   // already masterminding or acting
        OnCooldown, // free, but not yet available again for this role
        Dead,       // dead or disabled
        NoTie,      // no relationship to the mastermind (agent selection only)
        IsMastermind
    };

    [[nodiscard]] std::string_view RejectId(Reject r) noexcept;

    struct Candidate
    {
        RE::FormID npc = 0;
        double weight = 0.0;
        Reject reject = Reject::None;
        // Which rung of the agent ladder this candidate was found on.
        // 0 for mastermind selection. Diagnostic; it is how "the Thieves
        // Guild has options, a Riverwood farmer has themselves" becomes
        // checkable rather than asserted.
        int rung = 0;
    };

    struct Result
    {
        RE::FormID chosen = 0;
        int chosenRung = 0;
        // Everyone weighed, chosen and rejected alike.
        std::vector<Candidate> considered;
    };

    // True when the NPC is alive and enabled. Supplied by the caller so
    // this module stays engine-free; at runtime it is a plain per-actor
    // load, safe off the main thread under the codebase's documented
    // precedent, and in a probe it is whatever the probe says.
    using AlivePredicate = std::function<bool(RE::FormID)>;

    using OccupancyTable = std::unordered_map<RE::FormID, PlotModel::Occupancy>;

    struct Cooldowns
    {
        double mastermindDays = 5.0;
        double actorDays = 1.5;
    };

    // Whether `npc` may take `role` right now: not engaged in anything,
    // and past their cooldown stamp for that role. Occupancy and
    // cooldown are ONE lookup on purpose — two structures could
    // disagree, and this one cannot.
    [[nodiscard]] bool IsAvailable(const OccupancyTable& occupancy,
                                   RE::FormID npc,
                                   PlotModel::Role role,
                                   double gameDay);

    // Mark `npc` engaged in `plotId` as `role`.
    void Engage(OccupancyTable& occupancy, RE::FormID npc, PlotModel::Role role, std::uint32_t plotId);

    // Release `npc` and start their cooldown for the role they held.
    void Release(OccupancyTable& occupancy, RE::FormID npc, double gameDay, const Cooldowns& cooldowns);

    // Pick a mastermind, weighted by the resources they command.
    //
    // Weight rises with faction rank inside an admitted faction.
    // Independents stay eligible at a low but non-zero weight: what
    // independence costs them shows up in casting, not in eligibility.
    //
    // `rng` is the caller's stream position, advanced in place, so a
    // seeded run reproduces exactly.
    [[nodiscard]] Result SelectMastermind(const Population& population,
                                          const OccupancyTable& occupancy,
                                          double gameDay,
                                          const AlivePredicate& alive,
                                          std::uint64_t& rng);

    // Pick an actor for one of `mastermind`'s steps, down the ladder:
    //
    //   rung 1  a subordinate with a personal tie to the mastermind
    //   rung 2  any subordinate (lower rank in a shared admitted faction)
    //   rung 3  a personal tie without a shared faction
    //   rung 4  the mastermind themselves
    //
    // The last rung is not a failure case — it is how independents
    // operate by default, and it is why a Riverwood farmer's schemes
    // look different from Maven's without any rule saying so.
    [[nodiscard]] Result SelectActor(const Population& population,
                                     RE::FormID mastermind,
                                     const OccupancyTable& occupancy,
                                     double gameDay,
                                     const AlivePredicate& alive,
                                     std::uint64_t& rng);
} // namespace NarrativeEngine::PlotCasting
