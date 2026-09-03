#include <PlotCasting.h>

#include <algorithm>
#include <cmath>

namespace NarrativeEngine::PlotCasting
{
    namespace
    {
        // splitmix64, matching Plots::NextRandom. Duplicated rather than
        // shared because that one lives in an engine-bound translation
        // unit and this one must stay probeable; it is six lines and a
        // named constant, and the alternative is dragging Settings and
        // the logger into every casting probe.
        std::uint64_t NextRandom(std::uint64_t& state) noexcept
        {
            std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        double NextUniform(std::uint64_t& state) noexcept
        {
            return static_cast<double>(NextRandom(state) >> 11) * (1.0 / 9007199254740992.0);
        }

        // Draw one candidate in proportion to weight. Returns 0 when
        // nothing is eligible.
        RE::FormID WeightedDraw(const std::vector<Candidate>& candidates, std::uint64_t& rng)
        {
            double total = 0.0;
            for (const auto& c : candidates) {
                if (c.reject == Reject::None) {
                    total += c.weight;
                }
            }
            if (!(total > 0.0)) {
                return 0;
            }

            double roll = NextUniform(rng) * total;
            for (const auto& c : candidates) {
                if (c.reject != Reject::None) {
                    continue;
                }
                roll -= c.weight;
                if (roll <= 0.0) {
                    return c.npc;
                }
            }
            // Floating-point drift only; fall back to the last eligible.
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                if (it->reject == Reject::None) {
                    return it->npc;
                }
            }
            return 0;
        }

        // Does this membership count right now?
        //
        // One function so the three places that read standing cannot
        // drift apart: the weighting, the mastermind's own standings,
        // and the subordinate scan. An empty predicate means yes, which
        // keeps every probe that drives the pure arithmetic working
        // unchanged.
        bool Counts(const SeatedPredicate& seated, RE::FormID npc, const FactionStanding& f)
        {
            return !seated || seated(npc, f.faction);
        }

        // How much weight an NPC's standing earns them as a mastermind.
        //
        //   base            everyone, including independents
        //   + membership    belonging to any organisation at all
        //   + standing      how far up a ROSTERED one you are
        //
        // THE STANDING TERM IS EXPONENTIAL, doubling kStandingDoublings
        // times across the ladder. That is a reversal: it was linear,
        // on the argument that a jarl should be likelier than a guard
        // and not a hundred times likelier, or the same few NPCs would
        // scheme continuously with only the cooldown spreading the work.
        //
        // What that argument missed is what a mastermind NEEDS. A plot
        // is a person with something to hide and SOMEBODY TO SEND, and
        // the second half is not evenly distributed: a jarl has a court,
        // a guild master has a guild, a cook has herself. Casting
        // measured that correctly and the log said so -- once the agent
        // ladder stopped handing Elisif to the kitchen, nearly every
        // mastermind was executing their own steps, because nearly every
        // mastermind drawn had nobody to send. The draw was producing
        // schemers who could not scheme.
        //
        // Weighting the people with resources far more heavily is the
        // fix at source. The concentration risk the old comment named is
        // real and unchanged -- watch the CASTING line in the offline
        // harness for distinct-mastermind count, and the mastermind
        // cooldown is what has to absorb it.
        //
        // The standing term takes the MAXIMUM across factions, never the
        // sum: someone's weight should reflect the most authority they
        // hold anywhere, and summing would let a well-connected nobody
        // outrank a guild master. Note the deliberate asymmetry with
        // SelectActor, which takes the UNION across factions — "how much
        // can this person command" and "who can they command" are
        // different questions and must not be collapsed into one rule.
        double MastermindWeight(const Member& member, const SeatedPredicate& seated)
        {
            // Independents are eligible. This is the floor everyone
            // starts from, and it is what makes a Riverwood farmer's
            // plot possible at all.
            constexpr double kBase = 1.0;
            constexpr double kMembershipBonus = 0.5;
            // Doublings of the standing term across the full 0..1
            // ladder. Four gives 2^(4s) - 1, so the term doubles for
            // every quarter of the ladder climbed:
            //
            //   standing  0     0.25  0.5   0.75  1.0
            //   term      0     1     3     7     15
            //   weight    1.5   2.5   4.5   8.5   16.5
            //
            // An independent stays at 1.0, a bottom-rung member at 1.5,
            // and a jarl is eleven times the courtier and sixteen times
            // the farmer.
            constexpr double kStandingDoublings = 4.0;

            double weight = kBase;
            double best = 0.0;
            bool belongs = false;
            for (const auto& f : member.factions) {
                if (!Counts(seated, member.npc, f)) {
                    continue;
                }
                belongs = true;
                best = std::max(best, f.standing);
            }
            if (belongs) {
                weight += kMembershipBonus;
            }
            return weight + std::exp2(kStandingDoublings * std::clamp(best, 0.0, 1.0)) - 1.0;
        }

