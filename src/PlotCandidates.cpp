#include <PlotCandidates.h>

#include <algorithm>
#include <format>
#include <unordered_set>

namespace NarrativeEngine::PlotCandidates
{
    namespace
    {
        using PlotVerbs::Rule;

        // Not related at all. One past the last Relation, so it sorts after
        // every connected person.
        constexpr int kStranger = static_cast<int>(PlotMenus::Relation::Count);

        struct Ranked
        {
            const PlotCasting::Member* member = nullptr;
            int rank = kStranger;
            double standing = 0.0;
        };

        const PlotModel::SlotValue* Filled(const Request& request, std::string_view name)
        {
            for (const auto& value : request.filled) {
                if (value.name == name) {
                    return &value;
                }
            }
            return nullptr;
        }

        // The rules every person slot carries without saying so.
        bool Eligible(const Request& request, const PlotCasting::Member& member)
        {
            if (member.npc == request.mastermind) {
                return false;
            }
            if (request.alive && !request.alive(member.npc)) {
                return false;
            }
            for (const auto& value : request.filled) {
                if (value.form == member.npc) {
                    return false;
                }
            }
            return true;
        }

        bool Hostile(const PlotCasting::Member& boss, const PlotCasting::Member& other)
        {
            for (const auto& tie : boss.ties) {
                if (tie.other == other.npc && tie.tierDelta > 0) {
                    return true;
                }
            }
            for (const auto& tie : other.ties) {
                if (tie.other == boss.npc && tie.tierDelta > 0) {
                    return true;
                }
            }
            return false;
        }

        bool HasRosteredStanding(const PlotCasting::Member& member)
        {
            return std::any_of(member.factions.begin(), member.factions.end(), [](const auto& f) {
                return f.rostered && f.standing > 0.0;
            });
        }

        std::string SettlementName(const Request& request, RE::FormID settlement)
        {
            for (const auto& option : request.world.settlements) {
                if (option.location == settlement) {
                    return option.name;
                }
            }
            return {};
        }

        Ranked Rank(const Request& request, const PlotCasting::Member& member)
        {
            Ranked r{&member, kStranger, 0.0};
            if (const auto* boss = request.population.Find(request.mastermind)) {
                PlotMenus::Relation relation{};
                double standing = 0.0;
                if (PlotMenus::Classify(*boss, member, relation, standing)) {
                    r.rank = static_cast<int>(relation);
                    r.standing = standing;
                }
            }
            return r;
        }

        void Sort(std::vector<Ranked>& people)
        {
            std::sort(people.begin(), people.end(), [](const Ranked& a, const Ranked& b) {
                if (a.rank != b.rank) {
                    return a.rank < b.rank;
                }
                if (a.standing != b.standing) {
                    return a.standing > b.standing;
                }
                return a.member->npc < b.member->npc;
            });
        }

        std::string PersonLine(const Request& request, const Ranked& r)
        {
            std::string line = r.member->name;
            if (request.occupation) {
                const auto what = request.occupation(r.member->npc);
                if (!what.empty()) {
                    line += ", " + what;
                }
            }
            if (r.rank < kStranger) {
                line += std::format(" ({})", PlotMenus::RelationPhrase(static_cast<PlotMenus::Relation>(r.rank)));
            } else if (const auto where = SettlementName(request, r.member->settlement); !where.empty()) {
                line += std::format(" (lives in {})", where);
            }
            return line;
        }

        std::vector<Candidate> Keyed(std::vector<Candidate> list, std::size_t cap)
        {
            if (list.size() > cap) {
                list.resize(cap);
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                list[i].key = std::format("c{}", i + 1);
            }
            return list;
        }

        std::vector<Candidate> People(const PlotVerbs::Slot& slot,
                                      const Request& request,
                                      const std::vector<Ranked>& ranked,
                                      std::size_t cap)
        {
            std::vector<Candidate> out;
            for (const auto& r : ranked) {
                out.push_back({{}, {std::string{slot.name}, r.member->npc, r.member->name}, PersonLine(request, r)});
            }
            return Keyed(std::move(out), cap);
        }

        // Everybody eligible who passes `admit`, ordered by how connected
        // they are to the mastermind.
        template <typename Admit> std::vector<Ranked> Gather(const Request& request, Admit admit)
        {
            std::vector<Ranked> out;
            for (const auto& member : request.population.members) {
                if (!Eligible(request, member)) {
                    continue;
                }
                const auto r = Rank(request, member);
                if (admit(member, r)) {
                    out.push_back(r);
                }
            }
            Sort(out);
            return out;
        }

        std::vector<Ranked> Known(const Request& request)
        {
            return Gather(request, [](const PlotCasting::Member&, const Ranked& r) { return r.rank < kStranger; });
        }

        // People an earlier step reached, newest step first.
        std::vector<Ranked> Aware(const Request& request)
        {
            std::vector<Ranked> out;
            std::unordered_set<RE::FormID> seen;
            for (auto it = request.earlier.rbegin(); it != request.earlier.rend(); ++it) {
                const PlotModel::Step* step = *it;
                const auto* verb = step != nullptr ? PlotVerbs::Find(step->verb) : nullptr;
                if (verb == nullptr) {
                    continue;
                }
                for (std::size_t i = 0; i < verb->slotCount; ++i) {
                    const auto& slot = verb->slots[i];
                    if (slot.kind != PlotVerbs::SlotKind::Person || !AwarenessFired(slot.awareness, step->outcome)) {
                        continue;
                    }
                    const auto* value = step->FindSlot(slot.name);
                    const auto* member = value != nullptr ? request.population.Find(value->form) : nullptr;
                    if (member == nullptr || !Eligible(request, *member) || !seen.insert(member->npc).second) {
                        continue;
                    }
                    out.push_back(Rank(request, *member));
                }
            }
            return out;
        }

