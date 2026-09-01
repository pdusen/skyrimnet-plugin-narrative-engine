#include <PlotMenus.h>

#include <algorithm>
#include <unordered_set>

// The pure half of the menus: population in, four ordered lists out. No
// engine, no logging, no file system -- so the relevance rules can be
// driven by a probe against a fabricated population, which is the only
// way to assert that two different masterminds get two different menus.
namespace NarrativeEngine::PlotMenus
{
    std::string_view RelationId(Relation r) noexcept
    {
        switch (r) {
        case Relation::CloseAndColleague:
            return "close_and_colleague";
        case Relation::Close:
            return "close";
        case Relation::Colleague:
            return "colleague";
        case Relation::Neighbour:
            return "neighbour";
        case Relation::Count:
            break;
        }
        return "unknown";
    }

    std::string_view RelationPhrase(Relation r) noexcept
    {
        switch (r) {
        case Relation::CloseAndColleague:
            return "someone they know, in the same organisation";
        case Relation::Close:
            return "someone they know";
        case Relation::Colleague:
            return "in the same organisation";
        case Relation::Neighbour:
            return "lives in the same hold";
        case Relation::Count:
            break;
        }
        return "";
    }

    namespace
    {
        // The strongest link between two people, or nothing.
        //
        // Deliberately a total order rather than a score. A score would
        // need weights nobody has measured, and the four rungs already
        // say everything the prompt needs: whether they know each other,
        // whether they answer to the same people, or merely whether they
        // could plausibly have met.
        bool Classify(const PlotCasting::Member& boss,
                      const PlotCasting::Member& other,
                      Relation& relation,
                      double& standing)
        {
            standing = 0.0;

            bool close = false;
            for (const auto& tie : boss.ties) {
                if (tie.other == other.npc) {
                    close = true;
                    break;
                }
            }
            if (!close) {
                for (const auto& tie : other.ties) {
                    if (tie.other == boss.npc) {
                        close = true;
                        break;
                    }
                }
            }

            // A shared organisation, and how far up it they are. The
            // standing is theirs rather than the mastermind's: the
            // prompt wants to know who this person IS, and "the Jarl's
            // steward" is a fact about the steward.
            bool colleague = false;
            for (const auto& mine : boss.factions) {
                for (const auto& theirs : other.factions) {
                    if (mine.faction == theirs.faction) {
                        colleague = true;
                        standing = std::max(standing, theirs.standing);
                    }
                }
            }

            if (close && colleague) {
                relation = Relation::CloseAndColleague;
                return true;
            }
            if (close) {
                relation = Relation::Close;
                return true;
            }
            if (colleague) {
                relation = Relation::Colleague;
                return true;
            }
            // Hold 0 means "nowhere resolved", which is not a place two
            // people can share. Without this every unplaced NPC in the
            // province reads as everyone else's neighbour.
            if (boss.hold != 0 && boss.hold == other.hold) {
                relation = Relation::Neighbour;
                return true;
            }
            return false;
        }
    } // namespace

    Menus Build(const PlotCasting::Population& population,
                RE::FormID mastermind,
                const World& world,
                const Limits& limits)
    {
        Menus menus;

        const PlotCasting::Member* boss = population.Find(mastermind);
        if (boss == nullptr) {
            return menus;
        }

        for (const auto& member : population.members) {
            // The mastermind is never on their own menu: a plot whose
            // target is its own author is not a plot.
            if (member.npc == mastermind) {
                continue;
            }
            Relation relation = Relation::Neighbour;
            double standing = 0.0;
            if (!Classify(*boss, member, relation, standing)) {
                continue;
            }
            menus.actors.push_back({member.npc, member.name, relation, standing});
        }

        // Most connected first; within a rung, whoever stands highest in
        // the organisation they share; then by FormID, which is the tie
        // break that makes the order STABLE. Hash order would give the
        // same menu a different numbering run to run, and an index the
        // prompt cannot rely on is worse than no index.
        std::sort(menus.actors.begin(), menus.actors.end(), [](const ActorOption& a, const ActorOption& b) {
            if (a.relation != b.relation) {
                return a.relation < b.relation;
            }
            if (a.standing != b.standing) {
                return a.standing > b.standing;
            }
            return a.npc < b.npc;
        });
        if (menus.actors.size() > limits.actors) {
            menus.actors.resize(limits.actors);
        }

        // The places that matter to a scheme are where its people are,
        // so the locations follow the actors rather than being chosen
        // separately. Deduplicated, and in the actors' order, so the
        // nearest-to-the-plot settlements come first.
        std::unordered_set<RE::FormID> takenLocations;
        const auto addSettlement = [&](RE::FormID settlement) {
            if (settlement == 0 || menus.locations.size() >= limits.locations
                || !takenLocations.insert(settlement).second) {
                return;
            }
            const auto known = std::find_if(world.settlements.begin(),
                                            world.settlements.end(),
                                            [settlement](const LocationOption& l) { return l.location == settlement; });
            if (known != world.settlements.end()) {
                menus.locations.push_back(*known);
            }
        };
        addSettlement(boss->settlement);
        for (const auto& actor : menus.actors) {
            if (const auto* member = population.Find(actor.npc)) {
                addSettlement(member->settlement);
            }
        }

        // The mastermind's own organisations first -- those are the ones
        // they can actually act through -- then the rest of the roster,
        // which is what a scheme might act AGAINST.
        std::unordered_set<RE::FormID> takenFactions;
        for (const auto& mine : boss->factions) {
            if (menus.factions.size() >= limits.factions || !takenFactions.insert(mine.faction).second) {
                continue;
            }
            const auto known = std::find_if(world.factions.begin(),
                                            world.factions.end(),
                                            [&mine](const FactionOption& f) { return f.faction == mine.faction; });
            if (known != world.factions.end()) {
                menus.factions.push_back(*known);
            }
        }
        for (const auto& faction : world.factions) {
            if (menus.factions.size() >= limits.factions) {
                break;
            }
            if (takenFactions.insert(faction.faction).second) {
                menus.factions.push_back(faction);
            }
        }

        // The item pool is already curated and already ordered; it does
        // not depend on who is scheming, so it is passed through whole
        // up to the cap.
        menus.items = world.items;
        if (menus.items.size() > limits.items) {
            menus.items.resize(limits.items);
        }

        return menus;
    }
} // namespace NarrativeEngine::PlotMenus