        // How far above the mastermind a rung-3 candidate may stand
        // before they are simply not someone this person could ask.
        //
        // Standings are normalised 0..1, so a quarter is roughly one
        // rung of a four-or-five rung ladder: a thane may ask a fellow
        // thane a favour, and may not ask the housecarl. Deliberately
        // not zero -- "never ask anyone above you" would forbid a
        // steward leaning on a jarl, which is a thing that happens.
        constexpr double kOutrankMargin = 0.25;

        // What share of the rung-3 draw the mastermind takes for
        // themselves. Half: by the time a scheme is down to distant
        // acquaintances, doing it yourself is the competitive option
        // rather than the last resort.
        constexpr double kSelfWeightShare = 1.0;

        // The highest standing this member holds anywhere that counts.
        //
        // The same quantity MastermindWeight uses, and for the same
        // reason: the MAXIMUM across factions, never the sum, because it
        // answers "how much authority does this person hold anywhere".
        double BestStanding(const Member& member, const SeatedPredicate& seated)
        {
            double best = 0.0;
            for (const auto& f : member.factions) {
                if (Counts(seated, member.npc, f)) {
                    best = std::max(best, f.standing);
                }
            }
            return best;
        }

        // The mastermind's standing in each faction they belong to,
        // for the subordinate test.
        std::unordered_map<RE::FormID, double> StandingsOf(const Member& member, const SeatedPredicate& seated)
        {
            std::unordered_map<RE::FormID, double> standings;
            for (const auto& f : member.factions) {
                if (!Counts(seated, member.npc, f)) {
                    continue;
                }
                auto& slot = standings[f.faction];
                slot = std::max(slot, f.standing);
            }
            return standings;
        }

        bool HasTieTo(const Member& member, RE::FormID other, bool* sharedFaction)
        {
            for (const auto& tie : member.ties) {
                if (tie.other == other) {
                    if (sharedFaction != nullptr) {
                        *sharedFaction = tie.sharedFaction;
                    }
                    return true;
                }
            }
            return false;
        }

        // Rejection reason for a candidate, or None if they may be cast.
        Reject Screen(const OccupancyTable& occupancy,
                      RE::FormID npc,
                      PlotModel::Role role,
                      double gameDay,
                      const AlivePredicate& alive)
        {
            if (alive && !alive(npc)) {
                return Reject::Dead;
            }
            const auto it = occupancy.find(npc);
            if (it == occupancy.end()) {
                return Reject::None;
            }
            if (it->second.IsEngaged()) {
                return Reject::Occupied;
            }
            const double availableAt = role == PlotModel::Role::Mastermind ? it->second.mastermindAvailableAtGameDay
                                                                           : it->second.actorAvailableAtGameDay;
            return gameDay < availableAt ? Reject::OnCooldown : Reject::None;
        }
    } // namespace

    const Member* Population::Find(RE::FormID npc) const
    {
        const auto it = std::find_if(members.begin(), members.end(), [npc](const Member& m) { return m.npc == npc; });
        return it == members.end() ? nullptr : &*it;
    }

    std::string_view RejectId(Reject r) noexcept
    {
        switch (r) {
        case Reject::None:
            return "eligible";
        case Reject::Occupied:
            return "occupied";
        case Reject::OnCooldown:
            return "on_cooldown";
        case Reject::Dead:
            return "dead";
        case Reject::NoTie:
            return "no_tie";
        case Reject::NotViable:
            return "not_viable";
        case Reject::IsMastermind:
            return "is_mastermind";
        }
        return "unknown";
    }

    bool IsAvailable(const OccupancyTable& occupancy, RE::FormID npc, PlotModel::Role role, double gameDay)
    {
        return Screen(occupancy, npc, role, gameDay, nullptr) == Reject::None;
    }

    void Engage(OccupancyTable& occupancy, RE::FormID npc, PlotModel::Role role, std::uint32_t plotId)
    {
        auto& row = occupancy[npc];
        row.plotId = plotId;
        row.role = role;
    }

