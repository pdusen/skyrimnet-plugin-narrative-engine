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
                : input_(input), ask_(ask), naming_(input.scheme + "\n" + input.finish)
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
                    if (FindIn(preset, slot.name) != nullptr) {
                        continue;
                    }
                    if (slot.rule == PlotVerbs::Rule::BoundItem) {
                        if (input_.world.items.empty()
                            && !PlotCandidates::Bind(slot, RequestOf(preset, earlier, excluded))) {
                            return false;
                        }
                        continue;
                    }
                    if (DependsOnAnotherSlot(slot) && FindIn(preset, "mark") == nullptr) {
                        continue;
                    }
                    if (PlotCandidates::Build(slot, RequestOf(preset, earlier, excluded)).empty()) {
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
                        fromPool.rule = PlotVerbs::Rule::ItemPool;
                        list = PlotCandidates::Build(fromPool, request);
                    } else {
                        list = PlotCandidates::Build(slot, request);
                    }

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
                const auto excluded = PeopleOf(needing);

                std::vector<const PlotVerbs::Verb*> ways;
                for (const auto* id : {"steal", "buy"}) {
                    const auto* verb = PlotVerbs::Find(id);
                    if (verb != nullptr && Fillable(*verb, preset, excluded, index)) {
                        ways.push_back(verb);
                    }
                }
                if (ways.empty()) {
                    rejection_ = std::format("nobody to get {} from for {}", item->text, needing.verb);
                    return Fill::Unfillable;
                }

                const PlotVerbs::Verb* way = ways.front();
                if (ways.size() > 1) {
                    nlohmann::json options = nlohmann::json::array();
                    for (const auto* verb : ways) {
                        options.push_back({{"key", verb->id}, {"line", Sketch(Partial(*verb, preset))}});
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
                    for (const auto* verb : ways) {
                        if (verb->id == key) {
                            way = verb;
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
                const auto filled = FillSlots(*way, preset, excluded, index, purpose, step);
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
                std::vector<const PlotVerbs::Verb*> offered;
                nlohmann::json options = nlohmann::json::array();
                for (const auto& verb : PlotVerbs::kVerbs) {
                    if (HasItemSlot(verb) && input_.maxSteps < kPlanFloor) {
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
                            if (!prep->slot.empty()) {
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
                    const Option* chosen = nullptr;
                    for (std::size_t i = 0; i < offered.size(); ++i) {
                        if (key == std::format("p{}", i + 1)) {
                            chosen = &offered[i];
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

    bool CanRetry(const Step& failed)
    {
        const auto* verb = PlotVerbs::Find(failed.verb);
        return verb != nullptr && !verb->agentBoundToItem;
    }

    Step Retried(const Step& failed)
    {
        Step step;
        step.type = failed.type;
        step.verb = failed.verb;
        step.slots = failed.slots;
        step.target = failed.target;
        step.targetName = failed.targetName;
        return step;
    }

    std::unordered_set<RE::FormID> FailedAgents(const PlotModel::Plot& plot, const Step& step)
    {
        std::unordered_set<RE::FormID> out;
        for (const auto& past : plot.history) {
            if (past.actor != 0 && past.outcome != PlotModel::StepOutcome::Succeeded && SameStep(past, step)) {
                out.insert(past.actor);
            }
        }
        return out;
    }

    Adaptation Adapt(PlotModel::Plot& plot, int cap, const Ask& ask, const Replanner& replan)
    {
        // Checked before asking. Whatever the model would say, a plot that
        // has spent its attempts stops, which is what makes "it would not
        // concede; the cap did" an outcome of its own.
        if (plot.adaptations >= cap || plot.history.empty()) {
            return {Revision::CapHit, "the adaptation cap was reached", 0, 0};
        }
        ++plot.adaptations;
        const Step failed = plot.history.back();

        nlohmann::json options = nlohmann::json::array();
        const bool retryable = CanRetry(failed) && plot.cursor < plot.plan.size();
        if (retryable) {
            options.push_back(
                {{"key", kRetry}, {"line", "Try the same step again, with somebody else carrying it out."}});
        }
        options.push_back(
            {{"key", kSubstitute}, {"line", "Try a different way, and plan the rest of the scheme again from here."}});
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
            return {Revision::Conceded, std::format("the decision failed ({})", result.error), 0, 0};
        }
        const auto key = result.Choice(kAdaptQuestion);

        if (key == kRetry && retryable) {
            plot.plan[plot.cursor] = Retried(failed);
            return {Revision::Retried, {}, 1, 0};
        }
        if (key == kSubstitute) {
            auto tail = replan(plot);
            if (!tail.ok) {
                return {Revision::Conceded,
                        std::format("no other way could be planned ({})",
                                    !tail.error.empty() ? tail.error : tail.rejection),
                        0,
                        tail.decisions};
            }
            plot.plan.erase(plot.plan.begin() + static_cast<std::ptrdiff_t>(plot.cursor), plot.plan.end());
            const auto added = tail.plan.size();
            for (auto& step : tail.plan) {
                plot.plan.push_back(std::move(step));
            }
            return {Revision::Substituted, {}, added, tail.decisions};
        }
        if (key == kConcede) {
            return {Revision::Conceded, "they chose to give it up", 0, 0};
        }
        return {Revision::Conceded, std::format("answered '{}', which was not offered", key), 0, 0};
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
