#include <PlotPlanning.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace NarrativeEngine::PlotPlanning
{
    namespace
    {
        using PlotModel::SlotValue;
        using PlotModel::Step;

        bool DependsOnAnotherSlot(const PlotVerbs::Slot& slot)
        {
            return slot.rule == PlotVerbs::Rule::MarkCommunity
                   || slot.rule == PlotVerbs::Rule::MarkOrMastermindSettlement;
        }

        bool HasItemSlot(const PlotVerbs::Verb& verb)
        {
            for (std::size_t i = 0; i < verb.slotCount; ++i) {
                if (verb.slots[i].rule == PlotVerbs::Rule::BoundItem) {
                    return true;
                }
            }
            return false;
        }

        const SlotValue* FindIn(const std::vector<SlotValue>& values, std::string_view name)
        {
            for (const auto& value : values) {
                if (value.name == name) {
                    return &value;
                }
            }
            return nullptr;
        }

        std::string Capitalised(std::string text)
        {
            if (!text.empty()) {
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            }
            return text;
        }

        std::string_view OutcomePhrase(PlotModel::StepOutcome outcome)
        {
            switch (outcome) {
            case PlotModel::StepOutcome::Succeeded:
                return "it worked";
            case PlotModel::StepOutcome::FailedCaught:
                return "it went wrong";
            case PlotModel::StepOutcome::FailedTimeout:
                return "it ran out of time";
            case PlotModel::StepOutcome::None:
                break;
            }
            return {};
        }

        // Steps as the model reads them: numbered, and for a step that has
        // already happened, how it went.
        nlohmann::json StepsSoFar(const std::vector<const Step*>& steps)
        {
            nlohmann::json out = nlohmann::json::array();
            std::size_t n = 0;
            for (const auto* step : steps) {
                if (step == nullptr) {
                    continue;
                }
                auto line = std::format("{}. {}", ++n, PlotModel::Label(*step));
                if (const auto how = OutcomePhrase(step->outcome); !how.empty()) {
                    line += std::format(" ({})", how);
                }
                out.push_back(std::move(line));
            }
            return out;
        }

        nlohmann::json Options(const std::vector<PlotCandidates::Candidate>& list)
        {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& c : list) {
                out.push_back({{"key", c.key}, {"line", c.line}});
            }
            return out;
        }

        std::string_view KindId(PlotVerbs::SlotKind kind)
        {
            switch (kind) {
            case PlotVerbs::SlotKind::Person:
                return "person";
            case PlotVerbs::SlotKind::Place:
                return "place";
            case PlotVerbs::SlotKind::Thing:
                return "thing";
            }
            return "person";
        }

        // The name of a verb's first person slot: the person it acts on.
        std::string_view TargetSlot(const PlotVerbs::Verb& verb)
        {
            for (std::size_t i = 0; i < verb.slotCount; ++i) {
                if (verb.slots[i].kind == PlotVerbs::SlotKind::Person) {
                    return verb.slots[i].name;
                }
            }
            // Nobody it is done to: a crafting step is about the thing it
            // makes, and giving up on one is giving up on making that.
            return verb.slotCount > 0 ? verb.slots[0].name : std::string_view{};
        }

        // Everybody a step names in a person slot, other than its
        // mastermind: who a preparation or a theft serving it must leave be.
        std::unordered_set<RE::FormID> PeopleOf(const Step& step)
        {
            std::unordered_set<RE::FormID> out;
            const auto* verb = PlotVerbs::Find(step.verb);
            if (verb == nullptr) {
                return out;
            }
            for (std::size_t i = 0; i < verb->slotCount; ++i) {
                if (verb->slots[i].kind != PlotVerbs::SlotKind::Person) {
                    continue;
                }
                if (const auto* value = step.FindSlot(verb->slots[i].name); value != nullptr && value->form != 0) {
                    out.insert(value->form);
                }
            }
            return out;
        }

        // A step of `verb` with only `preset` filled, for showing as an
        // option before the rest of it is chosen.
        Step Partial(const PlotVerbs::Verb& verb, const std::vector<SlotValue>& preset)
        {
            Step step;
            step.verb = std::string{verb.id};
            step.type = verb.profile;
            step.slots = preset;
            return step;
        }

        class Planner
        {
        public:
            Planner(const Input& input, const Ask& ask)
                : input_(input), ask_(ask), naming_(input.scheme + "\n" + input.finish),
                  named_(PlotCandidates::NamedIn(input.population, naming_))
            {}

            Outcome Run()
            {
                Outcome out;
                auto result = Goal();
                if (result == Fill::Done) {
                    result = Prepare();
                }
                out.decisions = decisions_;
                if (result == Fill::Failed) {
                    out.error = error_;
                    return out;
                }
                if (result == Fill::Unfillable) {
                    out.rejection = rejection_;
                    return out;
                }
                out.plan = std::move(plan_);
                out.ok = true;
                return out;
            }

        private:
            enum class Fill : std::uint8_t
            {
                Done,
                Unfillable, // nothing could be offered, or the answer was not on the list
                Failed      // a decision was not answered
            };

            const Input& input_;
            const Ask& ask_;
            std::vector<Step> plan_;
            // Beside each planned step: whether it may be prepared. The goal
            // and the steps that get items may; a preparation may not, or a
            // bribe of a bystander grows a watch of its own and the plan
            // turns into one about the person bribed.
            std::vector<bool> preparable_;
            std::size_t decisions_ = 0;
            std::string error_;
            std::string rejection_;
            // The scheme and its finish, whose people head every list.
            std::string naming_;
            // Whoever they name. Kept off any slot a preparation leaves
            // open: they are what the plot is about, not its helpers.
            std::unordered_set<RE::FormID> named_;

            SkyrimNetAPI::DecisionResult Decide(std::string_view name, const nlohmann::json& ctx)
            {
                ++decisions_;
                return ask_(name, ctx);
            }

            // The steps ahead of plan position `index`: history, then the
            // plan up to it. Rebuilt on every call, because inserting into
            // the plan moves its steps.
            std::vector<const Step*> Earlier(std::size_t index) const
            {
                auto out = input_.before;
                for (std::size_t i = 0; i < index && i < plan_.size(); ++i) {
                    out.push_back(&plan_[i]);
                }
                return out;
            }

            // Every step a new one must not repeat.
            std::vector<const Step*> Existing() const
            {
                return Earlier(plan_.size());
            }

            PlotCandidates::Request RequestOf(const std::vector<SlotValue>& filled,
                                              const std::vector<const Step*>& earlier,
                                              const std::unordered_set<RE::FormID>& excluded) const
            {
                PlotCandidates::Request request{input_.population,
                                                input_.mastermind,
                                                input_.world,
                                                filled,
                                                earlier,
                                                input_.alive,
                                                input_.occupation,
                                                naming_};
                request.excluded = excluded;
                return request;
            }

            // What every question is told about the schemer.
            nlohmann::json Base() const
            {
                nlohmann::json ctx;
                ctx["mastermind"] = {{"name", input_.mastermindName}, {"standing", input_.standing}};
                ctx["bio"] = input_.bio;
                ctx["ambition"] = input_.ambition;
                ctx["scheme"] = input_.scheme;
                ctx["finish"] = input_.finish;
                ctx["history"] = StepsSoFar(input_.before);
                return ctx;
            }

            nlohmann::json PlanText() const
            {
                nlohmann::json out = nlohmann::json::array();
                for (std::size_t i = 0; i < plan_.size(); ++i) {
                    out.push_back(std::format("{}. {}", i + 1, Sketch(plan_[i])));
                }
                return out;
            }

            // Whether the pool lists this item as a document.
            bool IsDocument(RE::FormID form) const
            {
                return std::any_of(input_.world.items.begin(), input_.world.items.end(), [form](const auto& item) {
                    return item.form == form && item.category == PlotItemPool::Category::Document;
                });
            }

            // Whether the plot has given up on `verb` against `target`.
            bool IsAbandoned(const PlotVerbs::Verb& verb, RE::FormID target) const
            {
                return std::any_of(
                    input_.abandoned.begin(), input_.abandoned.end(), [&](const PlotModel::Abandoned& a) {
                        return a.verb == verb.id && a.target == target;
                    });
            }

            // A target slot's list, less whoever the plot has given up on
            // acting on with this verb.
            void DropAbandoned(const PlotVerbs::Verb& verb,
                               const PlotVerbs::Slot& slot,
                               std::vector<PlotCandidates::Candidate>& list) const
            {
                if (slot.name != TargetSlot(verb)) {
                    return;
                }
                std::erase_if(list,
                              [&](const PlotCandidates::Candidate& c) { return IsAbandoned(verb, c.value.form); });
            }

            // Whether some step of the plot already does `verb` with the
            // values in `preset`: the same watch of the same person, or --
            // with nothing preset -- the same verb at all.
            bool Repeats(const PlotVerbs::Verb& verb, const std::vector<SlotValue>& preset) const
            {
                for (const auto* other : Existing()) {
                    if (other->verb != verb.id) {
                        continue;
                    }
                    const bool same = std::all_of(preset.begin(), preset.end(), [other](const SlotValue& v) {
                        const auto* theirs = other->FindSlot(v.name);
                        return theirs != nullptr && theirs->form == v.form;
                    });
                    if (same) {
                        return true;
                    }
                }
                return false;
            }

            // Whether `verb` could be filled at `index`: every slot it would
            // ask has somebody or something to ask about. A slot that
            // depends on one not yet filled is judged when that one is.
            bool Fillable(const PlotVerbs::Verb& verb,
                          const std::vector<SlotValue>& preset,
                          const std::unordered_set<RE::FormID>& excluded,
                          std::size_t index) const
            {
                const auto earlier = Earlier(index);
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    const auto& slot = verb.slots[i];
                    if (const auto* given = FindIn(preset, slot.name); given != nullptr) {
                        if (slot.name == TargetSlot(verb) && IsAbandoned(verb, given->form)) {
                            return false;
                        }
                        // A person given from before -- a kept goal, a named
                        // role -- may have died or gone since.
                        if (slot.kind == PlotVerbs::SlotKind::Person && given->form != 0
                            && (input_.population.Find(given->form) == nullptr
                                || (input_.alive && !input_.alive(given->form)))) {
                            return false;
                        }
                        continue;
                    }
                    if (slot.rule == PlotVerbs::Rule::BoundItem) {
                        if (input_.world.items.empty() && input_.world.recipes.empty()
                            && !PlotCandidates::Bind(slot, RequestOf(preset, earlier, excluded))) {
                            return false;
                        }
                        continue;
                    }
                    if (DependsOnAnotherSlot(slot) && FindIn(preset, "mark") == nullptr) {
                        continue;
                    }
                    auto list = PlotCandidates::Build(slot, RequestOf(preset, earlier, excluded));
                    DropAbandoned(verb, slot, list);
                    if (list.empty()) {
                        return false;
                    }
                }
                return true;
            }

            // Fill `verb` as the step that will stand at `index`. `preset`
            // slots are taken as given; every other is asked, unless it has
            // one candidate. An item slot takes an item a succeeded step
            // already got, or else is chosen from the pool -- and the plan
            // then gets it (GetItems).
            Fill FillSlots(const PlotVerbs::Verb& verb,
                           const std::vector<SlotValue>& preset,
                           const std::unordered_set<RE::FormID>& excluded,
                           std::size_t index,
                           const std::string& purpose,
                           Step& step)
            {
                step = Partial(verb, preset);
                step.slots.insert(step.slots.begin(),
                                  SlotValue{"mastermind", input_.mastermind, input_.mastermindName});

                const auto earlier = Earlier(index);
                const auto existing = Existing();

                // The last slot asked is the one that could complete a
                // repeat, so it is the one whose list leaves those out.
                std::size_t lastAsked = verb.slotCount;
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    if (FindIn(preset, verb.slots[i].name) == nullptr
                        && verb.slots[i].rule != PlotVerbs::Rule::BoundItem) {
                        lastAsked = i;
                    }
                }

                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    const auto& slot = verb.slots[i];
                    if (FindIn(preset, slot.name) != nullptr) {
                        continue;
                    }
                    const auto request = RequestOf(step.slots, earlier, excluded);

                    std::vector<PlotCandidates::Candidate> list;
                    if (slot.rule == PlotVerbs::Rule::BoundItem) {
                        if (auto held = PlotCandidates::Bind(slot, request)) {
                            step.slots.push_back(std::move(*held));
                            continue;
                        }
                        auto fromPool = slot;
                        fromPool.rule = PlotVerbs::Rule::PoolOrMade;
                        list = PlotCandidates::Build(fromPool, request);
                    } else {
                        list = PlotCandidates::Build(slot, request);
                    }
                    DropAbandoned(verb, slot, list);

                    if (i == lastAsked) {
                        std::erase_if(list, [&](const PlotCandidates::Candidate& c) {
                            auto trial = step;
                            trial.slots.push_back(c.value);
                            return std::any_of(existing.begin(), existing.end(), [&trial](const Step* other) {
                                return SameStep(trial, *other);
                            });
                        });
                    }
                    if (list.empty()) {
                        rejection_ = std::format("{}: nobody or nothing can fill {{{}}}", verb.id, slot.name);
                        return Fill::Unfillable;
                    }

                    const PlotCandidates::Candidate* chosen = &list.front();
                    if (list.size() > 1) {
                        nlohmann::json ctx;
                        ctx["mastermind"] = {{"name", input_.mastermindName}};
                        ctx["scheme"] = input_.scheme;
                        ctx["finish"] = input_.finish;
                        ctx["step"] = Sketch(step);
                        ctx["purpose"] = purpose;
                        ctx["slot"] = slot.name;
                        ctx["kind"] = KindId(slot.kind);
                        ctx["options"] = Options(list);

                        const auto result = Decide(kSlotTemplate, ctx);
                        if (!result.ok) {
                            error_ = result.error;
                            return Fill::Failed;
                        }
                        const auto key = result.Choice(kSlotQuestion);
                        chosen = nullptr;
                        for (const auto& c : list) {
                            if (c.key == key) {
                                chosen = &c;
                            }
                        }
                        if (chosen == nullptr) {
                            rejection_ = std::format(
                                "{}: {{{}}} was answered '{}', which was not offered", verb.id, slot.name, key);
                            return Fill::Unfillable;
                        }
                    }
                    step.slots.push_back(chosen->value);
                }

                // The person the step is done to, for the resolution model,
                // which sizes a step by its target's importance and distance
                // and holds a conspicuous one while that target is near the
                // player. The first person slot is the one the verb acts on.
                for (std::size_t i = 0; i < verb.slotCount; ++i) {
                    if (verb.slots[i].kind == PlotVerbs::SlotKind::Person) {
                        if (const auto* value = step.FindSlot(verb.slots[i].name)) {
                            step.target = value->form;
                            step.targetName = value->text;
                        }
                        break;
                    }
                }
                return Fill::Done;
            }

            // Whether the step at `index` needs an item nobody ahead of it
            // will have got.
            bool NeedsItem(std::size_t index) const
            {
                const auto& step = plan_[index];
                const auto* verb = PlotVerbs::Find(step.verb);
                const auto* item = step.FindSlot("item");
                if (verb == nullptr || !HasItemSlot(*verb) || item == nullptr) {
                    return false;
                }
                for (const auto* earlier : Earlier(index)) {
                    const auto* theirs = PlotVerbs::Find(earlier->verb);
                    const auto* got = earlier->FindSlot("item");
                    if (theirs == nullptr || !theirs->obtainsItem || got == nullptr || got->form != item->form) {
                        continue;
                    }
                    // A planned step will get it; one in the history has
                    // only if it worked.
                    if (earlier->outcome == PlotModel::StepOutcome::None
                        || earlier->outcome == PlotModel::StepOutcome::Succeeded) {
                        return false;
                    }
                }
                return true;
            }

            // Put a theft or a purchase of the item the step at `index`
            // needs just before it, from somebody who is not in that step.
            Fill Obtain(std::size_t index)
            {
                const Step needing = plan_[index];
                const auto* item = needing.FindSlot("item");
                const std::vector<SlotValue> preset{{"item", item->form, item->text}};
                const auto* mark = needing.FindSlot("mark");

                // Nobody in the step being served is robbed or paid for it, so
                // the bribe before a theft never pays the person robbed. An
                // exposure is the exception: the proof against somebody is
                // theirs, and the ledger exposing Erikur is Erikur's. Only its
                // authority is kept out.
                auto excluded = PeopleOf(needing);
                if (needing.verb == "expose" && mark != nullptr) {
                    excluded.erase(mark->form);
                }

                // One way of getting the item, with what it is given.
                struct Way
                {
                    const PlotVerbs::Verb* verb = nullptr;
                    std::vector<SlotValue> preset;
                    std::unordered_set<RE::FormID> excluded;
                };
                std::vector<Way> ways;
                const auto offer = [&](const PlotVerbs::Verb* verb,
                                       std::vector<SlotValue> given,
                                       std::unordered_set<RE::FormID> keptOut) {
                    if (verb != nullptr && Fillable(*verb, given, keptOut, index)) {
                        ways.push_back({verb, std::move(given), std::move(keptOut)});
                    }
                };

                for (const auto* id : {"steal", "buy"}) {
                    offer(PlotVerbs::Find(id), preset, excluded);
                }
                if (IsDocument(item->form)) {
                    // A document can also be falsified: in the name of the
                    // person the step it serves is about, when it is about
                    // somebody, since a paper in a person's own name, planted
                    // on them, is what framing them is. Asked again, Jarl
                    // Idgrod's forgery went into her daughter's name.
                    auto forged = preset;
                    if (mark != nullptr) {
                        forged.push_back({"subject", mark->form, mark->text});
                    }
                    offer(PlotVerbs::Find("falsify"), std::move(forged), {});
                    // Or dug up: against the person the step it serves is
                    // about, when it is about somebody.
                    auto dug = preset;
                    if (mark != nullptr) {
                        dug.push_back({"mark", mark->form, mark->text});
                    }
                    offer(PlotVerbs::Find("dig_up"), std::move(dug), mark != nullptr ? decltype(excluded){} : excluded);
                }
                // And anything a craftsman the mastermind can reach is able to
                // make can be made: the crafter then holds it, and carries it
                // to whoever it is for. Nobody is robbed in the making, so
                // nobody is kept out.
                const auto request = RequestOf(preset, Earlier(index), {});
                for (const auto& verb : PlotVerbs::kVerbs) {
                    const auto craft = PlotVerbs::CraftOf(verb);
                    if (craft && PlotCandidates::CanBeMade(request, *craft, item->form)) {
                        offer(&verb, preset, {});
                    }
                }
                if (ways.empty()) {
                    rejection_ = std::format("nobody to get {} from for {}", item->text, needing.verb);
                    return Fill::Unfillable;
                }

                const Way* way = &ways.front();
                if (ways.size() > 1) {
                    nlohmann::json options = nlohmann::json::array();
                    for (const auto& w : ways) {
                        options.push_back({{"key", w.verb->id}, {"line", Sketch(Partial(*w.verb, w.preset))}});
                    }
                    auto ctx = Base();
                    ctx["plan"] = PlanText();
                    ctx["need"] = {{"step", index + 1}, {"sketch", Sketch(needing)}, {"item", item->text}};
                    ctx["options"] = std::move(options);

                    const auto result = Decide(kObtainTemplate, ctx);
                    if (!result.ok) {
                        error_ = result.error;
                        return Fill::Failed;
                    }
                    const auto key = result.Choice(kObtainQuestion);
                    way = nullptr;
                    for (const auto& w : ways) {
                        if (w.verb->id == key) {
                            way = &w;
                        }
                    }
                    if (way == nullptr) {
                        rejection_ =
                            std::format("getting {} was answered '{}', which was not offered", item->text, key);
                        return Fill::Unfillable;
                    }
                }

                Step step;
                const auto purpose = std::format("It gets {} for step {}: {}", item->text, index + 1, Sketch(needing));
                const auto filled = FillSlots(*way->verb, way->preset, way->excluded, index, purpose, step);
                if (filled != Fill::Done) {
                    return filled;
                }
                plan_.insert(plan_.begin() + static_cast<std::ptrdiff_t>(index), std::move(step));
                preparable_.insert(preparable_.begin() + static_cast<std::ptrdiff_t>(index), true);
                return Fill::Done;
            }

            // Get every item the plan needs and does not have.
            Fill GetItems()
            {
                for (std::size_t i = 0; i < plan_.size(); ++i) {
                    if (!NeedsItem(i)) {
                        continue;
                    }
                    const auto got = Obtain(i);
                    if (got != Fill::Done) {
                        return got;
                    }
                    ++i; // the step that needed it is now one further on
                }
                return Fill::Done;
            }

            // 1. The step that accomplishes the scheme: the one the scheme
            // was written to end in, or else asked for.
            Fill Goal()
            {
                // A re-plan keeps the goal it had, every slot as it was.
                if (input_.goalStep != nullptr) {
                    const auto* kept = PlotVerbs::Find(input_.goalStep->verb);
                    std::vector<SlotValue> preset;
                    for (const auto& value : input_.goalStep->slots) {
                        if (value.name != "mastermind") {
                            preset.push_back(value);
                        }
                    }
                    if (kept != nullptr && kept->finishes && Fillable(*kept, preset, {}, 0)) {
                        Step goal;
                        const auto filled = FillSlots(*kept, preset, {}, 0, "This step accomplishes the scheme.", goal);
                        if (filled != Fill::Done) {
                            return filled;
                        }
                        plan_.push_back(std::move(goal));
                        preparable_.push_back(true);
                        return GetItems();
                    }
                }

                // A goal whose roles the scheme call named is done to them, as
                // it named them: nothing about it is asked.
                if (!input_.goalRoles.empty()) {
                    const auto* named = PlotVerbs::Find(input_.goal);
                    if (named != nullptr && named->finishes && Fillable(*named, input_.goalRoles, {}, 0)) {
                        Step goal;
                        const auto filled =
                            FillSlots(*named, input_.goalRoles, {}, 0, "This step accomplishes the scheme.", goal);
                        if (filled != Fill::Done) {
                            return filled;
                        }
                        plan_.push_back(std::move(goal));
                        preparable_.push_back(true);
                        return GetItems();
                    }
                }

                std::vector<const PlotVerbs::Verb*> offered;
                nlohmann::json options = nlohmann::json::array();
                for (const auto& verb : PlotVerbs::kVerbs) {
                    if (HasItemSlot(verb) && input_.maxSteps < kPlanFloor) {
                        continue;
                    }
                    // Making, finding or hiding a thing is for a later step.
                    if (!verb.finishes) {
                        continue;
                    }
                    if (Fillable(verb, {}, {}, 0)) {
                        offered.push_back(&verb);
                        options.push_back({{"key", verb.id}, {"line", PlotVerbs::Generic(verb)}});
                    }
                }
                if (offered.empty()) {
                    rejection_ = "no verb could be offered";
                    return Fill::Unfillable;
                }

                const PlotVerbs::Verb* verb = nullptr;
                for (const auto* candidate : offered) {
                    if (candidate->id == input_.goal) {
                        verb = candidate;
                    }
                }
                // One verb that can be filled is the goal without asking:
                // SkyrimNet refuses a choice question with one option.
                if (verb == nullptr && offered.size() == 1) {
                    verb = offered.front();
                }
                if (verb == nullptr) {
                    auto ctx = Base();
                    ctx["options"] = std::move(options);
                    const auto result = Decide(kGoalTemplate, ctx);
                    if (!result.ok) {
                        error_ = result.error;
                        return Fill::Failed;
                    }
                    const auto key = result.Choice(kGoalQuestion);
                    for (const auto* candidate : offered) {
                        if (candidate->id == key) {
                            verb = candidate;
                        }
                    }
                    if (verb == nullptr) {
                        rejection_ = std::format("the goal was answered '{}', which was not offered", key);
                        return Fill::Unfillable;
                    }
                }

                Step goal;
                const auto filled = FillSlots(*verb, {}, {}, 0, "This step accomplishes the scheme.", goal);
                if (filled != Fill::Done) {
                    return filled;
                }
                plan_.push_back(std::move(goal));
                preparable_.push_back(true);
                return GetItems();
            }

            // 3. What must be done first, until `ready` or the cap.
            Fill Prepare()
            {
                struct Option
                {
                    std::size_t before = 0;
                    const PlotVerbs::Verb* verb = nullptr;
                    std::vector<SlotValue> preset;
                    std::unordered_set<RE::FormID> excluded;
                };

                while (plan_.size() < input_.maxSteps) {
                    std::vector<Option> offered;
                    nlohmann::json options = nlohmann::json::array();
                    for (std::size_t k = 0; k < plan_.size(); ++k) {
                        if (!preparable_[k]) {
                            continue;
                        }
                        for (const auto* prep : PlotVerbs::PreparationsOf(plan_[k].verb)) {
                            const auto* verb = PlotVerbs::Find(prep->verb);
                            if (verb == nullptr) {
                                continue;
                            }
                            // Room for it, and for the step that gets its item.
                            const std::size_t room = HasItemSlot(*verb) ? kPlanFloor : 1;
                            if (plan_.size() + room > input_.maxSteps) {
                                continue;
                            }
                            Option option{k, verb, {}, PeopleOf(plan_[k])};
                            if (prep->slot.empty()) {
                                // An open slot -- the bribe before a theft --
                                // never hires somebody the plot is about.
                                option.excluded.insert(named_.begin(), named_.end());
                            } else {
                                const auto* value = plan_[k].FindSlot(prep->from);
                                if (value == nullptr) {
                                    continue;
                                }
                                option.preset.push_back({std::string{prep->slot}, value->form, value->text});
                            }
                            if (Repeats(*verb, option.preset) || !Fillable(*verb, option.preset, option.excluded, k)) {
                                continue;
                            }
                            options.push_back(
                                {{"key", std::format("p{}", offered.size() + 1)},
                                 {"line",
                                  std::format("Before step {}: {}", k + 1, Sketch(Partial(*verb, option.preset)))}});
                            offered.push_back(std::move(option));
                        }
                    }
                    if (offered.empty()) {
                        break;
                    }
                    // One step is an errand, not a scheme: with only the goal
                    // planned, something has to come before it.
                    if (plan_.size() >= kPlanFloor) {
                        options.push_back({{"key", kReady}, {"line", "Nothing more: the plan is ready to carry out."}});
                    }

                    const Option* chosen = nullptr;
                    if (options.size() == 1) {
                        // One preparation and no `ready` yet: the plan needs a
                        // second step and this is the only one there is. Taken
                        // without asking, as SkyrimNet refuses a choice
                        // question with one option -- Quintus Navale's
                        // re-plan conceded on exactly that.
                        chosen = &offered.front();
                    } else {
                        auto ctx = Base();
                        ctx["plan"] = PlanText();
                        ctx["options"] = std::move(options);
                        const auto result = Decide(kPrepareTemplate, ctx);
                        if (!result.ok) {
                            error_ = result.error;
                            return Fill::Failed;
                        }
                        const auto key = result.Choice(kPrepareQuestion);
                        if (key == kReady) {
                            break;
                        }
                        for (std::size_t i = 0; i < offered.size(); ++i) {
                            if (key == std::format("p{}", i + 1)) {
                                chosen = &offered[i];
                            }
                        }
                    }
                    // What is planned already holds together; a bad answer
                    // ends the preparing rather than the plan.
                    if (chosen == nullptr) {
                        break;
                    }

                    Step step;
                    const auto purpose =
                        std::format("It prepares for step {}: {}", chosen->before + 1, Sketch(plan_[chosen->before]));
                    const auto filled =
                        FillSlots(*chosen->verb, chosen->preset, chosen->excluded, chosen->before, purpose, step);
                    if (filled == Fill::Failed) {
                        return filled;
                    }
                    if (filled == Fill::Unfillable) {
                        rejection_.clear();
                        break;
                    }
                    const auto at = static_cast<std::ptrdiff_t>(chosen->before);
                    plan_.insert(plan_.begin() + at, std::move(step));
                    preparable_.insert(preparable_.begin() + at, false);

                    const auto got = GetItems();
                    if (got == Fill::Failed) {
                        return got;
                    }
                    if (got == Fill::Unfillable) {
                        plan_.erase(plan_.begin() + at);
                        preparable_.erase(preparable_.begin() + at);
                        rejection_.clear();
                        break;
                    }
                }
                return Fill::Done;
            }
        };
    } // namespace

    bool SessionCannotDecide(std::string_view error) noexcept
    {
        return error == SkyrimNetAPI::kDecisionUnavailable || error == SkyrimNetAPI::kDecisionNoRoute
               || error == "ai_disabled";
    }

    bool SameStep(const Step& a, const Step& b)
    {
        if (a.verb != b.verb || a.slots.size() != b.slots.size()) {
            return false;
        }
        return std::all_of(a.slots.begin(), a.slots.end(), [&b](const SlotValue& v) {
            const auto* theirs = b.FindSlot(v.name);
            return theirs != nullptr && theirs->form == v.form;
        });
    }

    std::string Sketch(const Step& step, bool agentOpen)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        if (verb == nullptr) {
            return PlotModel::Label(step);
        }
        return Capitalised(PlotVerbs::Render(
            verb->Text(PlotVerbs::Template::Dashboard),
            [&step, agentOpen](std::string_view name) -> std::optional<std::string> {
                if (name == "agent") {
                    if (!step.actorName.empty()) {
                        return step.actorName;
                    }
                    return agentOpen ? std::nullopt : std::optional<std::string>{"someone"};
                }
                const auto* value = step.FindSlot(name);
                return value != nullptr ? std::optional<std::string>{value->text} : std::nullopt;
            },
            [](std::string_view name) { return std::format("[{}]", name); }));
    }

    bool FinishFailed(const PlotModel::Plot& plot) noexcept
    {
        return !plot.plan.empty() && plot.cursor + 1 >= plot.plan.size();
    }

    RE::FormID TargetOf(const Step& step)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        if (verb == nullptr) {
            return 0;
        }
        const auto* value = step.FindSlot(TargetSlot(*verb));
        return value != nullptr ? value->form : 0;
    }

    bool Serves(const Step& step, const Step& later)
    {
        const auto* verb = PlotVerbs::Find(step.verb);
        const auto* laterVerb = PlotVerbs::Find(later.verb);
        if (verb == nullptr || laterVerb == nullptr) {
            return false;
        }
        if (verb->obtainsItem && HasItemSlot(*laterVerb)) {
            const auto* got = step.FindSlot("item");
            const auto* used = later.FindSlot("item");
            if (got != nullptr && used != nullptr && got->form == used->form) {
                return true;
            }
        }
        for (const auto* prep : PlotVerbs::PreparationsOf(later.verb)) {
            if (prep->verb != step.verb) {
                continue;
            }
            if (prep->slot.empty()) {
                return true;
            }
            const auto* own = step.FindSlot(prep->slot);
            const auto* theirs = later.FindSlot(prep->from);
            if (own != nullptr && theirs != nullptr && own->form == theirs->form) {
                return true;
            }
        }
        return false;
    }

    std::vector<PlotModel::Abandoned> RouteOf(const PlotModel::Plot& plot)
    {
        if (plot.history.empty()) {
            return {};
        }
        std::vector<const Step*> route{&plot.history.back()};
        // Later steps it served, through one another, but never the finish:
        // the finish is the end being re-planned toward.
        const auto finish = plot.plan.empty() ? 0 : plot.plan.size() - 1;
        for (std::size_t i = plot.cursor + 1; i < finish; ++i) {
            const auto& later = plot.plan[i];
            if (std::any_of(route.begin(), route.end(), [&later](const Step* s) { return Serves(*s, later); })) {
                route.push_back(&later);
            }
        }
        std::vector<PlotModel::Abandoned> out;
        for (const auto* step : route) {
            const auto target = TargetOf(*step);
            if (target == 0) {
                continue;
            }
            const auto* verb = PlotVerbs::Find(step->verb);
            const auto* who = verb != nullptr ? step->FindSlot(TargetSlot(*verb)) : nullptr;
            out.push_back({step->verb, target, who != nullptr ? who->text : std::string{}});
        }
        return out;
    }

    Adaptation Adapt(PlotModel::Plot& plot, int cap, const Ask& ask, const Replanner& replan)
    {
        // Checked before asking. Whatever the model would say, a plot that
        // has spent its attempts stops, which is what makes "it would not
        // concede; the cap did" an outcome of its own.
        if (plot.adaptations >= cap || plot.history.empty()) {
            return {Revision::CapHit, "the adaptation cap was reached", 0, 0, {}};
        }
        ++plot.adaptations;
        const Step failed = plot.history.back();

        nlohmann::json options = nlohmann::json::array();
        options.push_back({{"key", kSubstitute},
                           {"line",
                            "Find another way to the same end, leaving behind what failed and everything it was "
                            "there to make possible."}});
        options.push_back({{"key", kConcede}, {"line", "Give the scheme up."}});

        std::vector<const Step*> history;
        for (const auto& past : plot.history) {
            history.push_back(&past);
        }
        nlohmann::json ctx;
        ctx["mastermind"] = {{"name", plot.mastermindName}};
        ctx["ambition"] = plot.ambition;
        ctx["scheme"] = plot.scheme;
        ctx["steps"] = StepsSoFar(history);
        ctx["failed"] = std::format("{} ({})", PlotModel::Label(failed), OutcomePhrase(failed.outcome));
        ctx["attempt"] = plot.adaptations;
        ctx["max_attempts"] = cap;
        ctx["options"] = std::move(options);

        const auto result = ask(kAdaptTemplate, ctx);
        if (!result.ok) {
            return {Revision::Conceded, std::format("the decision failed ({})", result.error), 0, 0, {}};
        }
        const auto key = result.Choice(kAdaptQuestion);

        if (key == kSubstitute) {
            // Abandoned before planning, so the plan cannot reach for it.
            std::vector<PlotModel::Abandoned> abandoned;
            for (auto& way : RouteOf(plot)) {
                const bool known =
                    std::any_of(plot.abandoned.begin(), plot.abandoned.end(), [&way](const PlotModel::Abandoned& a) {
                        return a.verb == way.verb && a.target == way.target;
                    });
                if (!known) {
                    plot.abandoned.push_back(way);
                    abandoned.push_back(std::move(way));
                }
            }
            auto tail = replan(plot);
            if (!tail.ok) {
                return {Revision::Conceded,
                        std::format("no other way could be planned ({})",
                                    !tail.error.empty() ? tail.error : tail.rejection),
                        0,
                        tail.decisions,
                        std::move(abandoned)};
            }
            plot.plan.erase(plot.plan.begin() + static_cast<std::ptrdiff_t>(plot.cursor), plot.plan.end());
            const auto added = tail.plan.size();
            for (auto& step : tail.plan) {
                plot.plan.push_back(std::move(step));
            }
            return {Revision::Substituted, {}, added, tail.decisions, std::move(abandoned)};
        }
        if (key == kConcede) {
            return {Revision::Conceded, "they chose to give it up", 0, 0, {}};
        }
        return {Revision::Conceded, std::format("answered '{}', which was not offered", key), 0, 0, {}};
    }

    RE::FormID ItemHolder(const PlotModel::Plot& plot, const Step& step)
    {
        const auto* wanted = step.FindSlot("item");
        for (auto it = plot.history.rbegin(); it != plot.history.rend(); ++it) {
            const auto* verb = PlotVerbs::Find(it->verb);
            if (verb == nullptr || !verb->obtainsItem || it->outcome != PlotModel::StepOutcome::Succeeded) {
                continue;
            }
            const auto* got = it->FindSlot("item");
            if (wanted == nullptr || (got != nullptr && got->form == wanted->form)) {
                return it->actor;
            }
        }
        return 0;
    }

    RE::FormID PickAgent(const PlotModel::Plot& plot,
                         const Step& step,
                         const std::vector<AgentOption>& options,
                         const Ask& ask,
                         std::string& error)
    {
        if (options.empty()) {
            return 0;
        }
        if (options.size() == 1) {
            return options.front().npc;
        }

        nlohmann::json list = nlohmann::json::array();
        for (std::size_t i = 0; i < options.size(); ++i) {
            list.push_back({{"key", std::format("c{}", i + 1)}, {"line", options[i].line}});
        }
        nlohmann::json ctx;
        ctx["mastermind"] = {{"name", plot.mastermindName}};
        ctx["scheme"] = plot.scheme;
        ctx["finish"] = plot.finish;
        ctx["step"] = Sketch(step, true);
        ctx["purpose"] = "Whoever is chosen carries this step out.";
        ctx["slot"] = "agent";
        ctx["kind"] = "person";
        ctx["options"] = std::move(list);

        const auto result = ask(kSlotTemplate, ctx);
        if (!result.ok) {
            error = result.error;
            return options.front().npc;
        }
        const auto key = result.Choice(kSlotQuestion);
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (key == std::format("c{}", i + 1)) {
                return options[i].npc;
            }
        }
        error = std::format("answered '{}', which was not offered", key);
        return options.front().npc;
    }

    Outcome Plan(const Input& input, const Ask& ask)
    {
        return Planner(input, ask).Run();
    }
} // namespace NarrativeEngine::PlotPlanning
