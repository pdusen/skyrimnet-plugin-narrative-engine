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

        Why WhyOf(const Request& request, const Ranked& r)
        {
            Why why;
            if (request.occupation) {
                why.occupation = request.occupation(r.member->npc);
            }
            if (r.rank < kStranger) {
                why.relation = PlotMenus::RelationId(static_cast<PlotMenus::Relation>(r.rank));
            } else {
                why.lives = SettlementName(request, r.member->settlement);
            }
            why.named = r.named;
            return why;
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
                out.push_back({{}, {std::string{slot.name}, r.member->npc, r.member->name}, WhyOf(ctx.request, r)});
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
                out.push_back({{}, {std::string{slot.name}, settlement, name}, {}});
            }
            return Keyed(std::move(out), cap);
        }

        using PlotCasting::Office;

        // Where an office stands in a hold's court, 0 for none. An orc chief
        // has no place in it.
        int CourtRank(Office office)
        {
            switch (office) {
            case Office::GuardCaptain:
                return 1;
            case Office::Steward:
                return 2;
            case Office::Jarl:
                return 3;
            case Office::None:
            case Office::OrcChief:
                break;
            }
            return 0;
        }

        // Whether an orc chief other than `subject` lives where they do: a
        // stronghold, whose people answer to its chief and not to a court.
        bool InStronghold(const Request& request, const PlotCasting::Member& subject)
        {
            return std::any_of(request.population.members.begin(),
                               request.population.members.end(),
                               [&subject](const PlotCasting::Member& m) {
                                   return m.office == Office::OrcChief && m.npc != subject.npc
                                          && m.settlement == subject.settlement;
                               });
        }

        // Whether a court can judge `subject` at all, and whether `judge`
        // sits on it above them: an official only above one of lower rank,
        // and nobody over a stronghold's people or its chief.
        bool CourtJudges(const Request& request, const PlotCasting::Member& judge, const PlotCasting::Member& subject)
        {
            const auto rank = CourtRank(judge.office);
            return rank > 0 && rank > CourtRank(subject.office) && subject.office != Office::OrcChief
                   && !InStronghold(request, subject);
        }

        const PlotCasting::Member* Mark(const Request& request)
        {
            const auto* mark = Filled(request, "mark");
            return mark != nullptr ? request.population.Find(mark->form) : nullptr;
        }

        // Whether the mastermind could send this member on a step, or is
        // them: the agent ladder, without regard to who is busy today. What
        // somebody could be sent to make is a question about the plan, and a
        // plan outlives today's errands.
        bool Reaches(const Request& request, const PlotCasting::Member& member)
        {
            if (request.alive && !request.alive(member.npc)) {
                return false;
            }
            if (member.npc == request.mastermind) {
                return true;
            }
            static const PlotCasting::OccupancyTable kNobodyBusy;
            const auto qualified = PlotCasting::QualifyAgent(
                request.population, request.mastermind, member.npc, {}, 0, kNobodyBusy, 0.0, request.alive, {});
            return qualified.Eligible();
        }

        // Every recipe of `craft` somebody the mastermind can reach is
        // skilled enough to make, in the world's order.
        std::vector<const PlotRecipes::Recipe*> Makeable(const Request& request, PlotRecipes::Craft craft)
        {
            double best = -1.0;
            for (const auto& member : request.population.members) {
                if (member.Practises(craft) && Reaches(request, member)) {
                    best = std::max(best, PlotCasting::CraftSkill(member, craft) * 100.0);
                }
            }
            std::vector<const PlotRecipes::Recipe*> out;
            if (best < 0.0) {
                return out;
            }
            for (const auto& recipe : request.world.recipes) {
                if (recipe.craft == craft && recipe.skill <= best + 1e-9) {
                    out.push_back(&recipe);
                }
            }
            return out;
        }

        std::string Lowered(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        // Whether `text` says this thing, its article aside and in any case:
        // a finish that brings "a gold diamond necklace" names "a Gold
        // Diamond Necklace".
        bool Says(std::string_view text, std::string_view displayName)
        {
            for (const std::string_view article : {"a sheaf of ", "an ", "a "}) {
                if (displayName.starts_with(article)) {
                    displayName.remove_prefix(article.size());
                    break;
                }
            }
            return !displayName.empty() && Lowered(text).find(Lowered(displayName)) != std::string::npos;
        }

        // Up to `count` of `recipes`, evenly spaced from the cheapest to the
        // dearest, so a cut list still runs from an iron dagger to Daedric
        // armor rather than stopping wherever the game's order does.
        std::vector<const PlotRecipes::Recipe*> Spread(std::vector<const PlotRecipes::Recipe*> recipes,
                                                       std::size_t count)
        {
            std::stable_sort(recipes.begin(), recipes.end(), [](const auto* a, const auto* b) {
                return a->ingredientValue < b->ingredientValue;
            });
            if (recipes.size() <= count) {
                return recipes;
            }
            std::vector<const PlotRecipes::Recipe*> out;
            if (count == 0) {
                return out;
            }
            if (count == 1) {
                out.push_back(recipes[recipes.size() / 2]);
                return out;
            }
            for (std::size_t i = 0; i < count; ++i) {
                out.push_back(recipes[i * (recipes.size() - 1) / (count - 1)]);
            }
            return out;
        }

        Candidate Thing(const PlotVerbs::Slot& slot, RE::FormID form, const std::string& text)
        {
            return {{}, {std::string{slot.name}, form, text}, {}};
        }

        // A list of made things: the ones `naming` says first, then a spread
        // of the rest.
        std::vector<Candidate> MadeThings(const PlotVerbs::Slot& slot,
                                          const Request& request,
                                          const std::vector<const PlotRecipes::Recipe*>& recipes,
                                          std::size_t cap)
        {
            std::vector<const PlotRecipes::Recipe*> named;
            std::vector<const PlotRecipes::Recipe*> rest;
            for (const auto* recipe : recipes) {
                (Says(request.naming, recipe->displayName) ? named : rest).push_back(recipe);
            }
            std::vector<Candidate> out;
            for (const auto* recipe : named) {
                out.push_back(Thing(slot, recipe->form, recipe->displayName));
            }
            const auto room = cap > out.size() ? cap - out.size() : 0;
            for (const auto* recipe : Spread(std::move(rest), room)) {
                out.push_back(Thing(slot, recipe->form, recipe->displayName));
            }
            return Keyed(std::move(out), cap);
        }

        // What an item nobody has got yet is chosen from: whatever made thing
        // the scheme names, then the pool, then an even share of each
        // craft's spread for whatever room is left. A made thing the pool
        // already lists is offered once, as the pool's.
        std::vector<Candidate> PoolAndMade(const PlotVerbs::Slot& slot, const Request& request, std::size_t cap)
        {
            std::unordered_set<RE::FormID> pooled;
            for (const auto& item : request.world.items) {
                pooled.insert(item.form);
            }

            std::vector<Candidate> out;
            std::vector<std::vector<const PlotRecipes::Recipe*>> rest;
            for (const auto craft : PlotRecipes::kCrafts) {
                auto& unnamed = rest.emplace_back();
                for (const auto* recipe : Makeable(request, craft)) {
                    if (pooled.contains(recipe->form)) {
                        continue;
                    }
                    if (Says(request.naming, recipe->displayName)) {
                        out.push_back(Thing(slot, recipe->form, recipe->displayName));
                    } else {
                        unnamed.push_back(recipe);
                    }
                }
            }
            for (const auto& item : request.world.items) {
                out.push_back(Thing(slot, item.form, item.displayName));
            }

            const auto crafts = static_cast<std::size_t>(
                std::count_if(rest.begin(), rest.end(), [](const auto& r) { return !r.empty(); }));
            const auto room = cap > out.size() ? cap - out.size() : 0;
            if (crafts > 0) {
                for (auto& recipes : rest) {
                    for (const auto* recipe : Spread(std::move(recipes), room / crafts)) {
                        out.push_back(Thing(slot, recipe->form, recipe->displayName));
                    }
                }
            }
            return Keyed(std::move(out), cap);
        }
        // Every list a rule could give, uncut: a role is judged against the
        // whole of it.
        constexpr Limits kWhole{static_cast<std::size_t>(-1),
                                static_cast<std::size_t>(-1),
                                static_cast<std::size_t>(-1)};

        bool Capitalised(const std::string& word)
        {
            return !word.empty() && std::isupper(static_cast<unsigned char>(word.front())) != 0;
        }

        // Which of `names` `text` names most surely, among those `eligible`
        // admits. Each word a name shares with the text counts one over the
        // number of names carrying it, so "Grelod" says Grelod the Kind --
        // and a word more than two names carry counts for nothing, as
        // NamedIn counts it: "Captain" names none of six captains, where it
        // used to tie them. Returns the indices of every name scoring best:
        // one is a match, several a shared name, none nobody.
        template <typename Eligible>
        std::vector<std::size_t> Closest(const std::vector<std::string_view>& names,
                                         std::string_view text,
                                         Eligible eligible)
        {
            const auto said = Words(text);
            const std::unordered_set<std::string> words(said.begin(), said.end());
            const auto shared = [&words](std::string_view name) {
                std::unordered_set<std::string> out;
                for (auto& word : Words(name)) {
                    if (Capitalised(word) && words.contains(word)) {
                        out.insert(std::move(word));
                    }
                }
                return out;
            };

            std::unordered_map<std::string, std::size_t> carriers;
            for (const auto name : names) {
                for (const auto& word : shared(name)) {
                    ++carriers[word];
                }
            }

            double best = 0.0;
            std::vector<std::size_t> out;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (!eligible(i)) {
                    continue;
                }
                constexpr std::size_t kMostSharing = 2;
                double score = 0.0;
                for (const auto& word : shared(names[i])) {
                    if (carriers[word] <= kMostSharing) {
                        score += 1.0 / static_cast<double>(carriers[word]);
                    }
                }
                if (score <= 0.0) {
                    continue;
                }
                if (score > best + 1e-9) {
                    best = score;
                    out = {i};
                } else if (score > best - 1e-9) {
                    out.push_back(i);
                }
            }
            return out;
        }

        // Who a person role's words name: the living people they name most
        // surely, and when there are none, why -- for the refusal -- and what
        // to tell the retry when the words name nobody at all.
        struct Called
        {
            std::vector<const PlotCasting::Member*> living;
            std::string why;
            std::string hint;
        };

        Called PeopleCalled(const Request& request, std::string_view text)
        {
            const auto& members = request.population.members;
            std::vector<std::string_view> names;
            for (const auto& member : members) {
                names.emplace_back(member.name);
            }
            const auto lives = [&request](RE::FormID npc) { return !request.alive || request.alive(npc); };

            Called out;
            for (const auto i : Closest(names, text, [&](std::size_t k) {
                     return members[k].npc != request.mastermind && lives(members[k].npc);
                 })) {
                out.living.push_back(&members[i]);
            }
            if (!out.living.empty()) {
                return out;
            }

            // The mastermind, who is left out of every role of their own
            // scheme. Without this they were said to be nobody in Skyrim.
            if (const auto self =
                    Closest(names, text, [&](std::size_t k) { return members[k].npc == request.mastermind; });
                !self.empty()) {
                out.why =
                    std::format("{} is the one scheming and cannot be in their own scheme", members[self.front()].name);
                return out;
            }

            // Not among the living. The dead and the disabled are in the
            // population still; the left out never were.
            if (const auto gone = Closest(
                    names,
                    text,
                    [&](std::size_t k) { return members[k].npc != request.mastermind && !lives(members[k].npc); });
                !gone.empty()) {
                out.why = std::format("{} is dead or not in the world", members[gone.front()].name);
                return out;
            }
            const auto& absent = request.population.absent;
            std::vector<std::string_view> absentNames;
            for (const auto& a : absent) {
                absentNames.emplace_back(a.name);
            }
            if (const auto left = Closest(absentNames, text, [](std::size_t) { return true; }); !left.empty()) {
                const auto& a = absent[left.front()];
                out.why = std::format("{} cannot take part in these plots ({})", a.name, a.reason);
                return out;
            }
            // Most often a company, a family or a court named as if it were
            // somebody: "the East Empire Company", "Honningbrew Meadery".
            out.why = std::format("'{}' names nobody in Skyrim", text);
            out.hint = "; a person's role takes one person's name, never a group's or a place's";
            return out;
        }

        // The settlement whose name `text` holds as whole words, the longest
        // name where several do: "the Markarth marketplace" is Markarth.
        const PlotMenus::LocationOption* PlaceCalled(const Request& request, std::string_view text)
        {
            std::string said;
            for (const auto& word : Words(text)) {
                said += " " + Lowered(word);
            }
            said += " ";
            const PlotMenus::LocationOption* out = nullptr;
            for (const auto& place : request.world.settlements) {
                const auto name = std::format(" {} ", Lowered(place.name));
                if (said.find(name) != std::string::npos && (out == nullptr || place.name.size() > out->name.size())) {
                    out = &place;
                }
            }
            return out;
        }

        // What a slot depends on the mark for, so is not judged when the mark
        // could not be resolved.
        bool NeedsMark(PlotVerbs::Rule rule)
        {
            return rule == Rule::AuthorityOverMark || rule == Rule::MarkCommunity
                   || rule == Rule::MarkOrMastermindSettlement;
        }

        bool Lists(const std::vector<Candidate>& list, RE::FormID form)
        {
            return std::any_of(list.begin(), list.end(), [form](const Candidate& c) { return c.value.form == form; });
        }

        // Who or what a rule admits, as the retry is told it, so a refused
        // role always comes with something to choose instead: the whole list
        // when it is short enough to read, else its first few, which are the
        // ones the mastermind is closest to as the rule ranks them.
        std::string Admitted(const std::vector<Candidate>& list)
        {
            if (list.empty()) {
                return "; nobody this plot can reach can be";
            }
            constexpr std::size_t kReadable = 8;
            const auto shown = std::min(list.size(), kReadable);
            std::string out = list.size() > kReadable ? "; it could be, among others, " : "; it can be ";
            for (std::size_t i = 0; i < shown; ++i) {
                out += (i == 0 ? "" : i + 1 == shown ? " or " : ", ") + list[i].value.text;
            }
            return out;
        }
    } // namespace

    Resolution ResolveRoles(const PlotVerbs::Verb& goal,
                            const std::vector<PlotModel::SlotValue>& named,
                            const Request& request)
    {
        Resolution out;
        std::vector<std::string> problems;
        bool markLost = false;

        for (std::size_t i = 0; i < goal.slotCount; ++i) {
            const auto& slot = goal.slots[i];
            const auto role = std::format("the {} ({})", slot.name, slot.says);
            const auto given =
                std::find_if(named.begin(), named.end(), [&slot](const auto& v) { return v.name == slot.name; });
            if (given == named.end() || given->text.empty()) {
                problems.push_back(std::format("nothing was named as {}", role));
                markLost = markLost || slot.name == "mark";
                continue;
            }
            if (markLost && NeedsMark(slot.rule)) {
                continue;
            }
            const auto& text = given->text;

            // Judged as planning would fill it, against what is resolved so
            // far. An item an earlier step will get is chosen from the pool
            // and from what can be made, as planning chooses one.
            const Request here{request.population,
                               request.mastermind,
                               request.world,
                               out.values,
                               request.earlier,
                               request.alive,
                               request.occupation};
            auto judged = slot;
            if (judged.rule == Rule::BoundItem) {
                judged.rule = Rule::PoolOrMade;
            }
            const auto list = Build(judged, here, kWhole);

            switch (slot.kind) {
            case PlotVerbs::SlotKind::Person: {
                // An audience rather than a person: whoever was named, a top
                // official is exposed to the organisation they lead.
                if (list.size() == 1 && list.front().value.form == 0) {
                    out.values.push_back({std::string{slot.name}, 0, list.front().value.text});
                    break;
                }
                const auto called = PeopleCalled(here, text);
                if (called.living.empty()) {
                    problems.push_back(std::format("{}, for {}{}{}", called.why, role, Admitted(list), called.hint));
                    markLost = markLost || slot.name == "mark";
                    continue;
                }
                if (called.living.size() > 1) {
                    std::string who;
                    for (std::size_t k = 0; k < called.living.size(); ++k) {
                        who += (k == 0 ? "" : k + 1 == called.living.size() ? " or " : ", ") + called.living[k]->name;
                    }
                    problems.push_back(std::format("'{}' could be {}; give the whole name, for {}", text, who, role));
                    markLost = markLost || slot.name == "mark";
                    continue;
                }
                const auto& who = *called.living.front();
                if (!Lists(list, who.npc)) {
                    problems.push_back(std::format("{} cannot be {}{}", who.name, role, Admitted(list)));
                    markLost = markLost || slot.name == "mark";
                    continue;
                }
                out.values.push_back({std::string{slot.name}, who.npc, who.name});
                break;
            }
            case PlotVerbs::SlotKind::Place: {
                const auto* place = PlaceCalled(here, text);
                if (place == nullptr) {
                    problems.push_back(std::format("'{}' names no settlement, for {}{}", text, role, Admitted(list)));
                    continue;
                }
                if (!Lists(list, place->location)) {
                    problems.push_back(std::format("{} cannot be {}{}", place->name, role, Admitted(list)));
                    continue;
                }
                out.values.push_back({std::string{slot.name}, place->location, place->name});
                break;
            }
            case PlotVerbs::SlotKind::Thing: {
                const Candidate* match = nullptr;
                for (const auto& c : list) {
                    if (Says(text, c.value.text)
                        && (match == nullptr || c.value.text.size() > match->value.text.size())) {
                        match = &c;
                    }
                }
                if (match == nullptr) {
                    std::string pool;
                    for (const auto& item : request.world.items) {
                        pool += (pool.empty() ? "" : "; ") + item.displayName;
                    }
                    problems.push_back(
                        std::format("'{}' names nothing that can be {}; it must be one of: {}", text, role, pool));
                    continue;
                }
                out.values.push_back({std::string{slot.name}, match->value.form, match->value.text});
                break;
            }
            }
        }

        for (const auto& p : problems) {
            out.problem += (out.problem.empty() ? "" : "; ") + p;
        }
        return out;
    }

    bool CanBeMade(const Request& request, PlotRecipes::Craft craft, RE::FormID form)
    {
        const auto recipes = Makeable(request, craft);
        return std::any_of(recipes.begin(), recipes.end(), [form](const auto* r) { return r->form == form; });
    }

    std::vector<std::string> MadeExamples(const Request& request, std::size_t perCraft)
    {
        std::vector<std::string> out;
        for (const auto craft : PlotRecipes::kCrafts) {
            for (const auto* recipe : Spread(Makeable(request, craft), perCraft)) {
                out.push_back(recipe->displayName);
            }
        }
        return out;
    }

    Why Describe(const Request& request, const PlotCasting::Member& member)
    {
        const Ctx ctx{request, NamedIn(request.population, request.naming)};
        return WhyOf(request, Rank(ctx, member));
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
        case PlotVerbs::Awareness::Success:
            return outcome == PlotModel::StepOutcome::Succeeded;
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
            // Who hears a denunciation is whoever the mark answers to: their
            // superiors in an organisation they belong to -- a College mage
            // answers to the College's senior mages, a Black-Briar to the
            // family's head, a steward to the jarl, courts being rostered
            // organisations too -- and, together with them, whoever holds
            // jurisdiction where the mark lives: the stronghold's chief, or
            // the court of their hold above them. A Battle-Born answers to
            // Olfrid and to Balgruuf alike. Only for a mark with neither does
            // it fall further: to the organisation an official leads, then to
            // the court of the mastermind's hold, else any. Each list is
            // narrowed rather than reordered, because the decision model
            // does not weigh order: offered Falk Firebeard's own jarl first,
            // it sent his denunciation of a Solitude thane to Riften.
            //
            // A step with no mark -- a petition -- asks on the mastermind's
            // behalf, so it goes to whoever the mastermind answers to.
            const auto ctx = people();
            const auto* mark = Mark(request);
            const auto* boss = request.population.Find(request.mastermind);
            const auto* subject = mark != nullptr ? mark : boss;
            if (subject == nullptr) {
                return {};
            }
            auto answersTo =
                Gather(ctx, [subject](const PlotCasting::Member& m, const auto&) { return OutranksIn(m, *subject); });
            const auto add = [&answersTo](const std::vector<Ranked>& more) {
                for (const auto& r : more) {
                    if (std::none_of(answersTo.begin(), answersTo.end(), [&r](const Ranked& had) {
                            return had.member == r.member;
                        })) {
                        answersTo.push_back(r);
                    }
                }
            };

            // A stronghold's people answer to its chief, wherever it stands.
            const auto chiefs = Gather(ctx, [subject](const PlotCasting::Member& m, const auto&) {
                return m.office == Office::OrcChief && m.settlement == subject->settlement;
            });

            // Then the court, and only those on it above the subject: a
            // steward is answerable to the jarl, never to the guard captain.
            const auto all = Gather(ctx, [&request, subject](const PlotCasting::Member& m, const auto&) {
                return CourtJudges(request, m, *subject);
            });
            const auto inHold = [&all](const PlotCasting::Member& anchor) {
                std::vector<Ranked> local;
                std::copy_if(all.begin(), all.end(), std::back_inserter(local), [&anchor](const Ranked& r) {
                    return r.member->hold == anchor.hold;
                });
                return local;
            };
            add(!chiefs.empty() ? chiefs : inHold(*subject));
            if (!answersTo.empty()) {
                return People(slot, ctx, answersTo, limits.people);
            }

            // An official nobody in their own hold stands above -- a jarl, a
            // chief -- answers to no person. They are exposed to the
            // organisation they lead as a whole, as rumors would expose them,
            // and the slot holds how it is said rather than anybody. A
            // petition has no such audience: the mastermind would be
            // petitioning their own following.
            if (mark != nullptr && mark->office != Office::None) {
                const PlotCasting::FactionStanding* top = nullptr;
                for (const auto& f : mark->factions) {
                    if (f.rostered && (top == nullptr || f.standing > top->standing)) {
                        top = &f;
                    }
                }
                std::string audience;
                for (const auto& option : request.world.factions) {
                    if (top != nullptr && option.faction == top->faction) {
                        audience = option.name;
                    }
                }
                if (audience.empty()) {
                    const auto where = SettlementName(request, mark->settlement);
                    audience = where.empty() ? std::string{"the people they lead"} : "the people of " + where;
                }
                return Keyed({{{}, {std::string{slot.name}, 0, audience}, audience}}, limits.people);
            }

            if (boss != nullptr && boss != subject) {
                if (const auto local = inHold(*boss); !local.empty()) {
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
                out.push_back({{}, {std::string{slot.name}, item.form, item.displayName}, {}});
            }
            return Keyed(std::move(out), limits.things);
        }

        case Rule::DocumentPool: {
            // Paper only: what can be falsified or dug up. A forged ruby is
            // not a thing a scheme does.
            std::vector<Candidate> out;
            for (const auto& item : request.world.items) {
                if (item.category == PlotItemPool::Category::Document) {
                    out.push_back({{}, {std::string{slot.name}, item.form, item.displayName}, {}});
                }
            }
            return Keyed(std::move(out), limits.things);
        }

        case Rule::BoundItem: {
            std::vector<Candidate> out;
            if (auto bound = Bind(slot, request)) {
                out.push_back({"c1", *bound, {}});
            }
            return out;
        }

        case Rule::PoolOrMade:
            return PoolAndMade(slot, request, limits.things);

        case Rule::SmithRecipe:
        case Rule::ConcoctRecipe:
        case Rule::EnchantRecipe:
        case Rule::SetRecipe:
        case Rule::FletchRecipe:
            return MadeThings(slot, request, Makeable(request, *PlotVerbs::CraftOf(slot.rule)), limits.things);
        }
        return {};
    }
} // namespace NarrativeEngine::PlotCandidates
