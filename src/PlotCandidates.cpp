#include <PlotCandidates.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <unordered_map>
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
            // Named in the scheme: offered first, whatever the ladder says.
            bool named = false;
        };

        // What every list of one Build call is ranked against. The scheme's
        // names are read once per call rather than once per person.
        struct Ctx
        {
            const Request& request;
            std::unordered_set<RE::FormID> named;
        };

        // The words of a text, each a run of letters, digits, hyphens and
        // apostrophes, with a trailing possessive dropped: "Hulda's" is
        // "Hulda", and "Ri'saad" stays whole.
        std::vector<std::string> Words(std::string_view text)
        {
            std::vector<std::string> out;
            std::string word;
            const auto flush = [&] {
                if (word.ends_with("'s")) {
                    word.resize(word.size() - 2);
                }
                while (!word.empty() && (word.back() == '\'' || word.back() == '-')) {
                    word.pop_back();
                }
                while (!word.empty() && (word.front() == '\'' || word.front() == '-')) {
                    word.erase(word.begin());
                }
                if (!word.empty()) {
                    out.push_back(std::move(word));
                }
                word.clear();
            };
            for (const char ch : text) {
                const auto u = static_cast<unsigned char>(ch);
                if (std::isalnum(u) != 0 || ch == '-' || ch == '\'') {
                    word.push_back(ch);
                } else {
                    flush();
                }
            }
            flush();
            return out;
        }

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
            return !request.excluded.contains(member.npc);
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

        // Standing at which a member counts as senior in an organisation:
        // the threshold PlotBirth's "who is senior in it" uses.
        constexpr double kSenior = 0.6;

        // Whether `superior` is senior in a rostered organisation `mark`
        // belongs to, and stands above the mark in it.
        bool OutranksIn(const PlotCasting::Member& superior, const PlotCasting::Member& mark)
        {
            for (const auto& theirs : mark.factions) {
                if (!theirs.rostered) {
                    continue;
                }
                for (const auto& own : superior.factions) {
                    if (own.faction == theirs.faction && own.rostered && own.standing >= kSenior
                        && own.standing > theirs.standing) {
                        return true;
                    }
                }
            }
            return false;
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

        Ranked Rank(const Ctx& ctx, const PlotCasting::Member& member)
        {
            Ranked r{&member, kStranger, 0.0, ctx.named.contains(member.npc)};
            if (const auto* boss = ctx.request.population.Find(ctx.request.mastermind)) {
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
                if (a.named != b.named) {
                    return a.named;
                }
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
            std::string why;
            if (r.rank < kStranger) {
                why = PlotMenus::RelationPhrase(static_cast<PlotMenus::Relation>(r.rank));
            } else if (const auto where = SettlementName(request, r.member->settlement); !where.empty()) {
                why = std::format("lives in {}", where);
            }
            if (r.named) {
                why = why.empty() ? std::string{"named in the scheme"} : why + "; named in the scheme";
            }
            if (!why.empty()) {
                line += std::format(" ({})", why);
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
                                      const Ctx& ctx,
                                      const std::vector<Ranked>& ranked,
                                      std::size_t cap)
        {
            std::vector<Candidate> out;
            for (const auto& r : ranked) {
                out.push_back(
                    {{}, {std::string{slot.name}, r.member->npc, r.member->name}, PersonLine(ctx.request, r)});
            }
            return Keyed(std::move(out), cap);
        }

        // Everybody eligible who passes `admit`, the scheme's own people
        // first and then by how connected they are to the mastermind.
        template <typename Admit> std::vector<Ranked> Gather(const Ctx& ctx, Admit admit)
        {
            std::vector<Ranked> out;
            for (const auto& member : ctx.request.population.members) {
                if (!Eligible(ctx.request, member)) {
                    continue;
                }
                const auto r = Rank(ctx, member);
                if (admit(member, r)) {
                    out.push_back(r);
                }
            }
            Sort(out);
            return out;
        }

        // Whom the mastermind knows: by the ladder, or by naming them in the
        // scheme.
        std::vector<Ranked> Known(const Ctx& ctx)
        {
            return Gather(ctx,
                          [](const PlotCasting::Member&, const Ranked& r) { return r.rank < kStranger || r.named; });
        }

        // People an earlier step reached, newest step first.
        std::vector<Ranked> Aware(const Ctx& ctx)
        {
            const auto& request = ctx.request;
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
                    out.push_back(Rank(ctx, *member));
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
        const Ctx ctx{request, NamedIn(request.population, request.naming)};
        return PersonLine(request, Rank(ctx, member));
    }

    std::unordered_set<RE::FormID> NamedIn(const PlotCasting::Population& population, std::string_view text)
    {
        std::unordered_set<RE::FormID> out;
        const auto words = Words(text);
        if (words.empty()) {
            return out;
        }
        const std::unordered_set<std::string> said(words.begin(), words.end());

        // Who carries each word the text says, among the words of anybody's
        // name. Short words ("of", "the") never name anybody.
        std::unordered_map<std::string, std::vector<RE::FormID>> carriers;
        for (const auto& member : population.members) {
            std::unordered_set<std::string> own;
            for (auto& word : Words(member.name)) {
                if (word.size() >= 3 && said.contains(word) && own.insert(word).second) {
                    carriers[word].push_back(member.npc);
                }
            }
        }
        // Two, not one: a parent and child can share a first name, and a
        // scheme naming either should find both rather than neither.
        constexpr std::size_t kMostSharing = 2;
        for (const auto& [word, people] : carriers) {
            if (people.size() <= kMostSharing) {
                out.insert(people.begin(), people.end());
            }
        }
        return out;
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
            if (verb == nullptr || !verb->obtainsItem || step->outcome != PlotModel::StepOutcome::Succeeded) {
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
        // Only a person list reads the scheme's names.
        const auto people = [&] { return Ctx{request, NamedIn(request.population, request.naming)}; };
        switch (slot.rule) {
        case Rule::AnyPerson: {
            const auto ctx = people();
            return People(slot, ctx, Gather(ctx, [](const auto&, const auto&) { return true; }), limits.people);
        }

        case Rule::KnownToMastermind: {
            const auto ctx = people();
            return People(slot, ctx, Known(ctx), limits.people);
        }

        case Rule::NotHostileKnown: {
            const auto ctx = people();
            const auto* boss = request.population.Find(request.mastermind);
            auto known = Known(ctx);
            std::erase_if(known, [boss](const Ranked& r) { return boss != nullptr && Hostile(*boss, *r.member); });
            return People(slot, ctx, known, limits.people);
        }

        case Rule::TradesForLiving: {
            const auto ctx = people();
            return People(slot,
                          ctx,
                          Gather(ctx, [](const PlotCasting::Member& m, const auto&) { return m.tradesForLiving; }),
                          limits.people);
        }

        case Rule::RosteredStanding: {
            const auto ctx = people();
            return People(slot,
                          ctx,
                          Gather(ctx, [](const PlotCasting::Member& m, const auto&) { return HasRosteredStanding(m); }),
                          limits.people);
        }

        case Rule::AuthorityOverMark: {
            // Who hears a denunciation is whoever the mark answers to. First
            // their superiors in an organisation they belong to: a College
            // mage answers to the College's senior mages, a Black-Briar to
            // the family's head, a steward to the jarl, courts being rostered
            // organisations too. Only for a mark nobody stands above does it
            // fall to jurisdiction: the court of the mark's hold, else of the
            // mastermind's, else any. Each list is narrowed rather than
            // reordered, because the decision model does not weigh order:
            // offered Falk Firebeard's own jarl first, it sent his
            // denunciation of a Solitude thane to Riften.
            //
            // A step with no mark -- a petition -- asks on the mastermind's
            // behalf, so it goes to whoever the mastermind answers to.
            const auto ctx = people();
            const auto* mark = Mark(request);
            const auto* boss = request.population.Find(request.mastermind);
            if (const auto* subject = mark != nullptr ? mark : boss; subject != nullptr) {
                const auto superiors = Gather(
                    ctx, [subject](const PlotCasting::Member& m, const auto&) { return OutranksIn(m, *subject); });
                if (!superiors.empty()) {
                    return People(slot, ctx, superiors, limits.people);
                }
            }
            const auto all = Gather(ctx, [](const PlotCasting::Member& m, const auto&) { return m.authority; });
            for (const auto* anchor : {mark, boss}) {
                if (anchor == nullptr) {
                    continue;
                }
                std::vector<Ranked> local;
                std::copy_if(all.begin(), all.end(), std::back_inserter(local), [anchor](const Ranked& r) {
                    return r.member->hold == anchor->hold;
                });
                if (!local.empty()) {
                    return People(slot, ctx, local, limits.people);
                }
            }
            return People(slot, ctx, all, limits.people);
        }

        case Rule::AwareOfEarlierStep: {
            // Nobody an earlier step reached -- the usual case, since a plan
            // is laid out before any of it has happened -- falls back to the
            // people the mastermind knows.
            const auto ctx = people();
            auto aware = Aware(ctx);
            return People(slot, ctx, aware.empty() ? Known(ctx) : aware, limits.people);
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

        case Rule::DocumentPool: {
            // Paper only: what can be falsified or dug up. A forged ruby is
            // not a thing a scheme does.
            std::vector<Candidate> out;
            for (const auto& item : request.world.items) {
                if (item.category == PlotItemPool::Category::Document) {
                    out.push_back({{}, {std::string{slot.name}, item.form, item.displayName}, item.displayName});
                }
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
