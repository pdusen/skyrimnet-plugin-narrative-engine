#include <PlotCasting.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

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

        // An ABSOLUTE ceiling, on top of the relative kOutrankMargin.
        //
        // That margin is measured against the mastermind, which makes it
        // inert for the people at the top of a ladder: Elenwen stands at
        // 1.0, so nothing can exceed her by a quarter and every jarl in
        // Skyrim sat inside her guard. That is exactly the mastermind
        // the exponential weighting now draws most often, so the
        // relative test failed where it was needed most.
        //
        // Strictly above 0.75 is the top rung of any ladder the roster
        // declares: a jarl, a guild master, the head of a great family.
        // Those people do not run errands for an acquaintance, whoever
        // is asking. Rungs 1 and 2 are unaffected -- being someone's
        // actual subordinate says more than this does, and a 1.0
        // standing makes that nearly impossible anyway.
        constexpr double kRung3StandingCeiling = 0.75;

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

        // Is this token a word in its own right at `at`, rather than a
        // fragment of a longer one? Hyphens and apostrophes count as
        // word characters so "Snow-Shod" and "Ri'saad" are single words.
        bool IsWordChar(char c)
        {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == '-' || c == '\'';
        }

        bool WholeWordAt(std::string_view text, std::size_t at, std::size_t len)
        {
            const bool leftOk = at == 0 || !IsWordChar(text[at - 1]);

            // An apostrophe ENDS a name and does not begin one, so the
            // two boundaries are not symmetric.
            //
            // This was symmetric, and possessives were the commonest way
            // a step names its subject -- "Erikur's quarters", "Captain
            // Veleth's movements", "the guard captain's footlocker". The
            // trailing apostrophe counted as a word character, so the
            // match was rejected, so the exclusion never fired, so the
            // person the step was ABOUT was eligible to carry it out.
            // Captain Veleth was duly cast to shadow Captain Veleth.
            //
            // The left side keeps the apostrophe, because there it is
            // genuinely part of the word: an O'Ryan is not a Ryan.
            const auto after = at + len;
            const bool rightOk = after >= text.size() || !IsWordChar(text[after]) || text[after] == '\'';
            return leftOk && rightOk;
        }

        bool ContainsWord(std::string_view text, std::string_view word)
        {
            if (word.empty() || word.size() > text.size()) {
                return false;
            }
            for (std::size_t at = 0; at + word.size() <= text.size(); ++at) {
                bool same = true;
                for (std::size_t i = 0; i < word.size() && same; ++i) {
                    same = std::tolower(static_cast<unsigned char>(text[at + i]))
                           == std::tolower(static_cast<unsigned char>(word[i]));
                }
                if (same && WholeWordAt(text, at, word.size())) {
                    return true;
                }
            }
            return false;
        }

        // Does this step's description NAME this person?
        //
        // The step that made this necessary read "Watch Asgeir and
        // Vittoria at the Bee and Barb to learn wedding plans", and
        // casting handed it to Asgeir Snow-Shod -- the groom, spying on
        // his own wedding for the father sabotaging it. Casting cannot
        // see what a step is about: targets are unresolved, so all it
        // has is this sentence, and it was not reading it.
        //
        // TOKEN-WISE, because the model writes "Asgeir", not "Asgeir
        // Snow-Shod". Tokens under four characters are ignored as too
        // collision-prone to be evidence, and bare honorifics are
        // ignored because "Elder Othreloth" would otherwise exclude
        // everyone from a step mentioning any elder.
        //
        // This is NOT name resolution. It never turns a string into an
        // entity -- it asks whether a KNOWN entity's name occurs in one,
        // which has no ambiguity to get wrong. A false positive costs
        // nothing: casting draws somebody else. See
        // docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md for the thing
        // this is deliberately not.
        bool NamesPerson(std::string_view text, std::string_view name)
        {
            if (text.empty() || name.empty()) {
                return false;
            }
            static constexpr std::string_view kTitles[] = {
                "elder",
                "captain",
                "commander",
                "general",
                "legate",
                "jarl",
                "chief",
                "thane",
                "brother",
                "sister",
                "mother",
                "father",
                "master",
                "lady",
            };

            std::size_t at = 0;
            while (at < name.size()) {
                const auto end = name.find(' ', at);
                const auto token = name.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
                at = end == std::string_view::npos ? name.size() : end + 1;

                if (token.size() < 4) {
                    continue;
                }
                bool isTitle = false;
                for (const auto title : kTitles) {
                    if (title.size() == token.size() && ContainsWord(title, token)) {
                        isTitle = true;
                        break;
                    }
                }
                if (!isTitle && ContainsWord(text, token)) {
                    return true;
                }
            }
            return false;
        }

        // The tie from `member` to `other`, or null. Returns the whole
        // Tie rather than a bool and an out-param, because callers need
        // more of it than "does one exist" -- whether it is collegial,
        // and now whether it is hostile.
        const Tie* FindTie(const Member& member, RE::FormID other)
        {
            for (const auto& tie : member.ties) {
                if (tie.other == other) {
                    return &tie;
                }
            }
            return nullptr;
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

    Qualification QualifyAgent(const Population& population,
                               RE::FormID mastermind,
                               RE::FormID candidate,
                               std::string_view stepText,
                               RE::FormID subject,
                               const OccupancyTable& occupancy,
                               double gameDay,
                               const AlivePredicate& alive,
                               const SeatedPredicate& seated)
    {
        Qualification out;

        const Member* boss = population.Find(mastermind);
        const Member* member = population.Find(candidate);
        if (boss == nullptr || member == nullptr || candidate == mastermind) {
            return out;
        }

        // The subject of a step does not carry it out.
        //
        // Twice over, because reading it out of the prose is best-effort
        // and the resolved target is not. The text test catches people
        // the step mentions without targeting; `subject` catches the one
        // it is actually aimed at, whether or not the sentence spells
        // their name the way this parser expects.
        if (subject != 0 && candidate == subject) {
            return out;
        }
        if (NamesPerson(stepText, member->name)) {
            return out;
        }

        // Either direction: the graph writes both, but a tie recorded
        // only one way still connects two people.
        const Tie* tie = FindTie(*boss, member->npc);
        if (tie == nullptr) {
            tie = FindTie(*member, mastermind);
        }
        const bool tied = tie != nullptr;
        const bool sharedFaction = tied && tie->sharedFaction;

        // A Foe is not an agent. The graph knows the difference and plot
        // casting used to throw it away, which is how the Thalmor
        // ambassador came to send Ulfric Stormcloak on an errand.
        const bool hostile = tied && tie->tierDelta > 0;

        // A subordinate is someone whose standing is LOWER than the
        // mastermind's in a faction they share. Equal standing is not
        // subordinate: a fellow Circle member is a colleague, and
        // ordering them about is a different social act.
        const auto bossStandings = StandingsOf(*boss, seated);
        bool subordinate = false;
        for (const auto& f : member->factions) {
            if (!Counts(seated, member->npc, f)) {
                continue;
            }
            const auto it = bossStandings.find(f.faction);
            if (it != bossStandings.end() && f.standing < it->second) {
                subordinate = true;
                break;
            }
        }

        const double bossBest = BestStanding(*boss, seated);
        const double memberBest = BestStanding(*member, seated);
        const bool outranks = memberBest > bossBest + kOutrankMargin || memberBest > kRung3StandingCeiling;

        if (subordinate && tied) {
            out.rung = 1;
        } else if (subordinate) {
            out.rung = 2;
        } else if (tied && !sharedFaction && !hostile && !outranks) {
            out.rung = 3;
        } else {
            return out;
        }

        out.reject = Screen(occupancy, member->npc, PlotModel::Role::Actor, gameDay, alive);
        return out;
    }

    Result SelectActor(const Population& population,
                       RE::FormID mastermind,
                       std::string_view stepText,
                       const OccupancyTable& occupancy,
                       double gameDay,
                       const AlivePredicate& alive,
                       RE::FormID subject,
                       const SeatedPredicate& seated,
                       std::uint64_t& rng,
                       const AgentScorer& score)
    {
        Result result;

        const Member* boss = population.Find(mastermind);
        if (boss == nullptr) {
            return result;
        }
        // Walk the ladder rung by rung and stop at the first rung with
        // anyone on it. Drawing across all rungs at once would let a
        // large pool of distant acquaintances drown out the one
        // subordinate who is the obvious choice.
        for (int rung = 1; rung <= 3 && result.chosen == 0; ++rung) {
            std::vector<Candidate> rungCandidates;

            for (const auto& member : population.members) {
                // One definition of who may act, shared with the
                // description-ranked path. The name exclusion inside it
                // applies to every rung, and NOT to the mastermind's own
                // self-candidacy below -- a scheme naming its own
                // schemer is ordinary, and excluding them there would
                // leave some steps with nobody at all.
                const auto qualified = QualifyAgent(
                    population, mastermind, member.npc, stepText, subject, occupancy, gameDay, alive, seated);
                if (qualified.rung != rung) {
                    continue;
                }

                Candidate candidate;
                candidate.npc = member.npc;
                candidate.rung = rung;
                candidate.reject = qualified.reject;
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
                // Under a scorer the coin flip does not apply: the
                // mastermind is scored against the step's description
                // like everybody else, so they do the work themselves
                // only when they genuinely fit it better than any
                // acquaintance does. The weight above still governs the
                // undescribed case.
            }

            // Best fit among the eligible, or a weighted draw when the
            // step described nobody. Either way the POOL is whatever
            // this rung admitted -- the description never widens it.
            RE::FormID picked = 0;
            if (score) {
                float best = -1.0f;
                for (const auto& c : rungCandidates) {
                    if (c.reject != Reject::None) {
                        continue;
                    }
                    const auto fit = score(c.npc);
                    if (fit > best) {
                        best = fit;
                        picked = c.npc;
                    }
                }
            } else {
                picked = WeightedDraw(rungCandidates, rng);
            }
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
