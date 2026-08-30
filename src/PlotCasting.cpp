#include <PlotCasting.h>

#include <algorithm>

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

        // How much weight an NPC's standing earns them as a mastermind.
        //
        // Rank inside an admitted faction is the signal, because an
        // admitted faction is one large and prominent enough to command
        // something. The curve is deliberately gentle: a jarl should be
        // likelier than a guard, not a hundred times likelier, or the
        // same handful of high-rank NPCs would scheme continuously and
        // the cooldown would be the only thing spreading the work.
        double MastermindWeight(const Member& member)
        {
            // Independents are eligible. This is the floor everyone
            // starts from, and it is what makes a Riverwood farmer's
            // plot possible at all.
            constexpr double kBase = 1.0;
            constexpr double kPerRank = 0.75;
            constexpr double kMembershipBonus = 0.5;

            double weight = kBase;
            int bestRank = -1;
            for (const auto& f : member.factions) {
                bestRank = std::max(bestRank, f.rank);
            }
            if (!member.factions.empty()) {
                weight += kMembershipBonus;
            }
            if (bestRank > 0) {
                weight += kPerRank * static_cast<double>(bestRank);
            }
            return weight;
        }

        // The mastermind's best rank in each admitted faction, for the
        // subordinate test.
        std::unordered_map<RE::FormID, int> RanksOf(const Member& member)
        {
            std::unordered_map<RE::FormID, int> ranks;
            for (const auto& f : member.factions) {
                auto& slot = ranks[f.faction];
                slot = std::max(slot, f.rank);
            }
            return ranks;
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
                            std::uint64_t& rng)
    {
        Result result;
        result.considered.reserve(population.members.size());

        for (const auto& member : population.members) {
            Candidate candidate;
            candidate.npc = member.npc;
            candidate.reject = Screen(occupancy, member.npc, PlotModel::Role::Mastermind, gameDay, alive);
            candidate.weight = candidate.reject == Reject::None ? MastermindWeight(member) : 0.0;
            result.considered.push_back(candidate);
        }

        result.chosen = WeightedDraw(result.considered, rng);
        return result;
    }

    Result SelectActor(const Population& population,
                       RE::FormID mastermind,
                       const OccupancyTable& occupancy,
                       double gameDay,
                       const AlivePredicate& alive,
                       std::uint64_t& rng)
    {
        Result result;

        const Member* boss = population.Find(mastermind);
        if (boss == nullptr) {
            return result;
        }
        const auto bossRanks = RanksOf(*boss);

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

                // A subordinate is someone who ranks LOWER than the
                // mastermind in a faction they share. Equal rank is not
                // subordinate: a fellow rank-3 is a colleague, and
                // ordering them about is a different social act.
                bool subordinate = false;
                for (const auto& f : member.factions) {
                    const auto it = bossRanks.find(f.faction);
                    if (it != bossRanks.end() && f.rank < it->second) {
                        subordinate = true;
                        break;
                    }
                }

                const bool onThisRung = (rung == 1 && subordinate && tied) || (rung == 2 && subordinate)
                                        || (rung == 3 && tied && !sharedFaction);
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

            const RE::FormID picked = WeightedDraw(rungCandidates, rng);
            result.considered.insert(result.considered.end(), rungCandidates.begin(), rungCandidates.end());
            if (picked != 0) {
                result.chosen = picked;
                result.chosenRung = rung;
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