    std::size_t PruneExpired(OccupancyTable& occupancy, double gameDay)
    {
        const auto before = occupancy.size();
        std::erase_if(occupancy, [gameDay](const auto& entry) {
            const auto& row = entry.second;
            // Mirrors Screen's test deliberately, including the
            // direction of the comparison: Screen rejects while
            // `gameDay < availableAt`, so a stamp is spent once the day
            // has reached it, not after it has passed it.
            return !row.IsEngaged() && !(gameDay < row.mastermindAvailableAtGameDay)
                   && !(gameDay < row.actorAvailableAtGameDay);
        });
        return before - occupancy.size();
    }

    void Release(OccupancyTable& occupancy, RE::FormID npc, double gameDay, const Cooldowns& cooldowns)
    {
        const auto it = occupancy.find(npc);
        if (it == occupancy.end()) {
            return;
        }
        auto& row = it->second;
        // The cooldown that starts is the one for the role they were
        // actually holding. Starting both would bench an NPC from
        // masterminding because they ran an errand, which is precisely
        // the conflation the two separate stamps exist to avoid.
        if (row.role == PlotModel::Role::Mastermind) {
            row.mastermindAvailableAtGameDay = gameDay + cooldowns.mastermindDays;
        } else {
            row.actorAvailableAtGameDay = gameDay + cooldowns.actorDays;
        }
        row.plotId = 0;
    }

    Result SelectMastermind(const Population& population,
                            const OccupancyTable& occupancy,
                            double gameDay,
                            const AlivePredicate& alive,
                            const SeatedPredicate& seated,
                            std::uint64_t& rng)
    {
        Result result;
        result.considered.reserve(population.members.size());

        for (const auto& member : population.members) {
            Candidate candidate;
            candidate.npc = member.npc;
            candidate.reject = Screen(occupancy, member.npc, PlotModel::Role::Mastermind, gameDay, alive);
            candidate.weight = candidate.reject == Reject::None ? MastermindWeight(member, seated) : 0.0;
            result.considered.push_back(candidate);
        }

        result.chosen = WeightedDraw(result.considered, rng);
        return result;
    }

    Result SelectMastermindShortlist(const Population& population,
                                     const OccupancyTable& occupancy,
                                     double gameDay,
                                     const AlivePredicate& alive,
                                     const AlivePredicate& viable,
                                     const SeatedPredicate& seated,
                                     std::size_t count,
                                     std::uint64_t& rng)
    {
        Result result;
        result.considered.reserve(population.members.size());

        for (const auto& member : population.members) {
            Candidate candidate;
            candidate.npc = member.npc;
            candidate.reject = Screen(occupancy, member.npc, PlotModel::Role::Mastermind, gameDay, alive);
            // Viability is checked AFTER the cheap screens and only for
            // survivors, because it is the one that touches an Actor.
            if (candidate.reject == Reject::None && viable && !viable(member.npc)) {
                candidate.reject = Reject::NotViable;
            }
            candidate.weight = candidate.reject == Reject::None ? MastermindWeight(member, seated) : 0.0;
            result.considered.push_back(candidate);
        }

        // Without replacement: a drawn candidate is struck out by
        // marking it rejected, so the next draw renormalises over what
        // is left. Drawing with replacement would hand the same person
        // to the model twice and quietly shrink the shortlist.
        //
        // The strike-outs are undone afterwards. `considered` is the
        // diagnostic record of who was ELIGIBLE, and a candidate that
        // was eligible enough to be drawn must not read as rejected in
        // the log.
        std::vector<std::size_t> drawnAt;
        for (std::size_t i = 0; i < count; ++i) {
            const auto npc = WeightedDraw(result.considered, rng);
            if (npc == 0) {
                break;
            }
            result.shortlist.push_back(npc);
            for (std::size_t j = 0; j < result.considered.size(); ++j) {
                if (result.considered[j].npc == npc) {
                    result.considered[j].reject = Reject::Occupied;
                    drawnAt.push_back(j);
                    break;
                }
            }
        }
        for (const auto j : drawnAt) {
            result.considered[j].reject = Reject::None;
        }

        if (!result.shortlist.empty()) {
            result.chosen = result.shortlist.front();
        }
        return result;
    }

