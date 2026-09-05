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
    // One faction membership, with the NPC's standing inside it.
    //
    // `standing` is NORMALISED to 0..1 by PlotFactionRoster, so factions
    // are comparable: the head of a two-rung guild and the head of a
    // seven-rung college both come out at 1.0.
    //
    // A faction the roster does not list still appears here — membership
    // is what makes someone "in an organisation" for weighting and for
    // the agent ladder — but with `rostered = false` and a standing of
    // 0. Its members are peers, and none is a subordinate of another.
    // That falls out of the arithmetic rather than needing a rule: the
    // subordinate test is "lower standing than the mastermind", and
    // 0 < 0 is false.
    struct FactionStanding
    {
        RE::FormID faction = 0;
        double standing = 0.0;
        bool rostered = false;
    };

    // A distance-blind tie to another participant, from GossipGraph's
    // PersonalEdge. `sharedFaction` distinguishes a colleague from a
    // relative, which is what separates the first two rungs of the
    // agent ladder.
    struct Tie
    {
        RE::FormID other = 0;
        bool sharedFaction = false;
        // They share a roof -- GossipGraph's Household channel. In
        // practice this is family: the Gray-Manes, the Battle-Borns,
        // the Companions in Jorrvaskr.
        //
        // Kept separate from `sharedFaction` because the menu was
        // flattening it away, and the result was Eorlund Gray-Mane
        // scheming against Olfina Gray-Mane with the prompt describing
        // his daughter as "someone they know, in the same organisation"
        // -- word for word what it said about Aela the Huntress.
        // GossipGraph's PersonalEdge::tierDelta, carried through rather
        // than discarded: +1 for a hostile relationship (Rival, Foe,
        // Enemy, Archnemesis), -1 for a close one (Lover, Ally,
        // Confidant), 0 otherwise.
        //
        // Dropping it meant plot casting could not tell an ally from an
        // archnemesis -- both were simply `tied` -- and rung 3, which
        // wants ties that are NOT collegial, routed hostile pairs
        // preferentially, because enemies rarely share an admitted
        // faction. Vanilla ships `ElenwenUlfric` at rank Foe, and the
        // ladder duly cast Ulfric Stormcloak to lift a note off a desk
        // in the Blue Palace for the Thalmor ambassador.
        //
        // It is also the only signal here for how CLOSE a pair are. A
        // `household` flag briefly sat beside it, reading
        // `edge.via == Channel::Household`, and it was never once true:
        // RebuildEdges emits only Channel::Faction and
        // Channel::Relationship, and Household is a gossip TRANSMISSION
        // tier rather than an edge channel. Anything wanting to know
        // that two people are family should read a negative tierDelta,
        // which is what vanilla's sibling and ally relationships
        // actually produce.
        int tierDelta = 0;
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

    // "This member is not on the road graph." Their settlement has no
    // map marker, or it is in a worldspace the graph does not cover, or
    // the graph is switched off entirely.
    inline constexpr std::size_t kNoRoadNode = static_cast<std::size_t>(-1);

    struct Member
    {
        RE::FormID npc = 0;
        std::string name;
        RE::FormID hold = 0;
        // The settlement they live in, which is finer than the hold and
        // is what a plot's locations are drawn from -- the places that
        // matter to a scheme are where its people are.
        RE::FormID settlement = 0;
        // Where this member is, coarsely, for measuring travel: the road
        // node nearest their settlement's map marker. Resolved once on
        // the main thread at population build, because it needs the
        // marker reference and the graph, and neither is a plot-thread
        // read.
        std::size_t roadNode = kNoRoadNode;
        SkillProfile skills;
        // Every faction membership that counts, rostered or not. An
        // independent NPC has none, and stays eligible on that basis
        // rather than being excluded by it.
        std::vector<FactionStanding> factions;
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
        NotViable,  // alive, but not in a state to start scheming right now
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
        // Mastermind selection only: the weighted draw's whole
        // shortlist, in draw order, `chosen` first.
        //
        // The plugin no longer picks the mastermind. It picks who is
        // ELIGIBLE and how likely each of them is to be scheming, and
        // an LLM chooses among them -- so what comes out of here is a
        // handful of plausible people rather than one.
        std::vector<RE::FormID> shortlist;
    };

    // True when the NPC is alive and enabled. Supplied by the caller so
    // this module stays engine-free; at runtime it is a plain per-actor
    // load, safe off the main thread under the codebase's documented
    // precedent, and in a probe it is whatever the probe says.
    using AlivePredicate = std::function<bool(RE::FormID)>;

    // Does this NPC's membership of this faction currently COUNT?
    //
    // Same shape and the same reason as AlivePredicate: the answer is a
    // live engine fact, and this module stays engine-free by being
    // handed it. A standing that does not count is treated as though the
    // membership were absent -- no rung, and no credit for belonging to
    // an organisation -- because a jarl driven out of his keep is not a
    // junior member of his own court, he is out of it.
    //
    // An EMPTY predicate means everything counts, which is what a probe
    // driving the pure arithmetic wants and what the roster produces
    // when no section declares a tenure gate.
    using SeatedPredicate = std::function<bool(RE::FormID npc, RE::FormID faction)>;

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

    // Drop rows that no longer say anything, returning how many went.
    //
    // A row is created the first time an NPC is engaged and is only ever
    // mutated afterwards, so without this the table grows for the life of
    // a save toward the size of the participating population -- and it is
    // persisted, so that growth is in every co-save.
    //
    // Safe because "inert row" and "no row" are the same answer: an
    // unengaged row whose cooldowns have both passed screens exactly as a
    // missing one does, for either role. Anything still engaged, or still
    // holding a stamp in the future for EITHER role, stays -- a row can be
    // spent as a mastermind and still benched as an actor.
    std::size_t PruneExpired(OccupancyTable& occupancy, double gameDay);

    // Pick a mastermind, weighted by the resources they command.
    //
    // Weight rises with faction rank inside an admitted faction.
    // Independents stay eligible at a low but non-zero weight: what
    // independence costs them shows up in casting, not in eligibility.
    //
    // `rng` is the caller's stream position, advanced in place, so a
    // seeded run reproduces exactly.
    // Draw up to `count` DISTINCT candidates by weight, without
    // replacement, strongest-weighted first by expectation.
    //
    // `viable` is a second liveness gate applied only here: a candidate
    // can be alive and off cooldown and still be a bad person to start
    // a scheme around this second -- mid-fight, following the player,
    // in engine limbo. It is the filter the visit beat applies for the
    // same reason, and it is separate from `alive` so a rejection says
    // which of the two it was.
    //
    // An empty `viable` means everything alive is viable, which is what
    // a probe driving the pure arithmetic wants.
    [[nodiscard]] Result SelectMastermindShortlist(const Population& population,
                                                   const OccupancyTable& occupancy,
                                                   double gameDay,
                                                   const AlivePredicate& alive,
                                                   const AlivePredicate& viable,
                                                   const SeatedPredicate& seated,
                                                   std::size_t count,
                                                   std::uint64_t& rng);

    [[nodiscard]] Result SelectMastermind(const Population& population,
                                          const OccupancyTable& occupancy,
                                          double gameDay,
                                          const AlivePredicate& alive,
                                          const SeatedPredicate& seated,
                                          std::uint64_t& rng);

    // Pick an actor for one of `mastermind`'s steps, down the ladder:
    //
    //   rung 1  a subordinate with a personal tie to the mastermind
    //   rung 2  any subordinate (lower rank in a shared admitted faction)
    //   rung 3  a personal tie without a shared faction, who is
    //           neither HOSTILE to the mastermind nor OUTRANKS them
    //   rung 4  the mastermind themselves
    //
    // `stepText` is the step's own description, and anyone it NAMES is
    // excluded from rungs 1 to 3. See NamesPerson.
    //
    // The last rung is not a failure case — it is how independents
    // operate by default, and it is why a Riverwood farmer's schemes
    // look different from Maven's without any rule saying so. It is also
    // a live option at rung 3 rather than only a fallback beneath it;
    // see kSelfWeightShare.
    //
    // ---------------------------------------------------------------------
    // WHY RUNG 3 IS GUARDED, AND WHY RUNG 4 COMPETES WITH IT
    //
    // The subordinate test is a STRICT inequality against the
    // mastermind's own standing, so two large groups can never reach
    // rungs 1 or 2 at all: anyone at the bottom of a rostered ladder
    // (nothing is below zero) and anyone in no rostered faction (their
    // standings map is empty, so the lookup never hits). Both fall
    // straight through to rung 3 every single time.
    //
    // Rung 3 then had no rank check whatsoever and drew uniformly. It
    // also selects deliberately for ties that are NOT collegial, which
    // in practice means the household channel — and in Skyrim your
    // housemates in a palace are the court. The observed result was
    // Elisif the Fair running surveillance for Gisli, a Blue Palace
    // cook, because they share a roof: a uniform draw among three
    // acquaintances, one of whom was the Jarl.
    //
    // Three changes, and they are complementary. The guard removes
    // people who plainly outrank the mastermind, or who sit at the top
    // of any ladder at all. The hostility check removes the ones who
    // would sooner see the scheme fail: rung 3 wants ties that are not
    // collegial, and enemies rarely share an admitted faction, so
    // without it hostile pairs were routed there preferentially. Rung 4
    // then takes half of what remains, because by the time a scheme is
    // down to distant acquaintances, doing it yourself is genuinely the
    // competitive option — and a scullion with nobody to command running
    // her own errand is the CORRECT story, not a degraded one.
    // May this specific person act for this mastermind on this step?
    //
    // The eligibility half of the ladder, pulled out so that a caller
    // with its OWN preference order -- a retrieval index ranking people
    // by how well they match the step's description of who it needs --
    // can apply the same rules without also inheriting the ladder's
    // ordering. SelectActor is written in terms of this, so the two
    // cannot drift into disagreeing about who is allowed.
    //
    // `rung` is the BEST rung the candidate reaches: 1 for a tied
    // subordinate, 2 for any subordinate, 3 for a non-collegial,
    // non-hostile tie that does not outrank the mastermind. 0 means they
    // reach no rung at all and are not an agent for this step under any
    // circumstances.
    //
    // `reject` is separate on purpose. Reaching a rung is about the
    // relationship, which does not change minute to minute; being
    // screened out is about right now -- occupied, on cooldown, dead.
    // A caller that wants to log WHY somebody was passed over needs to
    // tell those apart.
    struct Qualification
    {
        int rung = 0;
        Reject reject = Reject::None;

        // Both halves must hold. A tied subordinate who is already busy
        // is not an agent today.
        [[nodiscard]] bool Eligible() const noexcept
        {
            return rung != 0 && reject == Reject::None;
        }
    };

    // How well one candidate fits the step's description of who it
    // needs. Higher is better; 0 means no shared vocabulary at all.
    //
    // Comparable only WITHIN one selection, which is all this is used
    // for: the same query scored against different people. There is no
    // threshold and there deliberately is not one -- see SelectActor.
    using AgentScorer = std::function<float(RE::FormID)>;

    // `subject` is the person the step is done TO, once resolved, or 0.
    // Excluded outright: a step is never carried out by the person it is
    // aimed at. The prose test on `stepText` covers the same ground but
    // only when the sentence spells the name the way it expects, and it
    // does not always -- a possessive defeated it for a long time, and
    // Captain Veleth was cast to watch Captain Veleth.
    [[nodiscard]] Qualification QualifyAgent(const Population& population,
                                             RE::FormID mastermind,
                                             RE::FormID candidate,
                                             std::string_view stepText,
                                             RE::FormID subject,
                                             const OccupancyTable& occupancy,
                                             double gameDay,
                                             const AlivePredicate& alive,
                                             const SeatedPredicate& seated);

    // WHY `score` NARROWS RATHER THAN ORDERS
    //
    // The step says what kind of person it needs, and it is tempting to
    // search the whole population for the best match and then check
    // whether that person is allowed to act. That was tried and it does
    // not work: across a measured run, 47% of steps found candidates
    // matching the description well -- one at a perfect 1.00, another
    // with 24 acceptable matches -- and the ladder refused every single
    // one, because a stranger who happens to be the right KIND of person
    // is still a stranger. 40% of steps ended with the mastermind doing
    // the work themselves, which is the failure this whole mechanism
    // exists to avoid.
    //
    // So eligibility comes first and the description only ever chooses
    // among people who were already going to be asked. That also means
    // there is no minimum match quality here: the pool is a handful of
    // subordinates and acquaintances rather than a thousand strangers,
    // the best fit among them is usually a poor one in absolute terms,
    // and refusing it would just put the work back on the mastermind.
    // A weak match to the right kind of person still beats a coin toss.
    //
    // With no scorer the rung is drawn from at random, weighted, which
    // is what happens when a step carries no description of its agent.
    [[nodiscard]] Result SelectActor(const Population& population,
                                     RE::FormID mastermind,
                                     std::string_view stepText,
                                     const OccupancyTable& occupancy,
                                     double gameDay,
                                     const AlivePredicate& alive,
                                     RE::FormID subject,
                                     const SeatedPredicate& seated,
                                     std::uint64_t& rng,
                                     const AgentScorer& score = {});
} // namespace NarrativeEngine::PlotCasting