        std::vector<Candidate> Places(const PlotVerbs::Slot& slot,
                                      const Request& request,
                                      const std::vector<RE::FormID>& settlements,
                                      std::size_t cap)
        {
            std::vector<Candidate> out;
            std::unordered_set<RE::FormID> seen;
            for (const auto settlement : settlements) {
                if (settlement == 0 || !seen.insert(settlement).second) {
                    continue;
                }
                const auto name = SettlementName(request, settlement);
                if (name.empty()) {
                    continue;
                }
                out.push_back({{}, {std::string{slot.name}, settlement, name}, name});
            }
            return Keyed(std::move(out), cap);
        }

        const PlotCasting::Member* Mark(const Request& request)
        {
            const auto* mark = Filled(request, "mark");
            return mark != nullptr ? request.population.Find(mark->form) : nullptr;
        }
    } // namespace

    std::string Describe(const Request& request, const PlotCasting::Member& member)
    {
        return PersonLine(request, Rank(request, member));
    }

    bool AwarenessFired(PlotVerbs::Awareness awareness, PlotModel::StepOutcome outcome) noexcept
    {
        switch (awareness) {
        case PlotVerbs::Awareness::Contact:
            return outcome == PlotModel::StepOutcome::Succeeded || outcome == PlotModel::StepOutcome::FailedCaught;
        case PlotVerbs::Awareness::Failure:
            return outcome == PlotModel::StepOutcome::FailedCaught;
        case PlotVerbs::Awareness::Never:
        case PlotVerbs::Awareness::Unset:
            break;
        }
        return false;
    }

    bool Asks(const PlotVerbs::Slot& slot) noexcept
    {
        return slot.rule != Rule::BoundItem;
    }

    std::optional<PlotModel::SlotValue> Bind(const PlotVerbs::Slot& slot, const Request& request)
    {
        if (slot.rule != Rule::BoundItem) {
            return std::nullopt;
        }
        for (auto it = request.earlier.rbegin(); it != request.earlier.rend(); ++it) {
            const PlotModel::Step* step = *it;
            const auto* verb = step != nullptr ? PlotVerbs::Find(step->verb) : nullptr;
            if (verb == nullptr || !verb->obtainsItem) {
                continue;
            }
            if (const auto* item = step->FindSlot("item")) {
                return PlotModel::SlotValue{std::string{slot.name}, item->form, item->text};
            }
        }
        return std::nullopt;
    }

    std::vector<Candidate> Build(const PlotVerbs::Slot& slot, const Request& request, const Limits& limits)
    {
        switch (slot.rule) {
        case Rule::AnyPerson:
            return People(slot, request, Gather(request, [](const auto&, const auto&) { return true; }), limits.people);

        case Rule::KnownToMastermind:
            return People(slot, request, Known(request), limits.people);

        case Rule::NotHostileKnown: {
            const auto* boss = request.population.Find(request.mastermind);
            auto known = Known(request);
            std::erase_if(known, [boss](const Ranked& r) { return boss != nullptr && Hostile(*boss, *r.member); });
            return People(slot, request, known, limits.people);
        }

        case Rule::TradesForLiving:
            return People(slot,
                          request,
                          Gather(request, [](const PlotCasting::Member& m, const auto&) { return m.tradesForLiving; }),
                          limits.people);

        case Rule::RosteredStanding:
            return People(
                slot,
                request,
                Gather(request, [](const PlotCasting::Member& m, const auto&) { return HasRosteredStanding(m); }),
                limits.people);

        case Rule::SeniorInCourt:
            return People(slot,
                          request,
                          Gather(request, [](const PlotCasting::Member& m, const auto&) { return m.authority; }),
                          limits.people);

        case Rule::AwareOfEarlierStep: {
            // Nobody an earlier step reached -- the usual case, since a plan
            // is laid out before any of it has happened -- falls back to the
            // people the mastermind knows.
            auto aware = Aware(request);
            return People(slot, request, aware.empty() ? Known(request) : aware, limits.people);
        }

        case Rule::MarkCommunity: {
            const auto* mark = Mark(request);
            if (mark == nullptr) {
                return {};
            }
            std::vector<RE::FormID> settlements{mark->settlement};
            for (const auto& tie : mark->ties) {
                if (const auto* other = request.population.Find(tie.other)) {
                    settlements.push_back(other->settlement);
                }
            }
            return Places(slot, request, settlements, limits.places);
        }

        case Rule::MarkOrMastermindSettlement: {
            std::vector<RE::FormID> settlements;
            if (const auto* mark = Mark(request)) {
                settlements.push_back(mark->settlement);
            }
            if (const auto* boss = request.population.Find(request.mastermind)) {
                settlements.push_back(boss->settlement);
            }
            return Places(slot, request, settlements, limits.places);
        }

        case Rule::ItemPool: {
            std::vector<Candidate> out;
            for (const auto& item : request.world.items) {
                out.push_back({{}, {std::string{slot.name}, item.form, item.displayName}, item.displayName});
            }
            return Keyed(std::move(out), limits.things);
        }

        case Rule::BoundItem: {
            std::vector<Candidate> out;
            if (auto bound = Bind(slot, request)) {
                out.push_back({"c1", *bound, bound->text});
            }
            return out;
        }
        }
        return {};
    }
} // namespace NarrativeEngine::PlotCandidates