    Result SelectActor(const Population& population,
                       RE::FormID mastermind,
                       const OccupancyTable& occupancy,
                       double gameDay,
                       const AlivePredicate& alive,
                       const SeatedPredicate& seated,
                       std::uint64_t& rng)
    {
        Result result;

        const Member* boss = population.Find(mastermind);
        if (boss == nullptr) {
            return result;
        }
        // Every faction the mastermind belongs to, rostered or not. A
        // mastermind in several may draw subordinates from ANY of them:
        // the ladder pools candidates across all rather than picking one
        // faction first.
        const auto bossStandings = StandingsOf(*boss, seated);
        const double bossBest = BestStanding(*boss, seated);

        // Walk the ladder rung by rung and stop at the first rung with
        // anyone on it. Drawing across all rungs at once would let a
        // large pool of distant acquaintances drown out the one
        // subordinate who is the obvious choice.
        for (int rung = 1; rung <= 3 && result.chosen == 0; ++rung) {
            std::vector<Candidate> rungCandidates;

            for (const auto& member : population.members) {
                if (member.npc == mastermind) {
                    continue;
                }

                bool sharedFaction = false;
                const bool tied =
                    HasTieTo(*boss, member.npc, &sharedFaction) || HasTieTo(member, mastermind, &sharedFaction);

                // A subordinate is someone whose standing is LOWER
                // than the mastermind's in a faction they share. Equal
                // standing is not subordinate: a fellow Circle member is
                // a colleague, and ordering them about is a different
                // social act.
                //
                // In a faction the roster does not list, every standing
                // is 0, so this is false for everyone — its members are
                // peers, exactly as intended, with no special case.
                bool subordinate = false;
                for (const auto& f : member.factions) {
                    if (!Counts(seated, member.npc, f)) {
                        continue;
                    }
                    const auto it = bossStandings.find(f.faction);
                    if (it != bossStandings.end() && f.standing < it->second) {
                        subordinate = true;
                        break;
                    }
                }

                // Rungs 1 and 2 already require a subordinate, which is
                // a stronger statement than this. Rung 3 required
                // nothing at all, which is how a Blue Palace cook came
                // to have the Jarl of Solitude in her draw: they share a
                // roof, that is a non-collegial tie, and nothing asked
                // whether Elisif was a person Gisli could ask.
                //
                // Skipped rather than screened, matching how the ladder
                // already treats anyone not on the rung: `considered`
                // records who was WEIGHED, and someone three ranks above
                // the mastermind was never a candidate to weigh.
                const bool outranks = BestStanding(member, seated) > bossBest + kOutrankMargin;

                const bool onThisRung = (rung == 1 && subordinate && tied) || (rung == 2 && subordinate)
                                        || (rung == 3 && tied && !sharedFaction && !outranks);
                if (!onThisRung) {
                    continue;
                }

                Candidate candidate;
                candidate.npc = member.npc;
                candidate.rung = rung;
                candidate.reject = Screen(occupancy, member.npc, PlotModel::Role::Actor, gameDay, alive);
                candidate.weight = candidate.reject == Reject::None ? 1.0 : 0.0;
                rungCandidates.push_back(candidate);
            }

            // At rung 3 the mastermind enters the draw rather than
            // waiting below it. Their weight is the whole rest of the
            // rung, so it is a coin flip between "ask one of these
            // acquaintances" and "do it yourself" however many
            // acquaintances there are -- an NPC with twelve neighbours
            // should not thereby become twelve times less likely to act
            // for themselves.
            //
            // Not screened for occupancy, for the reason rung 4 gives
            // below: they are already engaged in this very plot as its
            // mastermind, and that must not read as unavailable.
            if (rung == 3 && (!alive || alive(mastermind))) {
                double othersWeight = 0.0;
                for (const auto& c : rungCandidates) {
                    othersWeight += c.weight;
                }
                Candidate self;
                self.npc = mastermind;
                self.rung = 4;
                // Floored at one so they remain drawable when every
                // acquaintance was screened out, which is the case the
                // fallback beneath used to be the only answer to.
                self.weight = std::max(1.0, kSelfWeightShare * othersWeight);
                rungCandidates.push_back(self);
            }

            const RE::FormID picked = WeightedDraw(rungCandidates, rng);
            result.considered.insert(result.considered.end(), rungCandidates.begin(), rungCandidates.end());
            if (picked != 0) {
                result.chosen = picked;
                // The mastermind is rung 4 wherever they were drawn
                // from. The log line says who acted and why, and "rung
                // 3" against their own name would be a lie about which.
                result.chosenRung = picked == mastermind ? 4 : rung;
            }
        }

        if (result.chosen == 0) {
            // Rung 4: the mastermind does it themselves. Note this
            // ignores their own occupancy — they are already engaged in
            // this very plot as its mastermind, and that must not read
            // as "unavailable".
            if (!alive || alive(mastermind)) {
                result.chosen = mastermind;
                result.chosenRung = 4;
                Candidate self;
                self.npc = mastermind;
                self.rung = 4;
                self.weight = 1.0;
                result.considered.push_back(self);
            }
        }

        return result;
    }
} // namespace NarrativeEngine::PlotCasting
